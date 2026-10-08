#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderHttpTransport.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include "Infrastructure/TestSupport.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <span>
#include <stop_token>
#include <string_view>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")
namespace {
using namespace std::chrono_literals;
using namespace ForgeConductor::Tests;
namespace Domain = ForgeConductor::Domain;
using Service = ForgeConductor::NativeTools::Windows::WindowsImageProviderHttpTransport;
struct HttpRequest final {
    std::string method;
    std::string path;
    std::string body;
    std::string headers;
};

struct ResponseScript final {
    std::string expectedMethod;
    std::string expectedPath;
    unsigned statusCode{200U};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds delay{};
    bool blockUntilReleased{};
    bool allowClientDisconnect{};
    bool delayBodyOnly{};
    std::string contentType{"application/json"};
    std::optional<std::string> declaredLength;
    bool omitContentLength{};
};

[[nodiscard]] std::string lowercase(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            if (character >= 'A' && character <= 'Z') {
                return static_cast<char>(character - 'A' + 'a');
            }
            return static_cast<char>(character);
        });
    return value;
}

[[nodiscard]] std::string_view trimAscii(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

class LoopbackHttpServer final {
public:
    explicit LoopbackHttpServer(std::vector<ResponseScript> scripts)
        : scripts_{std::move(scripts)}
    {
        WSADATA data{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup != 0) {
            throw std::runtime_error{
                "WSAStartup failed: " + std::to_string(startup)};
        }
        winsockStarted_ = true;
        try {
            const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET) {
                throwSocketError("socket");
            }
            listenSocket_.store(listener, std::memory_order_release);
            const BOOL exclusive = TRUE;
            if (setsockopt(
                    listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive),
                    sizeof(exclusive)) == SOCKET_ERROR) {
                throwSocketError("setsockopt(SO_EXCLUSIVEADDRUSE)");
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0U;
            if (bind(
                    listener, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == SOCKET_ERROR) {
                throwSocketError("bind");
            }
            if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
                throwSocketError("listen");
            }
            int addressBytes = sizeof(address);
            if (getsockname(
                    listener, reinterpret_cast<sockaddr*>(&address),
                    &addressBytes) == SOCKET_ERROR) {
                throwSocketError("getsockname");
            }
            port_ = ntohs(address.sin_port);
            worker_ = std::thread{[this]() noexcept { run(); }};
        } catch (...) {
            stop();
            throw;
        }
    }

    ~LoopbackHttpServer() noexcept { stop(); }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] bool waitForRequests(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{stateMutex_};
        return stateChanged_.wait_for(lock, timeout, [&]() noexcept {
            return requests_.size() >= count || !failure_.empty();
        }) && failure_.empty() && requests_.size() >= count;
    }

    [[nodiscard]] bool waitUntilHandled(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{stateMutex_};
        return stateChanged_.wait_for(lock, timeout, [&]() noexcept {
            return handledRequests_ >= count || !failure_.empty();
        }) && failure_.empty() && handledRequests_ >= count;
    }

    [[nodiscard]] std::vector<HttpRequest> requests() const
    {
        std::lock_guard lock{stateMutex_};
        return requests_;
    }

    [[nodiscard]] bool waitForBodyResponses(const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{stateMutex_};
        return stateChanged_.wait_for(lock, timeout, [&] {
            return startedBodyResponses_ >= count || !failure_.empty();
        }) && failure_.empty() && startedBodyResponses_ >= count;
    }

    void releaseBlockedResponses() noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            releaseResponses_ = true;
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    void requireHealthy() const
    {
        std::lock_guard lock{stateMutex_};
        if (!failure_.empty()) {
            throw std::runtime_error{"Loopback HTTP fixture failed: " + failure_};
        }
    }

