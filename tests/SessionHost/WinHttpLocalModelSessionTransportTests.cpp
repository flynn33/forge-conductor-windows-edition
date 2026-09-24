#include "ForgeConductor/Infrastructure/Windows/WinHttpLocalModelSessionTransport.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsModelPreparation.h"
#include "ForgeConductor/Infrastructure/Windows/SettingsBoundResponsesTransport.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include "ManagerConnection.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

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
using Json = nlohmann::json;

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

[[nodiscard]] InfrastructureWindows::LMStudioResponsesTransportConfiguration
responsesConfiguration(const std::uint16_t port)
{
    InfrastructureWindows::LMStudioResponsesTransportConfiguration value;
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

void lmStudioResponsesUsesFreshRootToolOutputAndActualResponseId()
{
    const std::string expectedAcknowledgement =
        "{\"handoff_id\":\"" + std::string{HandoffIdText} +
        "\",\"successor_session_id\":\"" +
        std::string{SuccessorSessionIdText} + "\"}";
    ResponseScript models{
        "GET", "/v1/models", 200U,
        R"({"object":"list","data":[{"id":"fixture-model","owned_by":"local"}]})"};
    ResponseScript toolCall{
        "POST", "/v1/responses", 200U,
        "{\"id\":\"resp_fresh_root\",\"status\":\"completed\","
        "\"output\":[{\"type\":\"function_call\",\"name\":\"context_get\","
        "\"call_id\":\"call_context\",\"arguments\":\"{\\\"handoff_id\\\":\\\"" +
            std::string{HandoffIdText} + "\\\"}\"}],"
        "\"usage\":{\"input_tokens\":31,\"output_tokens\":7},"
        "\"benign_extra\":true}"};
    ResponseScript acknowledgement{
        "POST", "/v1/responses", 200U,
        Json{
            {"id", "resp_successor_ack"},
            {"status", "completed"},
            {"output_text", expectedAcknowledgement},
            {"usage", Json{{"input_tokens", 47U}, {"output_tokens", 11U}}},
            {"provider_extension", Json{{"loaded", true}}}}
            .dump()};
    LoopbackHttpServer server{{models, toolCall, acknowledgement}};
    InfrastructureWindows::LMStudioResponsesTransport transport{
        responsesConfiguration(server.port())};

    const auto created = take(transport.createSession(
        creationRequest(),
        operationContext("65656565-6565-4565-8565-656565656561", 5s)));
    REQUIRE(created.providerSessionId.value().starts_with("forge-pending-"));
    REQUIRE(created.model == std::optional<std::string>{"fixture-model"});

    const auto bootstrapped = take(transport.bootstrap(
        bootstrapRequest(),
        operationContext("65656565-6565-4565-8565-656565656562", 5s)));
    REQUIRE(bootstrapped.providerResponseId.has_value());
    REQUIRE(bootstrapped.providerResponseId->value() == "resp_successor_ack");
    REQUIRE(bootstrapped.inputTokens == 78);
    REQUIRE(bootstrapped.outputTokens == 18);
    REQUIRE(responseText(bootstrapped) == expectedAcknowledgement);
    REQUIRE(take(transport.query(
                *bootstrapped.providerResponseId,
                operationContext(
                    "65656565-6565-4565-8565-656565656563", 5s))) ==
            Domain::HostSessionStatus::Ready);

    REQUIRE(server.waitUntilHandled(3U, 5s));
    const auto requests = server.requests();
    REQUIRE(requests.size() == 3U);
    const auto first = Json::parse(requests[1].body);
    REQUIRE(!first.contains("previous_response_id"));
    REQUIRE(first.at("model") == "fixture-model");
    REQUIRE(first.at("tools").at(0).at("name") == "context_get");
    REQUIRE(first.at("tool_choice") == "required");
    const auto second = Json::parse(requests[2].body);
    REQUIRE(second.at("previous_response_id") == "resp_fresh_root");
    REQUIRE(second.at("input").at(0).at("type") == "function_call_output");
    REQUIRE(second.at("input").at(0).at("call_id") == "call_context");
    REQUIRE(second.at("input").at(0).at("output") ==
            bootstrapRequest().canonicalHandoffUtf8);
    server.requireHealthy();
}

void lmStudioResponsesCompletesAnOrdinaryManagedTurn()
{
    ResponseScript models{
        "GET", "/v1/models", 200U,
        R"({"object":"list","data":[{"id":"fixture-model"}]})"};
    ResponseScript ordinary{
        "POST", "/v1/responses", 200U,
        R"({"id":"resp_ordinary","status":"completed","output_text":"ordinary result","usage":{"input_tokens":12,"output_tokens":4}})"};
    LoopbackHttpServer server{{models, ordinary}};
    InfrastructureWindows::LMStudioResponsesTransport transport{
        responsesConfiguration(server.port())};

    const auto result = take(transport.complete(
        Domain::ManagedProviderTurnRequest{
            parse<Domain::ProjectId>(ProjectIdText),
            parse<Domain::SessionId>(SuccessorSessionIdText),
            9U,
            "Perform the selected project task.",
            std::nullopt},
        operationContext(
            "64646464-6464-4464-8464-646464646464", 5s)));
    REQUIRE(result.responseId.value() == "resp_ordinary");
    REQUIRE(result.outputText == "ordinary result");
    REQUIRE(result.inputTokens == 12U);
    REQUIRE(result.outputTokens == 4U);
    REQUIRE(result.retainedContextTokens == 16U);

    REQUIRE(server.waitUntilHandled(2U, 5s));
    const auto requests = server.requests();
    REQUIRE(requests.size() == 2U);
    const auto body = Json::parse(requests[1].body);
    REQUIRE(body.at("model") == "fixture-model");
    REQUIRE(body.at("input") == "Perform the selected project task.");
    REQUIRE(body.at("store") == true);
    REQUIRE(!body.contains("previous_response_id"));
    server.requireHealthy();
}

void lmStudioResponsesCorrelatesManagedFunctionOutput()
{
    ResponseScript models{
        "GET", "/v1/models", 200U,
        R"({"object":"list","data":[{"id":"fixture-model"}]})"};
    ResponseScript toolCall{
        "POST", "/v1/responses", 200U,
        R"({"id":"resp_tool_call","status":"completed","output":[{"type":"function_call","name":"fixture_read","call_id":"call_fixture_1","arguments":"{\"path\":\"README.md\"}"}],"usage":{"input_tokens":20,"output_tokens":3}})"};
    ResponseScript terminal{
        "POST", "/v1/responses", 200U,
        R"({"id":"resp_tool_done","status":"completed","output_text":"tool result accepted","usage":{"input_tokens":28,"output_tokens":6}})"};
    LoopbackHttpServer server{{models, toolCall, terminal}};
    InfrastructureWindows::LMStudioResponsesTransport transport{
        responsesConfiguration(server.port())};
    const Domain::McpToolDescriptor descriptor{
        Domain::ToolDescriptor{
            "fixture_read",
            "Read a fixture.",
            "fixture",
            Domain::ToolEffect::Read,
            Domain::ToolAvailability::Available,
            true,
            false},
        R"({"additionalProperties":false,"properties":{"path":{"type":"string"}},"required":["path"],"type":"object"})"};

    const auto first = take(transport.complete(
        Domain::ManagedProviderTurnRequest{
            parse<Domain::ProjectId>(ProjectIdText),
            parse<Domain::SessionId>(SuccessorSessionIdText),
            9U,
            "Read the project file.",
            std::nullopt,
            {descriptor},
            {}},
        operationContext(
            "63636363-6363-4363-8363-636363636361", 5s)));
    REQUIRE(first.responseId.value() == "resp_tool_call");
    REQUIRE(first.outputText.empty());
    REQUIRE(first.functionCalls.size() == 1U);
    REQUIRE(first.functionCalls[0].callId == "call_fixture_1");
    REQUIRE(first.functionCalls[0].name == "fixture_read");
    REQUIRE(first.functionCalls[0].canonicalArguments ==
            "{\"path\":\"README.md\"}");

    const auto second = take(transport.complete(
        Domain::ManagedProviderTurnRequest{
            parse<Domain::ProjectId>(ProjectIdText),
            parse<Domain::SessionId>(SuccessorSessionIdText),
            9U,
            {},
            first.responseId,
            {descriptor},
            {{"call_fixture_1", "{\"ok\":true}"}}},
        operationContext(
            "63636363-6363-4363-8363-636363636362", 5s)));
    REQUIRE(second.responseId.value() == "resp_tool_done");
    REQUIRE(second.outputText == "tool result accepted");
    REQUIRE(second.functionCalls.empty());

    REQUIRE(server.waitUntilHandled(3U, 5s));
    const auto requests = server.requests();
    const auto firstBody = Json::parse(requests[1].body);
    REQUIRE(firstBody.at("tools").at(0).at("name") == "fixture_read");
    REQUIRE(firstBody.at("parallel_tool_calls") == false);
    const auto secondBody = Json::parse(requests[2].body);
    REQUIRE(secondBody.at("previous_response_id") == "resp_tool_call");
    REQUIRE(secondBody.at("input").at(0).at("type") ==
            "function_call_output");
    REQUIRE(secondBody.at("input").at(0).at("call_id") == "call_fixture_1");
    REQUIRE(secondBody.at("input").at(0).at("output") == "{\"ok\":true}");
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

