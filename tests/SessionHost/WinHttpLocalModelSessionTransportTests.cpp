#include "ForgeConductor/Infrastructure/Windows/WinHttpLocalModelSessionTransport.h"

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
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

using namespace std::chrono_literals;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view expression)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{
            "Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
}

struct HttpRequest final {
    std::string method;
    std::string path;
    std::string body;
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
            encoded.substr(*headerEnd, contentLength)};
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

    [[nodiscard]] static bool sendResponse(
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
                    if (script.blockUntilReleased) {
                        std::unique_lock lock{stateMutex_};
                        stateChanged_.wait(lock, [&]() noexcept {
                            return releaseResponses_ || stopping_;
                        });
                    } else if (script.delay > 0ms) {
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
    std::string failure_;
    bool releaseResponses_{};
    bool stopping_{};
    bool winsockStarted_{};
};

constexpr std::string_view ProjectIdText =
    "11111111-1111-4111-8111-111111111111";
constexpr std::string_view ContinuityOperationIdText =
    "22222222-2222-4222-8222-222222222222";
constexpr std::string_view PredecessorSessionIdText =
    "33333333-3333-4333-8333-333333333333";
constexpr std::string_view SuccessorSessionIdText =
    "44444444-4444-4444-8444-444444444444";
constexpr std::string_view HandoffIdText =
    "55555555-5555-4555-8555-555555555555";
constexpr std::string_view ProviderIdText = "provider-1";

[[nodiscard]] Domain::SessionCreationRequest creationRequest()
{
    return Domain::SessionCreationRequest{
        parse<Domain::ContinuityOperationId>(ContinuityOperationIdText),
        parse<Domain::ProjectId>(ProjectIdText),
        parse<Domain::SessionId>(PredecessorSessionIdText),
        take(Domain::IdempotencyKey::create("winhttp-transport-test"))};
}

[[nodiscard]] Domain::NativeBootstrapRequest bootstrapRequest()
{
    return Domain::NativeBootstrapRequest{
        parse<Domain::ContinuityOperationId>(ContinuityOperationIdText),
        parse<Domain::ProjectId>(ProjectIdText),
        parse<Domain::SessionId>(SuccessorSessionIdText),
        parse<Domain::ProviderSessionId>(ProviderIdText),
        parse<Domain::ContinuityHandoffId>(HandoffIdText),
        parse<Domain::Sha256Digest>(std::string(64U, 'a')),
        R"({"schema_version":"1.0"})"};
}

[[nodiscard]] Domain::OperationContext operationContext(
    const std::string_view operationId,
    const std::chrono::milliseconds lifetime,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operationId),
        std::chrono::steady_clock::now() + lifetime,
        cancellation,
        parse<Domain::CorrelationId>("winhttp-session-transport-test")};
}

[[nodiscard]] InfrastructureWindows::WinHttpLocalModelSessionTransportConfiguration
configuration(const std::uint16_t port)
{
    InfrastructureWindows::WinHttpLocalModelSessionTransportConfiguration value;
    value.port = port;
    value.connectTimeout = 2s;
    value.sendTimeout = 2s;
    value.receiveTimeout = 2s;
    return value;
}

[[nodiscard]] std::string acknowledgementBody(
    const std::string_view usage = {})
{
    return "{\"handoff_id\":\"" + std::string{HandoffIdText} +
        "\",\"successor_session_id\":\"" +
        std::string{SuccessorSessionIdText} + "\"" +
        (usage.empty() ? "}" : "," + std::string{usage} + "}");
}

[[nodiscard]] std::string responseText(
    const Domain::NativeBootstrapResponse& response)
{
    std::string result;
    for (const auto& chunk : response.chunks) {
        result.append(
            reinterpret_cast<const char*>(chunk.data()), chunk.size());
    }
    return result;
}

void loopbackConfigurationIsFailClosed()
{
    static_assert(std::is_final_v<
                  InfrastructureWindows::WinHttpLocalModelSessionTransport>);
    static_assert(std::is_base_of_v<
                  ForgeConductor::Contracts::INativeSessionTransport,
                  InfrastructureWindows::WinHttpLocalModelSessionTransport>);

    const auto rejects = [](const std::string_view host) {
        auto value = configuration(1U);
        value.loopbackHost = host;
        bool rejected{};
        try {
            InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
                value};
            transport.shutdown();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE(rejected);
    };
    rejects("0.0.0.0");
    rejects("127.0.0.2");
    rejects("example.test");
    rejects("");

    auto uppercaseLocalhost = configuration(1U);
    uppercaseLocalhost.loopbackHost = "LOCALHOST";
    InfrastructureWindows::WinHttpLocalModelSessionTransport accepted{
        uppercaseLocalhost};
    accepted.shutdown();

    auto invalidPath = configuration(1U);
    invalidPath.basePath = "/v1/../forge";
    bool rejectedPath{};
    try {
        InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
            invalidPath};
        transport.shutdown();
    } catch (const std::invalid_argument&) {
        rejectedPath = true;
    }
    REQUIRE(rejectedPath);
}