private:
    static constexpr std::size_t MaximumCapturedRequestBytes = 512U * 1024U;

    [[noreturn]] static void throwSocketError(const std::string_view action)
    {
        throw std::runtime_error{
            std::string{action} + " failed: " +
            std::to_string(WSAGetLastError())};
    }

    [[nodiscard]] static HttpRequest readRequest(const SOCKET client)
    {
        std::string encoded;
        encoded.reserve(8U * 1024U);
        std::optional<std::size_t> headerEnd;
        std::size_t contentLength{};
        std::array<char, 4096U> buffer{};
        while (true) {
            const int received = recv(
                client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received == 0) {
                throw std::runtime_error{
                    "client disconnected before sending a complete request"};
            }
            if (received == SOCKET_ERROR) {
                throwSocketError("recv");
            }
            encoded.append(buffer.data(), static_cast<std::size_t>(received));
            if (encoded.size() > MaximumCapturedRequestBytes) {
                throw std::runtime_error{"request exceeded fixture capture bound"};
            }
            if (!headerEnd) {
                const auto marker = encoded.find("\r\n\r\n");
                if (marker != std::string::npos) {
                    headerEnd = marker + 4U;
                    contentLength = parseContentLength(
                        std::string_view{encoded}.substr(0U, marker));
                }
            }
            if (headerEnd && encoded.size() >= *headerEnd + contentLength) {
                break;
            }
        }

        const auto lineEnd = encoded.find("\r\n");
        if (lineEnd == std::string::npos) {
            throw std::runtime_error{"request line is missing"};
        }
        const std::string_view line{encoded.data(), lineEnd};
        const auto firstSpace = line.find(' ');
        const auto secondSpace = firstSpace == std::string_view::npos
            ? std::string_view::npos
            : line.find(' ', firstSpace + 1U);
        if (firstSpace == std::string_view::npos ||
            secondSpace == std::string_view::npos) {
            throw std::runtime_error{"request line is malformed"};
        }
        return HttpRequest{
            std::string{line.substr(0U, firstSpace)},
            std::string{line.substr(
                firstSpace + 1U, secondSpace - firstSpace - 1U)},
            encoded.substr(*headerEnd, contentLength),
            encoded.substr(0U, *headerEnd)};
    }

    [[nodiscard]] static std::size_t parseContentLength(
        const std::string_view headers)
    {
        std::size_t cursor{};
        while (cursor < headers.size()) {
            const auto end = headers.find("\r\n", cursor);
            const auto lineEnd = end == std::string_view::npos
                ? headers.size()
                : end;
            const auto line = headers.substr(cursor, lineEnd - cursor);
            const auto separator = line.find(':');
            if (separator != std::string_view::npos &&
                lowercase(std::string{line.substr(0U, separator)}) ==
                    "content-length") {
                const auto value = trimAscii(line.substr(separator + 1U));
                std::size_t parsed{};
                const auto [last, error] = std::from_chars(
                    value.data(), value.data() + value.size(), parsed);
                if (error != std::errc{} || last != value.data() + value.size()) {
                    throw std::runtime_error{"Content-Length is invalid"};
                }
                return parsed;
            }
            if (end == std::string_view::npos) {
                break;
            }
            cursor = end + 2U;
        }
        return 0U;
    }

    [[nodiscard]] static std::string reasonPhrase(const unsigned status)
    {
        switch (status) {
        case 200U:
            return "OK";
        case 201U:
            return "Created";
        case 404U:
            return "Not Found";
        case 429U:
            return "Too Many Requests";
        default:
            return "Scripted";
        }
    }

    [[nodiscard]] static bool sendAll(
        const SOCKET client,
        const std::string_view bytes) noexcept
    {
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const int requested = static_cast<int>(std::min<std::size_t>(
                remaining, 16U * 1024U));
            const int sent = send(client, bytes.data() + offset, requested, 0);
            if (sent == SOCKET_ERROR || sent == 0) {
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    [[nodiscard]] bool sendResponse(
        const SOCKET client,
        const ResponseScript& script) noexcept
    {
        try {
            std::string headers =
                "HTTP/1.1 " + std::to_string(script.statusCode) + " " +
                reasonPhrase(script.statusCode) + "\r\n" +
                "Content-Type: " + script.contentType + "\r\n";
            if (!script.omitContentLength) {
                headers += "Content-Length: " + script.declaredLength.value_or(std::to_string(script.body.size())) + "\r\n";
            }
            headers += "Connection: close\r\n";
            for (const auto& [name, value] : script.headers) {
                headers += name + ": " + value + "\r\n";
            }
            headers += "\r\n";
            if (script.delayBodyOnly) {
                const auto prefixBytes = std::min<std::size_t>(8192U, script.body.size());
                if (!sendAll(client, headers) || !sendAll(client, std::string_view{script.body}.substr(0U, prefixBytes)))
                    return false;
                {
                    std::unique_lock lock{stateMutex_};
                    ++startedBodyResponses_;
                    stateChanged_.notify_all();
                    if (script.blockUntilReleased)
                        stateChanged_.wait(lock, [&] { return releaseResponses_ || stopping_; });
                    else
                        static_cast<void>(stateChanged_.wait_for(lock, script.delay, [&] { return stopping_; }));
                    if (stopping_) return false;
                }
                return sendAll(client, std::string_view{script.body}.substr(prefixBytes));
            }
            return sendAll(client, headers) && sendAll(client, script.body);
        } catch (...) {
            return false;
        }
    }

    void setFailure(std::string message) noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            if (stopping_) {
                stateChanged_.notify_all();
                return;
            }
            if (failure_.empty()) {
                failure_ = std::move(message);
            }
            stopping_ = true;
            releaseResponses_ = true;
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    void run() noexcept
    {
        try {
            for (std::size_t index = 0U; index < scripts_.size(); ++index) {
                const SOCKET listener =
                    listenSocket_.load(std::memory_order_acquire);
                if (listener == INVALID_SOCKET) {
                    break;
                }
                const SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET) {
                    std::lock_guard lock{stateMutex_};
                    if (stopping_) {
                        break;
                    }
                    throwSocketError("accept");
                }
                activeClient_.store(client, std::memory_order_release);

                try {
                    auto request = readRequest(client);
                    const auto& script = scripts_[index];
                    if (request.method != script.expectedMethod ||
                        request.path != script.expectedPath) {
                        throw std::runtime_error{
                            "unexpected " + request.method + " " + request.path};
                    }
                    {
                        std::lock_guard lock{stateMutex_};
                        requests_.push_back(std::move(request));
                        stateChanged_.notify_all();
                    }
                    if (script.blockUntilReleased && !script.delayBodyOnly) {
                        std::unique_lock lock{stateMutex_};
                        stateChanged_.wait(lock, [&]() noexcept {
                            return releaseResponses_ || stopping_;
                        });
                    } else if (script.delay > 0ms && !script.delayBodyOnly) {
                        std::unique_lock lock{stateMutex_};
                        static_cast<void>(stateChanged_.wait_for(
                            lock, script.delay,
                            [&]() noexcept { return stopping_; }));
                    }

                    bool stopping{};
                    {
                        std::lock_guard lock{stateMutex_};
                        stopping = stopping_;
                    }
                    if (!stopping) {
                        const bool sent = sendResponse(client, script);
                        if (!sent && !script.allowClientDisconnect) {
                            throw std::runtime_error{"response send failed"};
                        }
                    }
                    closeClient(client);
                    {
                        std::lock_guard lock{stateMutex_};
                        ++handledRequests_;
                        stateChanged_.notify_all();
                    }
                } catch (...) {
                    closeClient(client);
                    throw;
                }
            }
        } catch (const std::exception& error) {
            setFailure(error.what());
        } catch (...) {
            setFailure("unknown server failure");
        }
    }

    void stop() noexcept
    {
        try {
            {
                std::lock_guard lock{stateMutex_};
                stopping_ = true;
                releaseResponses_ = true;
                stateChanged_.notify_all();
            }
            const SOCKET listener =
                listenSocket_.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
            if (listener != INVALID_SOCKET) {
                static_cast<void>(shutdown(listener, SD_BOTH));
                closesocket(listener);
            }
            const SOCKET client =
                activeClient_.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
            if (client != INVALID_SOCKET) {
                static_cast<void>(shutdown(client, SD_BOTH));
                closesocket(client);
            }
            if (worker_.joinable()) {
                worker_.join();
            }
            if (winsockStarted_) {
                WSACleanup();
                winsockStarted_ = false;
            }
        } catch (...) {
        }
    }

    void closeClient(const SOCKET client) noexcept
    {
        SOCKET expected = client;
        if (activeClient_.compare_exchange_strong(
                expected, INVALID_SOCKET, std::memory_order_acq_rel)) {
            static_cast<void>(shutdown(client, SD_BOTH));
            closesocket(client);
        }
    }

    const std::vector<ResponseScript> scripts_;
    std::atomic<SOCKET> listenSocket_{INVALID_SOCKET};
    std::atomic<SOCKET> activeClient_{INVALID_SOCKET};
    std::uint16_t port_{};
    std::thread worker_;
    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::vector<HttpRequest> requests_;
    std::size_t handledRequests_{};
    std::size_t startedBodyResponses_{};
    std::string failure_;
    bool releaseResponses_{};
    bool stopping_{};
    bool winsockStarted_{};
};