void providerSettingsApplyToNewRunsAndPreserveExistingRuns()
{
    const auto reply = [](const char* id) {
        return Json{{"id", id}, {"status", "completed"}, {"output_text", "OK"},
            {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}}.dump();
    };
    LoopbackHttpServer first{{
        {"GET", "/v1/models", 200U, R"({"data":[{"id":"first-model"}]})"},
        {"POST", "/v1/responses", 200U, reply("first-response")},
        {"POST", "/v1/responses", 200U, reply("continued-response")},
        {"GET", "/v1/models", 200U, R"({"data":[{"id":"first-model"}]})"},
        {"POST", "/v1/responses", 200U, reply("recovered-response")}}};
    LoopbackHttpServer second{{
        {"GET", "/v1/models", 200U, R"({"data":[{"id":"second-model"}]})"},
        {"POST", "/v1/responses", 200U, reply("second-response")}}};
    std::uint16_t selectedPort = first.port();
    int resolved{};
    std::string journal;
    const auto resolver = [&](const Domain::OperationContext&) {
            ++resolved;
            auto config = responsesConfiguration(selectedPort);
            config.model = selectedPort == first.port() ? "first-model" : "second-model";
            return Domain::Result<InfrastructureWindows::LMStudioResponsesTransportConfiguration>::success(
                std::move(config));
        };
    const auto load = [&](const Domain::OperationContext&) { return Domain::Result<std::string>::success(journal); };
    const auto save = [&](const std::string& data, const Domain::OperationContext&) {
        journal = data; return Domain::Result<void>::success();
    };
    InfrastructureWindows::SettingsBoundResponsesTransport transport{resolver, load, save};
    Domain::ManagedProviderTurnRequest request{parse<Domain::ProjectId>(ProjectIdText),
        parse<Domain::SessionId>(SuccessorSessionIdText), 1U, "Check", std::nullopt, {}, {}};
    auto initial = take(transport.complete(request,
        operationContext("12121212-1212-4212-8212-121212121214", 5s)));
    selectedPort = second.port();
    request.previousResponseId = initial.responseId;
    auto continued = take(transport.complete(request,
        operationContext("12121212-1212-4212-8212-121212121215", 5s)));
    REQUIRE(continued.responseId.value() == "continued-response");
    request.runId = parse<Domain::SessionId>("12121212-1212-4212-8212-121212121216");
    request.previousResponseId.reset();
    auto fresh = take(transport.complete(request,
        operationContext("12121212-1212-4212-8212-121212121217", 5s)));
    REQUIRE(fresh.responseId.value() == "second-response");
    REQUIRE(resolved == 2);
    transport.shutdown();
    InfrastructureWindows::SettingsBoundResponsesTransport recovered{resolver, load, save};
    request.runId = parse<Domain::SessionId>(SuccessorSessionIdText);
    request.previousResponseId = continued.responseId;
    auto recovery = take(recovered.complete(request, operationContext("12121212-1212-4212-8212-121212121218", 5s)));
    REQUIRE(recovery.responseId.value() == "recovered-response");
    REQUIRE(resolved == 2);
    REQUIRE(first.waitUntilHandled(5U, 5s));
    REQUIRE(second.waitUntilHandled(2U, 5s));
    REQUIRE(Json::parse(first.requests()[2].body).at("model") == "first-model");
    REQUIRE(Json::parse(second.requests()[1].body).at("model") == "second-model");
    first.requireHealthy(); second.requireHealthy();
}

