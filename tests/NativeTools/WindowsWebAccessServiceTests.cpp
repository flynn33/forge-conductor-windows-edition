#include "ForgeConductor/NativeTools/Windows/WindowsWebAccessService.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include "Infrastructure/TestSupport.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
using Service = ForgeConductor::NativeTools::Windows::WindowsWebAccessService;
using Json = nlohmann::json;
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
                "Content-Type: application/json\r\n" +
                "Content-Length: " + std::to_string(script.body.size()) +
                "\r\nConnection: close\r\n";
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

std::string url(const LoopbackHttpServer& server, std::string_view path = "/") {
    return "http://127.0.0.1:" + std::to_string(server.port()) + std::string{path};
}
Json run(Service& service, std::string_view tool, const Json& args, const Domain::OperationContext& context = TestContext{}.active()) {
    return Json::parse(take(service.execute(tool, args.dump(), context)));
}
void fetchStatusUnicodeAndBounds() {
    const std::string unicode = "unseen \xCE\xA9 \xE2\x82\xAC";
    LoopbackHttpServer server{{{"GET", "/missing", 404U, unicode}, {"GET", "/large", 200U, std::string(1025U, 'x')}, {"GET", "/binary", 200U, std::string{"\xFF\0", 2U}}}};
    Service service;
    auto missing = run(service, "web_fetch", {{"url", url(server, "/missing")}});
    require(missing["status"] == 404U && missing["ok"] == false && missing["body_utf8"] == unicode, "Non-2xx status or Unicode source was lost.");
    require(missing["final_url"] == url(server, "/missing") && missing["encoding"] == "utf-8", "Response provenance was wrong.");
    auto large = run(service, "web_fetch", {{"url", url(server, "/large")}, {"max_bytes", 1024U}});
    require(large["bytes_returned"] == 1024U && large["truncated"] == true && large["content_length"] == 1025U, "Response body bound or total length was not preserved.");
    auto binary = run(service, "web_fetch", {{"url", url(server, "/binary")}});
    require(binary["encoding"] == "base64" && binary["body_base64"] == "/wA=", "Binary response bytes were not preserved.");
    const auto requests = server.requests();
    require(requests.size() == 3U, "Unexpected request count.");
    require(requests.front().headers.find("Forge-Conductor/" + std::string{Domain::ProductVersion}) != std::string::npos, "Wire User-Agent does not use the active version.");
    require(lowercase(requests.front().headers).find("authorization:") == std::string::npos && lowercase(requests.front().headers).find("cookie:") == std::string::npos, "Ambient credentials were sent.");
    server.requireHealthy();
}
void exactBoundAndEscapedProjection() {
    LoopbackHttpServer server{{{"GET", "/exact", 200U, std::string(1024U, 'x')}, {"GET", "/escaped", 200U, std::string(Service::MaximumBodyBytes, '\0')}, {"GET", "/quotes", 200U, std::string(Service::MaximumBodyBytes, '\\')}}};
    Service service;
    auto exact = run(service, "web_fetch", {{"url", url(server, "/exact")}, {"max_bytes", 1024U}});
    require(exact["truncated"] == false && exact["bytes_returned"] == 1024U, "An exact-limit EOF was called truncated.");
    for (const auto& path : {"/escaped", "/quotes"}) {
        const auto encoded = take(service.execute("web_fetch", Json{{"url", url(server, path)}, {"max_bytes", Service::MaximumBodyBytes}}.dump(), TestContext{}.active()));
        auto result = Json::parse(encoded);
        require(result["bytes_returned"] == Service::MaximumBodyBytes && encoded.size() < 110U * 1024U, "Response projection exceeded the MCP wire budget.");
        if (std::string_view{path} == "/escaped") require(result["encoding"] == "base64", "Control-byte source was not base64 encoded.");
    }
    server.requireHealthy();
}
void redirectsAndHeaderIsolation() {
    LoopbackHttpServer target{{{"GET", "/foreign", 200U, "foreign"}}};
    LoopbackHttpServer server{{{"GET", "/first", 302U, "", {{"Location", "/next"}, {"Set-Cookie", "secret=ambient"}}},
        {"GET", "/next", 302U, "", {{"Location", url(target, "/foreign")}}}}};
    Service service;
    auto result = run(service, "http_request", {{"url", url(server, "/first")}, {"headers", {{"Authorization", "Bearer explicit-secret"}, {"X-Private", "private-value"}}}});
    require(result["redirects"] == 2U && result["body_utf8"] == "foreign" && result["final_url"] == url(target, "/foreign"), "Relative or cross-origin redirects failed.");
    const auto local = server.requests(); const auto foreign = target.requests();
    require(local.size() == 2U && local[1].headers.find("Bearer explicit-secret") != std::string::npos, "Same-origin explicit header was lost.");
    require(local[1].headers.find("secret=ambient") == std::string::npos, "Redirect response cookie was replayed.");
    require(foreign.size() == 1U && foreign[0].headers.find("explicit-secret") == std::string::npos && foreign[0].headers.find("private-value") == std::string::npos, "Caller headers crossed an origin boundary.");
    require(result.dump().find("explicit-secret") == std::string::npos, "Request credential was reflected into the response.");
    server.requireHealthy(); target.requireHealthy();
}
void postAndHeadDoNotReplay() {
    LoopbackHttpServer server{{{"POST", "/post", 201U, "accepted"}, {"POST", "/redirect", 307U, "not replayed", {{"Location", "/unexpected"}}}, {"HEAD", "/head", 200U, "hidden"}}};
    Service service;
    const std::string body = "{\"value\":\"\xCE\xA9\"}";
    const auto posted = run(service, "http_request", {{"url", url(server, "/post")}, {"method", "POST"}, {"body", body}, {"headers", {{"Content-Type", "application/json"}, {"X-Test", "yes"}}}});
    require(posted["status"] == 201U && posted["ok"] == true, "POST status was lost.");
    const auto redirected = run(service, "http_request", {{"url", url(server, "/redirect")}, {"method", "POST"}, {"body", "once"}});
    require(redirected["status"] == 307U && redirected["redirects"] == 0U && redirected["ok"] == false, "POST redirect was replayed.");
    const auto head = run(service, "http_request", {{"url", url(server, "/head")}, {"method", "HEAD"}});
    require(head["bytes_returned"] == 0U && head["body_utf8"] == "", "HEAD exposed a body.");
    const auto requests = server.requests();
    require(requests.size() == 3U && requests[0].body == body && requests[1].body == "once", "POST body changed or request was replayed.");
    server.requireHealthy();
}
void explicitVerbsDoNotReplay() {
    const std::vector<std::string> methods{"PUT", "PATCH", "DELETE", "OPTIONS"};
    std::vector<ResponseScript> scripts;
    for (const auto& method : methods) {
        scripts.push_back({method, "/explicit", 200U, "actual method delivered"});
        scripts.push_back({method, "/redirect", 307U, "not replayed", {{"Location", "/unexpected"}}});
    }
    LoopbackHttpServer server{std::move(scripts)};
    Service service;
    const std::string body = "explicit-\xCE\xA9-body";
    for (const auto& method : methods) {
        Json args{{"url", url(server, "/explicit")}, {"method", method}};
        if (method != "OPTIONS") args["body"] = body;
        const auto delivered = run(service, "http_request", args);
        require(delivered["status"] == 200U && delivered["body_utf8"] == "actual method delivered",
            "An explicit HTTP verb was not delivered.");
        args["url"] = url(server, "/redirect");
        const auto redirected = run(service, "http_request", args);
        require(redirected["status"] == 307U && redirected["redirects"] == 0U && redirected["body_utf8"] == "not replayed",
            "An explicit HTTP verb was replayed after a redirect.");
    }
    require(server.waitUntilHandled(8U, 2s), "Explicit HTTP verbs were not all observed by the server.");
    const auto requests = server.requests();
    require(requests.size() == 8U, "A mutating or OPTIONS request was silently replayed.");
    for (std::size_t index = 0U; index < requests.size(); ++index)
        require(requests[index].method == methods[index / 2U] && requests[index].body == (methods[index / 2U] == "OPTIONS" ? "" : body),
            "The explicit method or Unicode request body changed.");
    server.requireHealthy();
}
void refusesInvalidBoundaryInput() {
    Service service;
    const auto context = TestContext{}.active();
    for (const auto& args : std::vector<Json>{
        {{"url", "file:///private"}}, {{"url", "https://user:secret@example.com/"}}, {{"url", "https://example.com/\r\nInjected"}},
        {{"url", "https://example.com/"}, {"max_bytes", 0}}, {{"url", "https://example.com/"}, {"max_bytes", Service::MaximumBodyBytes + 1U}},
        {{"url", "https://example.com/"}, {"timeout_sec", 61}}, {{"url", "https://example.com/"}, {"timeout_sec", 1.5}},
        {{"url", "https://example.com/"}, {"method", "TRACE"}}, {{"url", "https://example.com/"}, {"method", "GET"}, {"body", "hidden"}},
        {{"url", "https://example.com/"}, {"method", "HEAD"}, {"body", "hidden"}},
        {{"url", "https://example.com/"}, {"method", "OPTIONS"}, {"body", "hidden"}},
        {{"url", "https://example.com/"}, {"headers", {{"X-Test", "yes\r\nAuthorization: stolen"}}}},
        {{"url", "https://example.com/"}, {"headers", {{"Host", "elsewhere"}}}}, {{"url", "https://example.com/"}, {"headers", {{"Cookie", "secret"}}}},
        {{"url", "https://example.com/"}, {"headers", {{"Proxy-Authorization", "secret"}}}},
        {{"url", "https://example.com/"}, {"headers", {{"X-Duplicate", "one"}, {"x-duplicate", "two"}}}}
    }) requireError(service.execute("http_request", args.dump(), context), Domain::ErrorCodes::InvalidRequest, "Invalid HTTP boundary input was admitted.");
    requireError(service.execute("web_fetch", "[1]", context), Domain::ErrorCodes::InvalidRequest, "Non-object arguments were admitted.");
    requireError(service.execute("web_search", R"({"query":"test","limit":11})", context), Domain::ErrorCodes::InvalidRequest, "Excess search results were admitted.");
    requireError(service.execute("unknown", "{}", context), Domain::ErrorCodes::InvalidRequest, "Unknown web tool was admitted.");
    TestContext cancelled; cancelled.cancellation.request_stop();
    requireError(service.execute("web_fetch", "{}", cancelled.expired()), Domain::ErrorCodes::Cancelled, "Cancellation did not precede expiry/validation.");
    requireError(service.execute("web_fetch", "{}", TestContext{}.expired()), Domain::ErrorCodes::DeadlineExceeded, "Expired context was admitted.");
}
void callerDeadlineAndTotalTimeout() {
    Service service;
    {
        LoopbackHttpServer server{{{"GET", "/short", 200U, "late", {}, 1500ms, false, true}}};
        auto context = TestContext{}.active(); context.deadline = std::chrono::steady_clock::now() + 60ms;
        const auto start = std::chrono::steady_clock::now();
        requireError(service.execute("web_fetch", Json{{"url", url(server, "/short")}, {"timeout_sec", 60}}.dump(), context), Domain::ErrorCodes::DeadlineExceeded, "Caller deadline did not abort header wait.");
        require(std::chrono::steady_clock::now() - start < 600ms, "Caller deadline was delayed by synchronous HTTP.");
        server.requireHealthy();
    }
    {
        LoopbackHttpServer server{{{"GET", "/slow-body", 200U, std::string(16384U, 'x'), {}, 2500ms, false, true, true}}};
        const auto start = std::chrono::steady_clock::now();
        requireError(service.execute("web_fetch", Json{{"url", url(server, "/slow-body")}, {"timeout_sec", 1}}.dump(), TestContext{}.active()), Domain::ErrorCodes::DeadlineExceeded, "Selected timeout did not cover a partial body.");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        require(elapsed >= 850ms && elapsed < 1800ms, "Total body timeout did not enforce the selected one-second budget.");
        server.requireHealthy();
    }
}
void cancelsPendingBodies() {
    Service service;
    for (std::size_t i = 0; i < 4U; ++i) {
        LoopbackHttpServer server{{{"GET", "/cancel", 200U, std::string(16384U, 'x'), {}, 0ms, true, true, true}}};
        TestContext test;
        auto context = test.active();
        std::optional<Domain::Result<std::string>> result;
        std::thread caller{[&] { result.emplace(service.execute("web_fetch", Json{{"url", url(server, "/cancel")}}.dump(), context)); }};
        const bool started = server.waitForBodyResponses(1U, 2s);
        const auto start = std::chrono::steady_clock::now();
        test.cancellation.request_stop(); caller.join();
        require(started, "Cancellation fixture never sent the first body chunk.");
        require(result.has_value(), "Cancellation did not return.");
        requireError(*result, Domain::ErrorCodes::Cancelled, "Pending body cancellation was misclassified.");
        require(std::chrono::steady_clock::now() - start < 600ms, "Pending body cancellation was delayed.");
        server.releaseBlockedResponses(); server.requireHealthy();
    }
}
void boundedRedirectLoop() {
    std::vector<ResponseScript> scripts;
    for (std::size_t i = 0U; i < 6U; ++i) scripts.push_back({"GET", "/loop", 302U, "", {{"Location", "/loop"}}});
    LoopbackHttpServer server{std::move(scripts)};
    Service service;
    requireError(service.execute("web_fetch", Json{{"url", url(server, "/loop")}}.dump(), TestContext{}.active()), Domain::ErrorCodes::LimitExceeded, "Redirect loop exceeded five hops.");
    require(server.requests().size() == 6U, "Redirect hop limit was not exact.");
    server.requireHealthy();
}
void parsesObservedSearchFormatAndReportsChallenges() {
    const std::string html = "<a rel=\"nofollow\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Farticle&amp;rut=x\" class='result-link'>A &amp; &#937; <b>result</b></a><td class='result-snippet'>Actual &#x20ac; snippet</td><a class='result-link' href='http://example.org/plain'>Second</a>";
    LoopbackHttpServer server{{{"GET", "/search?q=%CE%A9%20%26", 200U, html}, {"GET", "/search?q=challenge", 202U, "challenge"}, {"GET", "/search?q=empty", 200U, "no recognized anchors"}}};
    Service service{url(server, "/search")};
    auto results = run(service, "web_search", {{"query", "\xCE\xA9 &"}, {"limit", 2U}});
    require(results["search_available"] == true && results["results"].size() == 2U, "Observed public search anchors were not parsed.");
    require(results["results"][0]["url"] == "https://example.com/article" && results["results"][0]["title"] == "A & \xCE\xA9 result" && results["results"][0]["snippet"] == "Actual \xE2\x82\xAC snippet", "Search result link/entity/source text was changed.");
    require(results["results"][1]["url"] == "http://example.org/plain", "HTTP result link was silently dropped.");
    require(results["results"][0]["source_url"] == results["source_url"], "Search result lacks source provenance.");
    auto challenge = run(service, "web_search", {{"query", "challenge"}});
    require(challenge["status"] == 202U && challenge["ok"] == false && challenge["results"].empty() && challenge.contains("diagnostic"), "Search challenge was reported as success.");
    auto empty = run(service, "web_search", {{"query", "empty"}});
    require(empty["search_available"] == false && empty.contains("diagnostic"), "Unrecognized search response was reported as success.");
    server.requireHealthy();
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--live") {
        try {
            Service service;
            const auto page = run(service, "web_fetch", {{"url", "https://example.com/"}});
            const auto search = run(service, "web_search", {{"query", "Forge Conductor Windows"}, {"limit", 3U}});
            std::cout << Json{{"page", page}, {"search", search}}.dump(2) << '\n';
            return page["ok"] == true && search["search_available"] == true ? EXIT_SUCCESS : EXIT_FAILURE;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
    }
    TestRegistry tests;
    addTest(tests, "web.status_unicode_bound_binary", fetchStatusUnicodeAndBounds);
    addTest(tests, "web.exact_bound_escaped_projection", exactBoundAndEscapedProjection);
    addTest(tests, "web.redirect_header_isolation", redirectsAndHeaderIsolation);
    addTest(tests, "web.explicit_verbs_no_replay", explicitVerbsDoNotReplay);
    addTest(tests, "web.post_head_no_replay", postAndHeadDoNotReplay);
    addTest(tests, "web.input_context_validation", refusesInvalidBoundaryInput);
    addTest(tests, "web.deadline_total_timeout", callerDeadlineAndTotalTimeout);
    addTest(tests, "web.cancel_partial_bodies", cancelsPendingBodies);
    addTest(tests, "web.redirect_loop", boundedRedirectLoop);
    addTest(tests, "web.search_format_challenge", parsesObservedSearchFormatAndReportsChallenges);
    std::size_t passed{};
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "SUMMARY passed=" << passed << " failed=" << (tests.size() - passed) << '\n';
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}