[[nodiscard]] Domain::ImageProviderConfig provider(const LoopbackHttpServer& server)
{
    return {true, "http://127.0.0.1:" + std::to_string(server.port()), "sd1", "cyberrealistic_final.safetensors"};
}

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value) noexcept
{
    return std::as_bytes(std::span{value.data(), value.size()});
}

[[nodiscard]] std::string bodyText(const ForgeConductor::Contracts::ImageProviderHttpResponse& response)
{
    if (response.body.empty()) return {};
    return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

void bigEndian(std::string& output, std::uint32_t value)
{
    for (unsigned shift = 24U;; shift -= 8U) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
        if (shift == 0U) break;
    }
}

[[nodiscard]] std::uint32_t crc32(std::string_view data)
{
    std::uint32_t value = 0xffffffffU;
    for (const unsigned char byte : data) {
        value ^= byte;
        for (unsigned bit{}; bit < 8U; ++bit) value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
    }
    return value ^ 0xffffffffU;
}

void pngChunk(std::string& output, std::string_view type, std::string_view data)
{
    bigEndian(output, static_cast<std::uint32_t>(data.size()));
    const auto start = output.size(); output.append(type); output.append(data);
    bigEndian(output, crc32(std::string_view{output}.substr(start)));
}

// A deterministic RGBA PNG with stored zlib blocks. The fixture contains NUL
// and non-UTF-8 bytes and exceeds the generic web tool's 48 KiB response limit.
[[nodiscard]] std::string pngBody()
{
    std::string raw;
    for (unsigned y{}; y < 96U; ++y) {
        raw.push_back('\0');
        for (unsigned x{}; x < 256U; ++x) {
            raw.push_back(static_cast<char>(x)); raw.push_back(static_cast<char>(y));
            raw.push_back(static_cast<char>(x ^ y)); raw.push_back(static_cast<char>(0xff));
        }
    }
    std::string compressed{"\x78\x01", 2U};
    std::size_t offset{};
    while (offset < raw.size()) {
        const auto count = static_cast<std::uint16_t>((std::min)(raw.size() - offset, std::size_t{65535U}));
        const auto inverse = static_cast<std::uint16_t>(~count);
        compressed.push_back(offset + count == raw.size() ? '\x01' : '\0');
        compressed.push_back(static_cast<char>(count & 0xffU)); compressed.push_back(static_cast<char>(count >> 8U));
        compressed.push_back(static_cast<char>(inverse & 0xffU)); compressed.push_back(static_cast<char>(inverse >> 8U));
        compressed.append(raw, offset, count); offset += count;
    }
    std::uint32_t a = 1U, b{};
    for (const unsigned char value : raw) { a = (a + value) % 65521U; b = (b + a) % 65521U; }
    bigEndian(compressed, (b << 16U) | a);
    std::string output{"\x89PNG\r\n\x1a\n", 8U};
    std::string header; bigEndian(header, 256U); bigEndian(header, 96U);
    header.append("\x08\x06\0\0\0", 5U);
    pngChunk(output, "IHDR", header); pngChunk(output, "IDAT", compressed); pngChunk(output, "IEND", {});
    return output;
}