void automaticModelPreparationUsesVerifiedInventory()
{
    const Json model{{"type", "llm"}, {"key", "coding-model"}, {"size_bytes", 4096},
        {"max_context_length", 65536}, {"capabilities", {{"trained_for_tool_use", true}}},
        {"loaded_instances", Json::array()}};
    auto loaded = model;
    loaded["loaded_instances"] = Json::array({{{"id", "coding-instance"}, {"config", {{"context_length", 32768}}}}});
    LoopbackHttpServer server{{
        {"GET", "/api/v1/models", 200U, Json{{"models", Json::array({model})}}.dump()},
        {"POST", "/api/v1/models/load", 200U, R"({"instance_id":"coding-instance","status":"loaded"})"},
        {"GET", "/api/v1/models", 200U, Json{{"models", Json::array({loaded})}}.dump()},
        {"GET", "/api/v1/models", 200U, Json{{"models", Json::array({loaded})}}.dump()}}};
    Domain::ManagerSettings settings;
    settings.localModelPort = server.port();
    InfrastructureWindows::WindowsModelPreparation preparation;
    auto result = take(preparation.prepare(settings,
        operationContext("12121212-1212-4212-8212-121212121211", 5s)));
    REQUIRE(result.identifier == "coding-instance");
    REQUIRE(result.contextCapacity == 32768U);
    REQUIRE(result.modelLoaded);
    REQUIRE(!result.serverStarted);
    settings.localModelName = result.identifier;
    auto reused = take(preparation.prepare(settings,
        operationContext("12121212-1212-4212-8212-121212121212", 5s)));
    REQUIRE(reused.identifier == result.identifier);
    REQUIRE(!reused.modelLoaded);
    REQUIRE(server.waitUntilHandled(4U, 5s));
    const auto requests = server.requests();
    REQUIRE(Json::parse(requests[1].body).at("model") == "coding-model");
    REQUIRE(Json::parse(requests[1].body).at("context_length") == 32768U);
    server.requireHealthy();
}