void createBootstrapAndQueryUseExactRoutes()
{
    ResponseScript create{
        "POST", "/v1/forge/sessions", 201U,
        R"({"model":"fixture-model","provider_session_id":"provider-1"})"};
    ResponseScript bootstrap{
        "POST", "/v1/forge/sessions/provider-1/bootstrap", 200U,
        acknowledgementBody()};
    bootstrap.headers = {
        {"X-Forge-Input-Tokens", "12"},
        {"X-Forge-Output-Tokens", "3"}};
    ResponseScript query{
        "GET", "/v1/forge/sessions/provider-1", 200U,
        R"({"provider_session_id":"provider-1","status":"ready"})"};
    LoopbackHttpServer server{{create, bootstrap, query}};
    InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
        configuration(server.port())};

    const auto created = take(transport.createSession(
        creationRequest(),
        operationContext(
            "66666666-6666-4666-8666-666666666661", 5s)));
    REQUIRE(created.providerSessionId.value() == ProviderIdText);
    REQUIRE(created.model == std::optional<std::string>{"fixture-model"});

    const auto bootstrapped = take(transport.bootstrap(
        bootstrapRequest(),
        operationContext(
            "66666666-6666-4666-8666-666666666662", 5s)));
    REQUIRE(bootstrapped.inputTokens == 12);
    REQUIRE(bootstrapped.outputTokens == 3);
    REQUIRE(bootstrapped.chunks.size() == 1U);
    const auto acknowledgement = responseText(bootstrapped);
    REQUIRE(acknowledgement.find(std::string{HandoffIdText}) !=
            std::string::npos);
    REQUIRE(acknowledgement.find(std::string{SuccessorSessionIdText}) !=
            std::string::npos);

    REQUIRE(take(transport.query(
                created.providerSessionId,
                operationContext(
                    "66666666-6666-4666-8666-666666666663", 5s))) ==
            Domain::HostSessionStatus::Ready);
    REQUIRE(server.waitUntilHandled(3U, 5s));
    const auto requests = server.requests();
    REQUIRE(requests.size() == 3U);
    REQUIRE(requests[0].body.find(std::string{ProjectIdText}) !=
            std::string::npos);
    REQUIRE(requests[0].body.find("winhttp-transport-test") !=
            std::string::npos);
    REQUIRE(requests[1].body.find(std::string{HandoffIdText}) !=
            std::string::npos);
    REQUIRE(requests[1].body.find(std::string(64U, 'a')) !=
            std::string::npos);
    REQUIRE(requests[2].body.empty());
    server.requireHealthy();
}

void malformedAndOversizedResponsesFailClosed()
{
    ResponseScript malformed{
        "POST", "/v1/forge/sessions", 200U, "{not-json"};
    ResponseScript duplicate{
        "POST", "/v1/forge/sessions", 200U,
        R"({"provider_session_id":"provider-1","provider_session_id":"provider-2"})"};
    ResponseScript unknown{
        "POST", "/v1/forge/sessions", 200U,
        R"({"provider_session_id":"provider-1","transcript":"must-reject"})"};
    ResponseScript oversized{
        "POST", "/v1/forge/sessions", 200U,
        std::string(Domain::MaximumNativeResponseBytes + 1U, 'x')};
    oversized.allowClientDisconnect = true;
    ResponseScript wrongQuery{
        "GET", "/v1/forge/sessions/provider-1", 200U,
        R"({"provider_session_id":"provider-2","status":"ready"})"};
    ResponseScript negativeUsage{
        "POST", "/v1/forge/sessions/provider-1/bootstrap", 200U,
        acknowledgementBody("\"input_tokens\":-1,\"output_tokens\":3")};
    LoopbackHttpServer server{
        {malformed, duplicate, unknown, oversized, wrongQuery, negativeUsage}};
    InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
        configuration(server.port())};

    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto suffix = static_cast<char>('1' + index);
        std::string operation =
            "77777777-7777-4777-8777-77777777777";
        operation.push_back(suffix);
        const auto result = transport.createSession(
            creationRequest(), operationContext(operation, 5s));
        requireError(
            result,
            index == 3U ? Domain::ErrorCodes::PayloadTooLarge
                        : Domain::ErrorCodes::MalformedMessage);
    }

    requireError(
        transport.query(
            parse<Domain::ProviderSessionId>(ProviderIdText),
            operationContext(
                "77777777-7777-4777-8777-777777777775", 5s)),
        Domain::ErrorCodes::IntegrityFailure);
    requireError(
        transport.bootstrap(
            bootstrapRequest(),
            operationContext(
                "77777777-7777-4777-8777-777777777776", 5s)),
        Domain::ErrorCodes::MalformedMessage);

    auto oversizedHandoff = bootstrapRequest();
    oversizedHandoff.canonicalHandoffUtf8.assign(
        Domain::MaximumContinuityHandoffEncodedBytes + 1U, 'x');
    requireError(
        transport.bootstrap(
            oversizedHandoff,
            operationContext(
                "77777777-7777-4777-8777-777777777777", 5s)),
        Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(server.waitUntilHandled(6U, 5s));
    server.requireHealthy();
}