void binaryMultipartAndLargePngPreserveEveryByte()
{
    const auto png = pngBody();
    require(png.size() > 48U * 1024U && png.find('\0') != std::string::npos && png.find('\xff') != std::string::npos,
        "The binary PNG fixture does not exercise the required byte and size boundaries.");
    ResponseScript script{"POST", "/upload/image", 200U, png}; script.contentType = "image/png";
    LoopbackHttpServer server{{std::move(script)}};
    const std::string contentType = "multipart/form-data; boundary=forge-owned-http-test";
    const std::string prefix = "--forge-owned-http-test\r\nContent-Disposition: form-data; name=\"image\"; filename=\"source.png\"\r\nContent-Type: image/png\r\n\r\n";
    const auto multipart = prefix + png + "\r\n--forge-owned-http-test--\r\n";
    Service transport;
    auto response = take(transport.request(provider(server), "POST", "/upload/image", contentType, bytes(multipart), png.size(), TestContext{}.active()));
    require(response.status == 200U && response.contentType == "image/png" && bodyText(response) == png,
        "Native binary transport truncated, recoded or altered the PNG response.");
    require(server.waitUntilHandled(1U, 2s), "The binary response fixture did not finish.");
    const auto requests = server.requests();
    require(requests.size() == 1U && requests.front().body == multipart,
        "Binary multipart request bytes were changed, truncated or replayed.");
    require(requests.front().headers.find("Content-Type: " + contentType) != std::string::npos,
        "Multipart content type and boundary were changed.");
    require(requests.front().headers.find("Forge-Conductor/" + std::string{Domain::ProductVersion}) != std::string::npos,
        "Native transport User-Agent does not use the current product version.");
    const auto headers = lowercase(requests.front().headers);
    require(headers.find("cookie:") == std::string::npos && headers.find("authorization:") == std::string::npos &&
        headers.find("proxy-authorization:") == std::string::npos && headers.find("referer:") == std::string::npos,
        "Ambient cookie, authentication, proxy authentication or referer was sent.");
    server.requireHealthy();
}