void automaticModelPreparationRejectsUnusableAndMalformedInventory()
{
    LoopbackHttpServer server{{
        {"GET", "/api/v1/models", 200U, R"({"models":[{"type":"embedding","key":"not-a-language-model"}]})"},
        {"GET", "/api/v1/models", 200U, "{malformed"},
        {"GET", "/api/v1/models", 401U, R"({"error":"authentication required"})"}}};
    Domain::ManagerSettings settings;
    settings.localModelPort = server.port();
    InfrastructureWindows::WindowsModelPreparation preparation;
    for (int attempt = 0; attempt != 3; ++attempt) {
        auto result = preparation.prepare(settings,
            operationContext("12121212-1212-4212-8212-121212121213", 5s));
        REQUIRE(!result);
    }
    REQUIRE(server.waitUntilHandled(3U, 5s));
    REQUIRE(server.requests().size() == 3U);
    server.requireHealthy();
}

void automaticSetupUsesRealManagerAndPersistsProject()
{
    namespace App = ForgeConductor::Hosts::App;
    namespace W = InfrastructureWindows;
    const auto reply = [](const char* id) {
        return Json{{"id", id}, {"status", "completed"}, {"output_text", "OK"},
            {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}}.dump();
    };
    const auto inventory = R"({"models":[{"type":"llm","key":"test-model","capabilities":{"trained_for_tool_use":true},"loaded_instances":[{"id":"test-model","config":{"context_length":32768}}]}]})";
    const auto models = R"({"data":[{"id":"test-model"}]})";
    LoopbackHttpServer server{{
        {"GET", "/api/v1/models", 200U, inventory},
        {"GET", "/v1/models", 200U, models},
        {"POST", "/v1/responses", 200U, reply("setup-check-1")},
        {"GET", "/api/v1/models", 200U, inventory},
        {"GET", "/v1/models", 200U, models},
        {"POST", "/v1/responses", 200U, reply("setup-check-2")},
        {"GET", "/v1/models", 200U, models},
        {"POST", "/v1/responses", 200U,
            R"({"id":"first-task-tool","status":"completed","output":[{"type":"function_call","name":"fs_write","call_id":"setup-first-write","arguments":"{\"path\":\"setup-proof.txt\",\"content\":\"Project setup completed real native work.\"}"}],"usage":{"input_tokens":20,"output_tokens":3}})"},
        {"POST", "/v1/responses", 200U, reply("first-task")}}};
    std::array<wchar_t, 32768> executable{};
    REQUIRE(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) > 0);
    const auto root = std::filesystem::path{executable.data()}.parent_path().parent_path().parent_path().parent_path().parent_path() /
        (L"setup-proof-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto project = root / L"project";
    std::filesystem::create_directories(project);
    const auto profile = take(W::WindowsAlphaManagerProfile::create((root / L"profile").wstring()));
    App::ManagerConnection connection{std::wstring{profile.nativeDataRoot()}};
    const auto started = connection.start({});
    auto settings = connection.providerSettings({});
    if (!settings.loaded) throw std::runtime_error{started + " " + settings.message};
    const auto context = [] { return operationContext("12121212-1212-4212-8212-121212121299", 10s); };
    auto clock = std::make_shared<W::SystemClock>();
    auto identity = take(W::WindowsCurrentUserIdentity::load());
    W::WindowsManagerInstanceLeaseOptions options;
    options.purposeSuffix = profile.purposeSuffix();
    auto names = take(W::WindowsManagerInstanceLease::namesFor(identity, options));
    W::DpapiSecureStorage secure{std::wstring{profile.secureStorageRegistrySubkey()}};
    W::WindowsManagerAuthenticationTokenGenerator generator;
    W::WindowsManagerAuthenticationTokenStore tokens{secure, generator};
    auto nonce = take(tokens.load(context()));
    REQUIRE(nonce.has_value());
    auto client = take(W::WindowsManagerNamedPipeClient::create(clock, std::wstring{names.pipeName()}, *nonce));
    struct Cleanup {
        W::WindowsManagerNamedPipeClient& client;
        ~Cleanup() { (void)client.requestShutdown(operationContext("12121212-1212-4212-8212-121212121298", 10s)); }
    } cleanup{*client};
    Domain::ManagerSettingsPatch patch;
    patch.localModelPort = server.port();
    const auto saved = take(client->updateSettings(patch, true, context()));
    REQUIRE(saved.settings.localModelPort == server.port());
    REQUIRE(client->control({Domain::ManagerControlAction::Stop}, context()));
    const auto folderBytes = project.u8string();
    const std::string folder{reinterpret_cast<const char*>(folderBytes.data()), folderBytes.size()};
    const auto prepared = connection.prepareProject(folder, {});
    if (!prepared.ready) {
        std::string detail;
        for (const auto& check : prepared.checks) detail += check.detail + "\n";
        throw std::runtime_error{"Real Manager setup failed: " + detail};
    }
    REQUIRE(!prepared.projectId.empty());
    REQUIRE(prepared.model == "test-model");
    REQUIRE(take(client->status(context())).serviceActive);
    const auto repeated = connection.prepareProject(folder, {});
    REQUIRE(repeated.ready);
    REQUIRE(repeated.projectId == prepared.projectId);
    const auto projects = connection.projects({});
    REQUIRE(projects.loaded && projects.snapshot);
    REQUIRE(projects.snapshot->projects.size() == 1U);
    const auto freshSettings = connection.providerSettings({});
    REQUIRE(freshSettings.loaded);
    REQUIRE(freshSettings.settings.localModelName == prepared.model);
    auto run = connection.startManagedRun(prepared.projectId,
        "forge-conductor-manager", 0U, "Write setup-proof.txt in this project.", true, {});
    if (!run.loaded) throw std::runtime_error{"First task failed: " + run.message};
    REQUIRE(run.snapshot.has_value());
    const auto runId = run.snapshot->record.runId.value();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (run.snapshot->record.state == Domain::ManagedRunState::Running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
        run = connection.controlManagedRun(runId, App::ManagedRunAction::Status, {});
        REQUIRE(run.loaded && run.snapshot);
    }
    if (run.snapshot->record.state != Domain::ManagedRunState::Completed) {
        server.requireHealthy();
        throw std::runtime_error{"First task did not complete: " + run.message +
            (run.snapshot->record.lastError ? " " + run.snapshot->record.lastError->message : "")};
    }
    REQUIRE(run.snapshot->record.state == Domain::ManagedRunState::Completed);
    REQUIRE(server.waitUntilHandled(9U, 5s));
    std::ifstream proof{project / L"setup-proof.txt", std::ios::binary};
    const std::string written{std::istreambuf_iterator<char>{proof}, std::istreambuf_iterator<char>{}};
    REQUIRE(written == "Project setup completed real native work.");
    const auto completion = Json::parse(server.requests().at(8).body);
    REQUIRE(completion.at("previous_response_id") == "first-task-tool");
    REQUIRE(completion.at("input").at(0).at("call_id") == "setup-first-write");
    namespace C = ForgeConductor::Contracts;
    const auto policyFolder = root / L"policy";
    std::filesystem::create_directories(policyFolder);
    { std::ofstream policy{policyFolder / "README.md"}; policy << "Review before editing. Only approved project paths may change."; }
    const auto policyBytes = policyFolder.generic_u8string();
    const std::string policySource{reinterpret_cast<const char*>(policyBytes.data()), policyBytes.size()};
    const auto projectId = parse<Domain::ProjectId>(prepared.projectId);
    const auto preview = take(client->projectPolicy({projectId, C::ProjectPolicyAction::Preview, policySource}, context()));
    const auto revision = Json::parse(preview.canonicalJson).at("revision").get<std::string>();
    REQUIRE(client->projectPolicy({projectId, C::ProjectPolicyAction::Adopt, {}, revision}, context()));
    auto denied = connection.invokeTool(prepared.projectId, "fs_write", R"({"path":"blocked.txt","content":"must not write"})", {});
    REQUIRE(!denied.loaded || !denied.snapshot || !denied.snapshot->ok);
    REQUIRE(!std::filesystem::exists(project / L"blocked.txt"));
    const auto index = connection.invokeTool(prepared.projectId, "project_policy.read", "{}", {});
    REQUIRE(index.loaded && index.snapshot && index.snapshot->ok);
    const auto document = connection.invokeTool(prepared.projectId, "project_policy.read", R"({"path":"README.md"})", {});
    REQUIRE(document.loaded && document.snapshot && document.snapshot->ok);
    REQUIRE(Json::parse(document.snapshot->canonicalPayload).at("content") == "Review before editing. Only approved project paths may change.");
    Json review{{"schema", 1}, {"accepted", true}, {"policy_revision", revision}, {"reviewer", "Integration fixture"},
        {"reviewed_at", "2026-09-23T00:00:00Z"}, {"evidence", "Controlled policy integration test"},
        {"unresolved_obligations", Json::array()}, {"source_coverage", Json::array({
            {{"path", "README.md"}, {"status", "read"}, {"evidence_or_reason", "Fixture review"}}})},
        {"write_paths", Json::array({"approved.txt"})}, {"prohibited_paths", Json::array()}, {"approved_calls", Json::array()}};
    REQUIRE(client->projectPolicy({projectId, C::ProjectPolicyAction::Review, {}, revision, review.dump()}, context()));
    const auto permitted = connection.invokeTool(prepared.projectId, "fs_write", R"({"path":"approved.txt","content":"reviewed scope"})", {});
    REQUIRE(permitted.loaded && permitted.snapshot && permitted.snapshot->ok);
    denied = connection.invokeTool(prepared.projectId, "fs_write", R"({"path":"blocked.txt","content":"must not write"})", {});
    REQUIRE(!denied.loaded || !denied.snapshot || !denied.snapshot->ok);
    REQUIRE(!std::filesystem::exists(project / L"blocked.txt"));
    denied = connection.invokeTool(prepared.projectId, "shell_exec", R"({"command":"Set-Content blocked.txt bypass"})", {});
    REQUIRE(!denied.loaded || !denied.snapshot || !denied.snapshot->ok);
    REQUIRE(!std::filesystem::exists(project / L"blocked.txt"));
    App::ManagerConnection reopened{std::wstring{profile.nativeDataRoot()}};
    const auto persistedPolicy = reopened.projectPolicy({projectId, C::ProjectPolicyAction::Inspect}, {});
    REQUIRE(persistedPolicy.loaded);
    REQUIRE(Json::parse(persistedPolicy.canonicalJson).at("review_accepted").get<bool>());
    server.requireHealthy();
    std::cout << "PASS automatic_setup.real_manager_project_retry_and_first_task " << root.string() << '\n';
}