void rateLimitUsageAndProviderCancellationAreExact()
{
    ResponseScript rateLimited{
        "POST", "/v1/forge/sessions", 429U, R"({"retry":true})"};
    ResponseScript nestedUsage{
        "POST", "/v1/forge/sessions/provider-1/bootstrap", 200U,
        acknowledgementBody(
            R"("usage":{"input_tokens":9,"output_tokens":4})")};
    ResponseScript matchingUsage{
        "POST", "/v1/forge/sessions/provider-1/bootstrap", 200U,
        acknowledgementBody(R"("input_tokens":7,"output_tokens":2)")};
    matchingUsage.headers = {
        {"X-Forge-Input-Tokens", "7"},
        {"X-Forge-Output-Tokens", "2"}};
    ResponseScript conflictingUsage{
        "POST", "/v1/forge/sessions/provider-1/bootstrap", 200U,
        acknowledgementBody(R"("input_tokens":6,"output_tokens":2)")};
    conflictingUsage.headers = {
        {"X-Forge-Input-Tokens", "7"},
        {"X-Forge-Output-Tokens", "2"}};
    ResponseScript providerCancel{
        "POST", "/v1/forge/sessions/provider-1/cancel", 200U, R"({})"};
    LoopbackHttpServer server{{
        rateLimited,
        nestedUsage,
        matchingUsage,
        conflictingUsage,
        providerCancel}};
    InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
        configuration(server.port())};

    const auto limited = transport.createSession(
        creationRequest(),
        operationContext(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa1", 5s));
    requireError(limited, Domain::ErrorCodes::RateLimited);
    REQUIRE(limited.error().retryable);

    const auto nested = take(transport.bootstrap(
        bootstrapRequest(),
        operationContext(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa2", 5s)));
    REQUIRE(nested.inputTokens == 9);
    REQUIRE(nested.outputTokens == 4);

    const auto matching = take(transport.bootstrap(
        bootstrapRequest(),
        operationContext(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa3", 5s)));
    REQUIRE(matching.inputTokens == 7);
    REQUIRE(matching.outputTokens == 2);

    requireError(
        transport.bootstrap(
            bootstrapRequest(),
            operationContext(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa4", 5s)),
        Domain::ErrorCodes::MalformedMessage);

    const auto cancelOperation = parse<Domain::OperationId>(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa5");
    transport.cancel(
        cancelOperation,
        parse<Domain::ProviderSessionId>(ProviderIdText));
    REQUIRE(server.waitUntilHandled(5U, 5s));
    const auto requests = server.requests();
    REQUIRE(requests.size() == 5U);
    REQUIRE(requests.back().body.find(cancelOperation.value()) !=
            std::string::npos);
    server.requireHealthy();
}

template <typename ResultType, typename Start, typename Interrupt>
[[nodiscard]] ResultType runInterruptedRequest(
    LoopbackHttpServer& server,
    Start start,
    Interrupt interrupt)
{
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<ResultType> result;
    std::thread worker{[&]() {
        auto completed = start();
        {
            std::lock_guard lock{mutex};
            result = std::move(completed);
        }
        changed.notify_all();
    }};
    const bool requestObserved = server.waitForRequests(1U, 5s);
    if (!requestObserved) {
        interrupt();
        server.releaseBlockedResponses();
        worker.join();
        REQUIRE(requestObserved);
    }
    interrupt();

    bool completed{};
    {
        std::unique_lock lock{mutex};
        completed = changed.wait_for(lock, 3s, [&]() noexcept {
            return result.has_value();
        });
    }
    server.releaseBlockedResponses();
    worker.join();
    REQUIRE(completed);
    REQUIRE(result.has_value());
    return std::move(*result);
}

void deadlineAndActiveCancellationAreBounded()
{
    ResponseScript delayed{
        "POST", "/v1/forge/sessions", 200U,
        R"({"provider_session_id":"provider-1"})"};
    delayed.delay = 300ms;
    delayed.allowClientDisconnect = true;
    LoopbackHttpServer timeoutServer{{delayed}};
    auto timeoutConfiguration = configuration(timeoutServer.port());
    timeoutConfiguration.receiveTimeout = 50ms;
    InfrastructureWindows::WinHttpLocalModelSessionTransport timeoutTransport{
        timeoutConfiguration};
    const auto timedOut = timeoutTransport.createSession(
        creationRequest(),
        operationContext(
            "88888888-8888-4888-8888-888888888881", 80ms));
    requireError(timedOut, Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(timedOut.error().retryable);
    REQUIRE(timeoutServer.waitForRequests(1U, 2s));
    timeoutServer.requireHealthy();

    ResponseScript blocked{
        "POST", "/v1/forge/sessions", 200U,
        R"({"provider_session_id":"provider-1"})"};
    blocked.blockUntilReleased = true;
    blocked.allowClientDisconnect = true;
    LoopbackHttpServer cancellationServer{{blocked}};
    InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
        configuration(cancellationServer.port())};
    const auto context = operationContext(
        "88888888-8888-4888-8888-888888888882", 5s);
    const auto cancelled = runInterruptedRequest<
        Domain::Result<Domain::NativeTransportSession>>(
        cancellationServer,
        [&]() { return transport.createSession(creationRequest(), context); },
        [&]() { transport.cancel(context.operationId, std::nullopt); });
    requireError(cancelled, Domain::ErrorCodes::Cancelled);

    requireError(
        transport.createSession(creationRequest(), context),
        Domain::ErrorCodes::Cancelled);
    cancellationServer.requireHealthy();

    ResponseScript stopBlocked{
        "POST", "/v1/forge/sessions", 200U,
        R"({"provider_session_id":"provider-1"})"};
    stopBlocked.blockUntilReleased = true;
    stopBlocked.allowClientDisconnect = true;
    LoopbackHttpServer stopServer{{stopBlocked}};
    InfrastructureWindows::WinHttpLocalModelSessionTransport stopTransport{
        configuration(stopServer.port())};
    std::stop_source stopSource;
    const auto stopContext = operationContext(
        "88888888-8888-4888-8888-888888888883",
        5s,
        stopSource.get_token());
    const auto stopCancelled = runInterruptedRequest<
        Domain::Result<Domain::NativeTransportSession>>(
        stopServer,
        [&]() {
            return stopTransport.createSession(
                creationRequest(), stopContext);
        },
        [&]() { stopSource.request_stop(); });
    requireError(stopCancelled, Domain::ErrorCodes::Cancelled);
    stopServer.requireHealthy();
}

void shutdownClosesActiveAndFutureRequests()
{
    ResponseScript blocked{
        "GET", "/v1/forge/sessions/provider-1", 200U,
        R"({"provider_session_id":"provider-1","status":"ready"})"};
    blocked.blockUntilReleased = true;
    blocked.allowClientDisconnect = true;
    LoopbackHttpServer server{{blocked}};
    InfrastructureWindows::WinHttpLocalModelSessionTransport transport{
        configuration(server.port())};
    const auto provider = parse<Domain::ProviderSessionId>(ProviderIdText);
    const auto context = operationContext(
        "99999999-9999-4999-8999-999999999991", 5s);
    const auto interrupted = runInterruptedRequest<
        Domain::Result<Domain::HostSessionStatus>>(
        server,
        [&]() { return transport.query(provider, context); },
        [&]() { transport.shutdown(); });
    REQUIRE(!interrupted);
    REQUIRE(interrupted.error().code == Domain::ErrorCodes::TransportClosed ||
            interrupted.error().code == Domain::ErrorCodes::Cancelled);

    requireError(
        transport.query(
            provider,
            operationContext(
                "99999999-9999-4999-8999-999999999992", 5s)),
        Domain::ErrorCodes::TransportClosed);
    transport.shutdown();
    server.requireHealthy();
}

} // namespace

int main()
{
    try {
        loopbackConfigurationIsFailClosed();
        std::cout << "PASS winhttp_transport.loopback_configuration\n";
        createBootstrapAndQueryUseExactRoutes();
        std::cout << "PASS winhttp_transport.create_bootstrap_query\n";
        malformedAndOversizedResponsesFailClosed();
        std::cout << "PASS winhttp_transport.response_validation_bounds\n";
        rateLimitUsageAndProviderCancellationAreExact();
        std::cout << "PASS winhttp_transport.rate_limit_usage_cancel\n";
        deadlineAndActiveCancellationAreBounded();
        std::cout << "PASS winhttp_transport.deadline_cancellation\n";
        shutdownClosesActiveAndFutureRequests();
        std::cout << "PASS winhttp_transport.shutdown\n";
        std::cout << "SUMMARY passed=6 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