void authenticationChallengeAndCookiesDoNotReplay()
{
    ResponseScript challenge{"GET", "/challenge", 401U, "authentication required"};
    challenge.headers = {{"WWW-Authenticate", "Negotiate"}, {"WWW-Authenticate", "NTLM"}, {"Set-Cookie", "forge_ambient=private; Path=/"}};
    LoopbackHttpServer server{{std::move(challenge), {"GET", "/after", 200U, "observed"}}};
    Service transport;
    const auto first = take(transport.request(provider(server), "GET", "/challenge", {}, {}, 1024U, TestContext{}.active()));
    require(first.status == 401U && bodyText(first) == "authentication required", "Authentication challenge was hidden or retried.");
    const auto second = take(transport.request(provider(server), "GET", "/after", {}, {}, 1024U, TestContext{}.active()));
    require(second.status == 200U && bodyText(second) == "observed", "Request after challenge failed.");
    require(server.waitUntilHandled(2U, 2s), "Challenge fixture did not finish.");
    const auto requests = server.requests(); require(requests.size() == 2U, "Authentication challenge caused an extra request.");
    for (const auto& request : requests) {
        const auto headers = lowercase(request.headers);
        require(headers.find("authorization:") == std::string::npos && headers.find("cookie:") == std::string::npos &&
            headers.find("proxy-authorization:") == std::string::npos,
            "Authentication challenge or Set-Cookie introduced ambient credentials.");
    }
    server.requireHealthy();
}