void automaticModelPreparationCancelsPendingLoad()
{
    ResponseScript loading{"POST", "/api/v1/models/load", 200U, R"({"instance_id":"cancelled-model"})"};
    loading.blockUntilReleased = true;
    loading.allowClientDisconnect = true;
    LoopbackHttpServer server{{
        {"GET", "/api/v1/models", 200U, R"({"models":[{"type":"llm","key":"test-model","capabilities":{"trained_for_tool_use":true},"size_bytes":4096,"max_context_length":65536,"loaded_instances":[]}]})"},
        loading}};
    Domain::ManagerSettings settings;
    settings.localModelPort = server.port();
    InfrastructureWindows::WindowsModelPreparation preparation;
    std::stop_source cancellation;
    auto context = operationContext("12121212-1212-4212-8212-121212121296", 10s);
    context.cancellation = cancellation.get_token();
    std::optional<Domain::Result<InfrastructureWindows::PreparedLocalModel>> result;
    std::thread worker{[&] { result.emplace(preparation.prepare(settings, context)); }};
    const bool loadingStarted = server.waitForRequests(2U, 5s);
    const auto cancelledAt = std::chrono::steady_clock::now();
    cancellation.request_stop();
    worker.join();
    server.releaseBlockedResponses();
    REQUIRE(loadingStarted);
    REQUIRE(std::chrono::steady_clock::now() - cancelledAt < 2s);
    REQUIRE(result.has_value());
    requireError(*result, Domain::ErrorCodes::Cancelled);
    server.requireHealthy();
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
        automaticSetupUsesRealManagerAndPersistsProject();
        automaticModelPreparationCancelsPendingLoad();
        providerSettingsApplyToNewRunsAndPreserveExistingRuns();
        automaticModelPreparationUsesVerifiedInventory();
        automaticModelPreparationRejectsUnusableAndMalformedInventory();
        std::cout << "PASS automatic_setup.model_load_reuse_and_failure\n";
        loopbackConfigurationIsFailClosed();
        std::cout << "PASS winhttp_transport.loopback_configuration\n";
        createBootstrapAndQueryUseExactRoutes();
        std::cout << "PASS winhttp_transport.create_bootstrap_query\n";
        lmStudioResponsesUsesFreshRootToolOutputAndActualResponseId();
        std::cout << "PASS lmstudio_responses.fresh_root_tool_ack\n";
        lmStudioResponsesCompletesAnOrdinaryManagedTurn();
        std::cout << "PASS lmstudio_responses.ordinary_managed_turn\n";
        lmStudioResponsesCorrelatesManagedFunctionOutput();
        std::cout << "PASS lmstudio_responses.managed_function_output\n";
        malformedAndOversizedResponsesFailClosed();
        std::cout << "PASS winhttp_transport.response_validation_bounds\n";
        rateLimitUsageAndProviderCancellationAreExact();
        std::cout << "PASS winhttp_transport.rate_limit_usage_cancel\n";
        deadlineAndActiveCancellationAreBounded();
        std::cout << "PASS winhttp_transport.deadline_cancellation\n";
        shutdownClosesActiveAndFutureRequests();
        std::cout << "PASS winhttp_transport.shutdown\n";
        std::cout << "SUMMARY passed=14 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