void redirectsDoNotFollowOrReplayPost()
{
    LoopbackHttpServer target{{{"GET", "/foreign", 200U, "must not be requested"}}};
    const auto location = provider(target).endpoint + "/foreign";
    ResponseScript get{"GET", "/get", 302U, "redirect observed"}; get.headers = {{"Location", location}};
    ResponseScript post{"POST", "/post", 307U, "post redirect observed"}; post.headers = {{"Location", location}};
    LoopbackHttpServer origin{{std::move(get), std::move(post)}};
    Service transport;
    const auto first = take(transport.request(provider(origin), "GET", "/get", {}, {}, 1024U, TestContext{}.active()));
    const auto second = take(transport.request(provider(origin), "POST", "/post", "application/json", bytes("{}"), 1024U, TestContext{}.active()));
    require(first.status == 302U && bodyText(first) == "redirect observed" && second.status == 307U && bodyText(second) == "post redirect observed",
        "A redirect was followed or its actual response was hidden.");
    require(origin.waitUntilHandled(2U, 2s) && origin.requests().size() == 2U, "Redirect request was replayed.");
    require(!target.waitForRequests(1U, 200ms) && target.requests().empty(), "A redirect contacted the second endpoint.");
    origin.requireHealthy(); target.requireHealthy();
}

void responseBoundsRequireCompleteEof()
{
    const std::string exact(8192U, '\0');
    ResponseScript bounded{"GET", "/exact", 200U, exact}; bounded.omitContentLength = true;
    ResponseScript overflow{"GET", "/overflow", 200U, exact + "x"}; overflow.omitContentLength = true; overflow.allowClientDisconnect = true;
    ResponseScript declared{"GET", "/declared-overflow", 200U, exact + "x"}; declared.allowClientDisconnect = true;
    LoopbackHttpServer server{{std::move(bounded), std::move(overflow), std::move(declared)}};
    Service transport;
    const auto response = take(transport.request(provider(server), "GET", "/exact", {}, {}, exact.size(), TestContext{}.active()));
    require(bodyText(response) == exact, "An exact-bound response was rejected or truncated before EOF.");
    requireError(transport.request(provider(server), "GET", "/overflow", {}, {}, exact.size(), TestContext{}.active()),
        Domain::ErrorCodes::PayloadTooLarge, "An undeclared oversized response returned partial success.");
    requireError(transport.request(provider(server), "GET", "/declared-overflow", {}, {}, exact.size(), TestContext{}.active()),
        Domain::ErrorCodes::PayloadTooLarge, "A declared oversized response returned partial success.");
    require(server.waitUntilHandled(3U, 2s) && server.requests().size() == 3U, "Bounded response requests were replayed.");
    server.requireHealthy();
}

void actual400AndMalformedOrIncompleteResponses()
{
    const std::string validation = R"({"error":{"type":"invalid_prompt","message":"fixture validation failed"}})";
    ResponseScript badLength{"GET", "/bad-length", 200U, "abc"}; badLength.declaredLength = "3-invalid"; badLength.allowClientDisconnect = true;
    ResponseScript incomplete{"GET", "/incomplete", 200U, "abc"}; incomplete.declaredLength = "12";
    LoopbackHttpServer server{{{"POST", "/prompt", 400U, validation}, std::move(badLength), std::move(incomplete)}};
    Service transport;
    const auto invalid = take(transport.request(provider(server), "POST", "/prompt", "application/json", bytes("{}"), 1024U, TestContext{}.active()));
    require(invalid.status == 400U && bodyText(invalid) == validation, "Provider validation status or error body was lost.");
    for (const auto route : {"/bad-length", "/incomplete"}) {
        const auto result = transport.request(provider(server), "GET", route, {}, {}, 1024U, TestContext{}.active());
        require(!result, "Malformed or incomplete provider response returned bytes as success.");
        require(result.error().code == Domain::ErrorCodes::MalformedMessage || result.error().code == Domain::ErrorCodes::HostCapabilityUnavailable,
            "Malformed response was converted to an unrelated error instead of native/parser rejection.");
    }
    require(server.waitUntilHandled(3U, 2s), "Malformed-response fixture did not finish."); server.requireHealthy();
}

void cancellationWhileResponseIsPendingReturnsNoBytes()
{
    ResponseScript blocked{"GET", "/pending", 200U, std::string(32768U, '\xff')};
    blocked.blockUntilReleased = true; blocked.delayBodyOnly = true; blocked.allowClientDisconnect = true;
    LoopbackHttpServer server{{std::move(blocked)}};
    Service transport; TestContext context;
    auto active = context.active(); active.deadline = std::chrono::steady_clock::now() + 10s;
    auto pending = std::async(std::launch::async, [&] { return transport.request(provider(server), "GET", "/pending", {}, {}, 65536U, active); });
    require(server.waitForBodyResponses(1U, 2s), "Cancellation did not reach the blocked body receive phase.");
    context.cancellation.request_stop();
    require(pending.wait_for(3s) == std::future_status::ready, "Cancelling a pending native read did not return promptly.");
    requireError(pending.get(), Domain::ErrorCodes::Cancelled, "Pending cancellation returned a partial image or wrong error.");
    server.releaseBlockedResponses(); require(server.waitUntilHandled(1U, 2s), "Cancelled response fixture did not finish."); server.requireHealthy();
}

void shortDeadlineDuringPendingBodyReturnsNoBytes()
{
    ResponseScript blocked{"GET", "/deadline", 200U, std::string(32768U, '\xff')};
    blocked.blockUntilReleased = true; blocked.delayBodyOnly = true; blocked.allowClientDisconnect = true;
    LoopbackHttpServer server{{std::move(blocked)}};
    Service transport; TestContext context;
    auto active = context.active(); active.deadline = std::chrono::steady_clock::now() + 1s;
    const auto start = std::chrono::steady_clock::now();
    auto pending = std::async(std::launch::async, [&] { return transport.request(provider(server), "GET", "/deadline", {}, {}, 65536U, active); });
    require(server.waitForBodyResponses(1U, 750ms), "Short deadline did not reach a blocked native body receive.");
    require(pending.wait_for(3s) == std::future_status::ready, "Native caller deadline did not end a pending body receive.");
    requireError(pending.get(), Domain::ErrorCodes::DeadlineExceeded, "Expired receive returned a partial image or wrong error.");
    require(std::chrono::steady_clock::now() - start < 4s, "Caller deadline was replaced with a longer transport timeout.");
    server.releaseBlockedResponses(); require(server.waitUntilHandled(1U, 2s), "Deadline response fixture did not finish."); server.requireHealthy();
}

void invalidInputsAndAlreadyStoppedContextsNeverSendHttp()
{
    LoopbackHttpServer server{{{"GET", "/must-not-hit", 200U, "unexpected"}}};
    Service transport; const auto configured = provider(server);
    for (const auto endpoint : {"", "http://localhost:8188", "http://127.0.0.1:0", "http://127.0.0.1:65536", "http://user:pass@127.0.0.1:8188", "http://127.0.0.1:8188/extra", "https://example.com:443"}) {
        auto invalid = configured; invalid.endpoint = endpoint;
        requireError(transport.request(invalid, "GET", "/must-not-hit", {}, {}, 1024U, TestContext{}.active()),
            Domain::ErrorCodes::InvalidRequest, "Invalid provider endpoint reached HTTP.");
    }
    for (const auto route : {"", "relative", "/has space", "/has#fragment", "/has\\backslash", "/has\r\nInjected: value"}) {
        requireError(transport.request(configured, "GET", route, {}, {}, 1024U, TestContext{}.active()),
            Domain::ErrorCodes::InvalidRequest, "Invalid image-provider route reached HTTP.");
    }
    const std::string longRoute = "/" + std::string(4096U, 'x');
    requireError(transport.request(configured, "GET", longRoute, {}, {}, 1024U, TestContext{}.active()),
        Domain::ErrorCodes::InvalidRequest, "Oversized image-provider route reached HTTP.");
    for (const auto method : {"PUT", "DELETE", "HEAD", "get", "POST\r\nInjected: value"}) {
        requireError(transport.request(configured, method, "/must-not-hit", {}, {}, 1024U, TestContext{}.active()),
            Domain::ErrorCodes::InvalidRequest, "Unsupported method reached HTTP.");
    }
    const std::string oversized(16U * 1024U * 1024U + 4097U, '\0');
    requireError(transport.request(configured, "POST", "/must-not-hit", "image/png", bytes(oversized), 1024U, TestContext{}.active()),
        Domain::ErrorCodes::InvalidRequest, "Oversized upload reached HTTP.");
    requireError(transport.request(configured, "GET", "/must-not-hit", {}, bytes("x"), 1024U, TestContext{}.active()),
        Domain::ErrorCodes::InvalidRequest, "GET request body reached HTTP.");
    for (const auto maximum : {std::size_t{0U}, std::size_t{16U * 1024U * 1024U + 1U}}) {
        requireError(transport.request(configured, "GET", "/must-not-hit", {}, {}, maximum, TestContext{}.active()),
            Domain::ErrorCodes::InvalidRequest, "Invalid response limit reached HTTP.");
    }
    for (const auto& contentType : {std::string{"image/png\r\nAuthorization: injected"}, std::string(257U, 'x')}) {
        requireError(transport.request(configured, "POST", "/must-not-hit", contentType, {}, 1024U, TestContext{}.active()),
            Domain::ErrorCodes::InvalidRequest, "Invalid content type reached HTTP.");
    }
    TestContext cancelled; cancelled.cancellation.request_stop();
    requireError(transport.request(configured, "GET", "/must-not-hit", {}, {}, 1024U, cancelled.active()),
        Domain::ErrorCodes::Cancelled, "Already cancelled transport attempted HTTP.");
    TestContext expired;
    requireError(transport.request(configured, "GET", "/must-not-hit", {}, {}, 1024U, expired.expired()),
        Domain::ErrorCodes::DeadlineExceeded, "Already expired transport attempted HTTP.");
    require(server.requests().empty() && !server.waitForRequests(1U, 200ms), "Invalid boundary input sent an HTTP request.");
    server.requireHealthy();
}

} // namespace

int main()
{
    TestRegistry tests;
    addTest(tests, "image_http.binary_multipart_large_png", binaryMultipartAndLargePngPreserveEveryByte);
    addTest(tests, "image_http.authentication_cookie_isolation", authenticationChallengeAndCookiesDoNotReplay);
    addTest(tests, "image_http.redirects_no_replay", redirectsDoNotFollowOrReplayPost);
    addTest(tests, "image_http.complete_eof_bounds", responseBoundsRequireCompleteEof);
    addTest(tests, "image_http.status_malformed_incomplete", actual400AndMalformedOrIncompleteResponses);
    addTest(tests, "image_http.cancel_pending_body", cancellationWhileResponseIsPendingReturnsNoBytes);
    addTest(tests, "image_http.deadline_pending_body", shortDeadlineDuringPendingBodyReturnsNoBytes);
    addTest(tests, "image_http.invalid_inputs_no_network", invalidInputsAndAlreadyStoppedContextsNeverSendHttp);
    std::size_t passed{};
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "SUMMARY passed=" << passed << " failed=" << tests.size() - passed << '\n';
    return passed == tests.size() ? 0 : 1;
}
