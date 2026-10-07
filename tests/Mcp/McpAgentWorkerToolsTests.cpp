#include "Mcp/McpAgentWorkerTools.h"
#include "ForgeConductor/Application/ManagedRunService.h"
#include "ForgeConductor/Application/ManagedRunWorkerPolicy.h"
#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/Mcp/McpToolRouter.h"
#include "ForgeConductor/NativeTools/Windows/WindowsDesktopArtifactService.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsReviewerRunStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")
namespace {
using namespace std::chrono_literals;
using namespace ForgeConductor::Tests;
using namespace ForgeConductor::Infrastructure::Windows;
namespace Domain = ForgeConductor::Domain;
namespace Contracts = ForgeConductor::Contracts;
namespace Application = ForgeConductor::Application;
namespace Mcp = ForgeConductor::Mcp;
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

Domain::OperationContext context() { return TestContext{}.active(); }
Domain::ProjectId project() { return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001"); }
Domain::PathText pathText(const std::filesystem::path& path) {
    return take(Domain::PathText::create(take(Detail::strictUtf16ToUtf8(path.native()))));
}
class Tree final {
public:
    Tree() {
        std::wstring temporary(32768U, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(), "Temporary directory lookup failed."); temporary.resize(length);
        WindowsUuidGenerator generator; const auto value = take(generator.next()).value();
        root = std::filesystem::path{temporary} / std::filesystem::path{"ForgeConductor.Reviewer." + value};
        require(std::filesystem::create_directory(root), "Reviewer fixture root creation failed.");
        storage = root / L"memory" / L"reviewer-runs";
        require(std::filesystem::create_directories(storage), "Private reviewer fixture directory creation failed.");
    }
    ~Tree() { std::error_code ignored; static_cast<void>(std::filesystem::remove_all(root, ignored)); }
    std::filesystem::path root, storage;
};
struct StorageFixture final {
    Tree tree;
    WindowsWorkspaceAuthority authority{{WindowsWorkspaceAuthorityPolicy{
        parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project(), parse<Domain::ClientId>("reviewer-store-test"),
        {pathText(tree.root)}, Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create}, {}, false, 1U}}};
    WindowsAtomicFileStore files;
    BCryptSha256Hasher hasher;
    SystemClock clock;
    WindowsUuidGenerator uuids;
    Contracts::WorkspaceAuthority token{take(authority.authorityFor(project(), context()))};
    Contracts::AuthorizedPath directory{take(authority.authorize(token, {pathText(tree.storage), std::nullopt, Domain::FileAccess::Read, true}, context()))};
    std::filesystem::path path(const Domain::SessionId& id) const { return tree.storage / std::filesystem::path{id.value() + ".json"}; }
    ReviewerRunStorageResolver resolver() {
        return [this](const Domain::SessionId& id, const Domain::OperationContext& operation) -> Domain::Result<ReviewerRunStoragePaths> {
            const auto requested = pathText(path(id));
            auto read = authority.authorize(token, {requested, std::nullopt, Domain::FileAccess::Read, true}, operation);
            auto write = authority.authorize(token, {requested, std::nullopt, Domain::FileAccess::Write, true}, operation);
            auto create = authority.authorize(token, {requested, std::nullopt, Domain::FileAccess::Create, true}, operation);
            if (!read) return Domain::Result<ReviewerRunStoragePaths>::failure(read.error());
            if (!write) return Domain::Result<ReviewerRunStoragePaths>::failure(write.error());
            if (!create) return Domain::Result<ReviewerRunStoragePaths>::failure(create.error());
            return Domain::Result<ReviewerRunStoragePaths>::success({read.value(), write.value(), create.value()});
        };
    }
    std::unique_ptr<WindowsReviewerRunStore> store(ManagedReceiptPurpose purpose = ManagedReceiptPurpose::IndependentWorker) { return std::make_unique<WindowsReviewerRunStore>(files, hasher, clock, directory, resolver(), purpose); }
    Domain::ManagedRunRecord record() {
        Domain::ManagedRunRecord record{Domain::SessionId{take(uuids.next())}, project(), token.callerId(), "Review the complete original report.", 4U};
        record.state = Domain::ManagedRunState::Completed; record.readOnlyTools = true;
        record.providerResponseId = parse<Domain::ProviderSessionId>("actual-response-store-fixture");
        record.inputTokens = 991U; record.outputTokens = 311U; record.retainedContextTokens = 2001U;
        record.createdAt = clock.utcNow(); record.updatedAt = record.createdAt;
        record.outputText = "Verified review fixture report.";
        return record;
    }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary}; require(static_cast<bool>(stream), "Reviewer fixture read failed.");
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc}; require(static_cast<bool>(stream), "Reviewer fixture write failed.");
    stream.write(text.data(), static_cast<std::streamsize>(text.size())); require(static_cast<bool>(stream), "Reviewer fixture write failed.");
}
class Agents final : public Contracts::IAgentCatalog {
public:
    Domain::Result<std::vector<Domain::AgentSpec>> all(const Domain::OperationContext&) noexcept override { return Domain::Result<std::vector<Domain::AgentSpec>>::success({}); }
    Domain::Result<std::optional<Domain::AgentSpec>> get(const Domain::AgentId& id, const Domain::OperationContext&) noexcept override {
        if (id.value() != "fixture-specialist") return Domain::Result<std::optional<Domain::AgentSpec>>::success(std::nullopt);
        Domain::AgentSpec spec{id, "Fixture specialist", "Test only", {}, {}, {}, {}, {}, {}, {}, {}, "Use the supplied file task."};
        return Domain::Result<std::optional<Domain::AgentSpec>>::success(std::move(spec));
    }
    Domain::Result<Domain::AgentSpec> recommend(std::string_view, const Domain::OperationContext&) noexcept override { return Domain::Result<Domain::AgentSpec>::failure(Domain::makeError(Domain::ErrorCodes::AgentNotFound, "Unused fixture recommendation.")); }
};
class Catalog final : public Contracts::IToolCatalog {
public:
    Catalog() {
        for (const auto& name : {"fs_write", "agent_spawn", "workspace_authority_bind", "schedule_create", "session_checkpoint", "session_handoff"})
            descriptors.push_back({Domain::ToolDescriptor{name, "Fixture operation", "fixture", Domain::ToolEffect::Write,
                Domain::ToolAvailability::Available, true, false}, R"({"type":"object","properties":{"path":{"type":"string"}}})"});
    }
    std::span<const Domain::McpToolDescriptor> tools() const noexcept override { return descriptors; }
    std::vector<Domain::McpToolDescriptor> descriptors;
};
class Router final : public Contracts::IToolRouter {
public:
    explicit Router(Contracts::IWorkspaceAuthority& authority) : issuer{authority} {}
    Domain::Result<Domain::ToolCallOutcome> invoke(const Domain::ToolCallRequest& request, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& operation) noexcept override {
        try {
            ++calls;
            if (request.toolName != "fs_write") return Domain::Result<Domain::ToolCallOutcome>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized, "Unexpected fixture tool."));
            lastRoots = authority.trustedRoots(); lastGrants = authority.grants();
            const auto args = Json::parse(request.canonicalArguments);
            const auto path = take(Domain::PathText::create(args.at("path").get<std::string>()));
            auto authorized = issuer.authorize(authority, {path, std::nullopt, Domain::FileAccess::Write, true}, operation);
            if (!authorized) return Domain::Result<Domain::ToolCallOutcome>::failure(authorized.error());
            write(std::filesystem::path{take(Detail::strictUtf8ToUtf16(authorized.value().canonicalPath().value()))}, "actual-worker-mutation\xCE\xA9");
            return Domain::Result<Domain::ToolCallOutcome>::success({{request.metadata.requestId, request.toolName, true, std::nullopt, 1ms}, "{\"ok\":true,\"written\":true}", std::nullopt, std::nullopt});
        } catch (...) { return Domain::Result<Domain::ToolCallOutcome>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Fixture mutation failed.")); }
    }
    void cancel(const Domain::OperationId&) noexcept override { ++cancels; }
    void shutdown() noexcept override {}
    Contracts::IWorkspaceAuthority& issuer;
    std::size_t calls{}, cancels{};
    std::vector<Domain::PathText> lastRoots;
    std::vector<Domain::FileAccess> lastGrants;
};

class NativeImageRouter final : public Contracts::IToolRouter {
public:
    explicit NativeImageRouter(ForgeConductor::NativeTools::Windows::WindowsDesktopArtifactService& service,
        const int malformed = 0) : service_{service}, malformed_{malformed} {}
    Domain::Result<Domain::ToolCallOutcome> invoke(const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& operation) noexcept override {
        try {
            auto output = service_.execute(request.toolName, request.canonicalArguments, authority, operation);
            if (!output) return Domain::Result<Domain::ToolCallOutcome>::failure(output.error());
            ++calls;
            nativePayload = output.value();
            auto payload = Json::parse(nativePayload);
            if (malformed_ == 1) payload.erase("image_mime_type");
            else if (malformed_ == 2) payload["image_mime_type"] = "image/jpeg";
            return Domain::Result<Domain::ToolCallOutcome>::success({
                {request.metadata.requestId, request.toolName, true, std::nullopt, 1ms}, payload.dump(), std::nullopt, std::nullopt});
        } catch (...) { return Domain::Result<Domain::ToolCallOutcome>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The native image fixture router failed.")); }
    }
    void cancel(const Domain::OperationId&) noexcept override {}
    void shutdown() noexcept override {}
    std::size_t calls{};
    std::string nativePayload;
private:
    ForgeConductor::NativeTools::Windows::WindowsDesktopArtifactService& service_;
    int malformed_{};
};
class InvocationGuard final : public Contracts::IToolInvocationGuard {
public:
    Domain::Result<Domain::ToolInvocationAdmission> beforeInvoke(const Domain::ToolCallRequest&,
        const Domain::ToolDescriptor&, const Domain::OperationContext&) noexcept override {
        return Domain::Result<Domain::ToolInvocationAdmission>::success({});
    }
    Domain::Result<Domain::ToolCallOutcome> afterInvoke(const Domain::ToolCallRequest&,
        const Domain::ToolDescriptor&, Domain::Result<Domain::ToolCallOutcome> outcome,
        const Domain::OperationContext&) noexcept override { return outcome; }
    void cancel(const Domain::OperationId&) noexcept override {}
    void shutdown() noexcept override {}
};
class AuditRepository final : public Contracts::IAuditRepository {
public:
    Domain::Result<void> append(const Domain::AuditEvent&, const Domain::OperationContext&) noexcept override {
        ++count;
        return Domain::Result<void>::success();
    }
    Domain::Result<std::vector<Domain::AuditEvent>> recent(std::size_t,
        const Domain::OperationContext&) noexcept override {
        return Domain::Result<std::vector<Domain::AuditEvent>>::success({});
    }
    void close() noexcept override {}
    std::atomic_size_t count{};
};
class ParentLeaseHandler final : public Contracts::IToolHandler {
public:
    ParentLeaseHandler(Catalog& catalog, Agents& agents, StorageFixture& fixture, Router& mutation,
        LoopbackHttpServer& server) : catalog_{catalog}, agents_{agents}, fixture_{fixture}, mutation_{mutation}, server_{server} {}
    std::span<const Domain::McpToolDescriptor> tools() const noexcept override { return catalog_.tools(); }
    Domain::Result<Domain::ToolCallOutcome> handle(const Contracts::AuthorizedToolCall& authorized,
        const Contracts::WorkspaceAuthority& authority, const Domain::OperationContext& operation) noexcept override {
        try {
            if (authorized.toolName() == "fs_write") {
                require(router->activeOperationCount() == 2U, "Worker call did not overlap its parent's router lease.");
                require(operation.operationId != parentOperation, "Worker retained the parent operation ID.");
                auto result = mutation_.invoke(authorized.request(), authority, operation);
                overlapped.store(true, std::memory_order_release);
                return result;
            }
            parentOperation = operation.operationId;
            Mcp::AgentWorkerToolDependencies deps{[this] { return runs; }, agents_, catalog_, fixture_.uuids};
            auto result = Mcp::executeAgentWorkerTool("agent_spawn", authorized.canonicalRequest(), authority, operation, deps);
            if (!result) return Domain::Result<Domain::ToolCallOutcome>::failure(result.error());
            require(server_.waitUntilHandled(3U, 2s), "Worker did not finish its tool turn while agent_spawn remained active.");
            require(router->activeOperationCount() == 1U, "Parent router lease was released before the worker finished its tool call.");
            return Domain::Result<Domain::ToolCallOutcome>::success({
                {authorized.requestId(), authorized.toolName(), true, std::nullopt, 1ms},
                std::move(result).value(), std::nullopt, std::nullopt});
        } catch (const std::exception& error) {
            return Domain::Result<Domain::ToolCallOutcome>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure, error.what()));
        }
    }
    Mcp::McpToolRouter* router{};
    Application::ManagedRunService* runs{};
    std::atomic_bool overlapped{};
private:
    Catalog& catalog_;
    Agents& agents_;
    StorageFixture& fixture_;
    Router& mutation_;
    LoopbackHttpServer& server_;
    Domain::OperationId parentOperation{context().operationId};
};
LMStudioResponsesTransportConfiguration transportConfig(const LoopbackHttpServer& server) {
    LMStudioResponsesTransportConfiguration config; config.port = server.port(); config.model = "loopback-worker-fixture";
    config.receiveTimeout = 5s; return config;
}
std::string reply(std::string id, std::string output) {
    return Json{{"id", id}, {"status", "completed"}, {"output_text", output}, {"usage", {{"input_tokens", 7U}, {"output_tokens", 3U}}}}.dump();
}
ResponseScript modelInventory() {
    return {"GET", "/v1/models", 200U, Json{{"data", Json::array({{{"id", "loopback-worker-fixture"}}})}}.dump()};
}
Json tool(std::string id, std::string name, Json args) { return {{"type", "function_call"}, {"name", name}, {"call_id", id}, {"arguments", args.dump()}}; }
Json call(std::string_view name, Json args, const Contracts::WorkspaceAuthority& authority, const Mcp::AgentWorkerToolDependencies& deps) {
    auto operation = context(); operation.operationId = Domain::OperationId{take(deps.uuids.next())};
    return Json::parse(take(Mcp::executeAgentWorkerTool(name, args.dump(), authority, operation, deps)));
}
Json terminal(std::string id, const Contracts::WorkspaceAuthority& authority, const Mcp::AgentWorkerToolDependencies& deps) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        auto status = call("agent_poll", {{"run_id", id}}, authority, deps);
        if (status["state"] != "running" && status["state"] != "cancelling") return status;
        require(std::chrono::steady_clock::now() < deadline, "Worker did not reach a terminal state.");
        std::this_thread::sleep_for(5ms);
    }
}
void excludesManagedImageAnalysisFromWorkerCatalogs() {
    require(!Application::isManagedWorkerToolPermitted("image_analyze"),
        "An independent worker could recursively start Manager-owned image analysis.");
    Catalog catalog;
    catalog.descriptors.push_back({Domain::ToolDescriptor{"image_analyze", "Analyze an authorized image.", "fixture",
        Domain::ToolEffect::Write, Domain::ToolAvailability::Available, true, false}, R"({"type":"object"})"});
    catalog.descriptors.push_back({Domain::ToolDescriptor{"image_read", "Read an authorized image.", "fixture",
        Domain::ToolEffect::Read, Domain::ToolAvailability::Available, true, false}, R"({"type":"object"})"});
    for (const bool readOnly : {false, true}) {
        const auto names = Application::managedWorkerToolNames(catalog, readOnly);
        require(std::find(names.begin(), names.end(), "image_analyze") == names.end(),
            "Manager-owned image analysis was advertised to an independent worker.");
        require(std::find(names.begin(), names.end(), "image_read") != names.end(),
            "The worker policy removed authorized image reading with Manager-owned analysis.");
        require((std::find(names.begin(), names.end(), "fs_write") != names.end()) == !readOnly,
            "Image analysis filtering changed the worker's existing read-only mutation policy.");
    }
}
void dispatchesWorkerWhileParentRouterLeaseRemainsActive() {
    StorageFixture fixture; auto store = fixture.store(); Agents agents; Catalog catalog;
    std::erase_if(catalog.descriptors, [](const auto& descriptor) {
        return descriptor.tool.name != "agent_spawn" && descriptor.tool.name != "fs_write";
    });
    Router mutation{fixture.authority};
    const auto destination = fixture.tree.root / L"overlapping-parent-worker.txt";
    write(destination, "before worker");
    const auto first = Json{{"id", "overlapping-worker-tool"}, {"status", "completed"},
        {"output", Json::array({tool("write", "fs_write", {{"path", pathText(destination).value()}})})}}.dump();
    LoopbackHttpServer server{{modelInventory(), {"POST", "/v1/responses", 200U, first},
        {"POST", "/v1/responses", 200U, reply("overlapping-worker-final", "Actual write complete.")}}};
    LMStudioResponsesTransport transport{transportConfig(server)};
    ParentLeaseHandler handler{catalog, agents, fixture, mutation, server};
    Mcp::McpToolAuthorizer authorizer{fixture.clock}; InvocationGuard guard; AuditRepository audit;
    const std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    auto router = take(Mcp::McpToolRouter::create(catalog, handlers, authorizer, guard, audit, fixture.hasher, fixture.clock));
    Application::ManagedRunService runs{transport, *store, fixture.clock, {&catalog, router.get(), &fixture.authority}};
    handler.router = router.get(); handler.runs = &runs;
    auto operation = context(); operation.operationId = Domain::OperationId{take(fixture.uuids.next())};
    const Domain::ToolCallRequest parent{{parse<Domain::RequestId>(take(fixture.uuids.next()).value()), operation.correlationId,
        fixture.token.callerId(), fixture.token.projectId(), "2025-03-26"}, "agent_spawn",
        Json{{"task", "Write the fixture while its parent's invocation is active."}, {"authorization", "Fixture owner"}}.dump()};
    const auto spawned = Json::parse(take(router->invoke(parent, fixture.token, operation)).canonicalPayload);
    Mcp::AgentWorkerToolDependencies deps{[&] { return &runs; }, agents, catalog, fixture.uuids};
    const auto final = terminal(spawned.at("run_id").get<std::string>(), fixture.token, deps);
    require(final["state"] == "completed" && handler.overlapped.load(std::memory_order_acquire) &&
        mutation.calls == 1U && read(destination) == "actual-worker-mutation\xCE\xA9",
        "Spawned worker collided with the active parent operation instead of performing its authorized write.");
    require(server.requests()[2].body.find("already active") == std::string::npos && audit.count.load() == 2U,
        "Worker returned an operation-ID collision or bypassed the actual router's audit path.");
    runs.shutdown(); server.requireHealthy();
}
void mutatesWithFreshContextAndReconnectsSealedOutput() {
    StorageFixture fixture; auto store = fixture.store(); Agents agents; Catalog catalog; Router router{fixture.authority};
    const auto destination = fixture.tree.root / L"actual-worker.txt";
    write(destination, "before worker");
    const auto first = Json{{"id", "worker-real-tool"}, {"status", "completed"}, {"output", Json::array({tool("write", "fs_write", {{"path", pathText(destination).value()}}),
        tool("recursive", "agent_spawn", Json::object()), tool("escalate", "workspace_authority_bind", Json::object()),
        tool("checkpoint", "session_checkpoint", Json::object()), tool("handoff", "session_handoff", Json::object())})},
        {"usage", {{"input_tokens", 11U}, {"output_tokens", 2U}}}}.dump();
    const std::string report = std::string(8192U, 'x') + "\xCE\xA9\xE2\x82\xAC";
    LoopbackHttpServer server{{modelInventory(), {"POST", "/v1/responses", 200U, first}, {"POST", "/v1/responses", 200U, reply("worker-real-final", report)}}};
    LMStudioResponsesTransport transport{transportConfig(server)};
    Application::ManagedRunService runs{transport, *store, fixture.clock, {&catalog, &router, &fixture.authority}};
    Mcp::AgentWorkerToolDependencies deps{[&] { return &runs; }, agents, catalog, fixture.uuids};
    const auto started = call("agent_spawn", {{"task", "Write the supplied fixture and report actual results."}, {"authorization", "Fixture owner authorization"}, {"agent_id", "fixture-specialist"}}, fixture.token, deps);
    const auto id = started.at("run_id").get<std::string>();
    const auto final = terminal(id, fixture.token, deps);
    require(final["state"] == "completed" && final["evidence_integrity"] == "verified" && final["input_tokens"] == 18U && final["output_tokens"] == 5U,
        "Real provider usage, completion, or seal was lost. State: " + final["state"].dump() + "; error: " + final["error"].dump());
    require(read(destination) == "actual-worker-mutation\xCE\xA9" && router.calls == 1U && router.lastRoots == fixture.token.trustedRoots() && router.lastGrants == fixture.token.grants(),
        "Worker did not perform exactly its inherited authorized mutation.");
    require(server.waitUntilHandled(3U, 2s), "Provider did not receive the second turn.");
    const auto requests = server.requests(); const auto opening = Json::parse(requests[1].body); const auto followup = Json::parse(requests[2].body);
    require(!opening.contains("previous_response_id") || opening["previous_response_id"].is_null(), "Worker received an executor conversation.");
    require(requests[1].body.find("Use the supplied file task.") != std::string::npos && opening["tools"].size() == 1U, "Worker playbook or bounded tool advertisement was wrong.");
    require(opening["tools"].front()["name"] == "fs_write", "A legacy continuity mutator was advertised to the independent worker.");
    require(followup["previous_response_id"] == "worker-real-tool" && requests[2].body.find("unauthorized") != std::string::npos,
        "Actual blocked fabricated tools were not returned to the provider.");
    for (const auto callId : {"checkpoint", "handoff"}) {
        const auto& outputs = followup.at("input");
        const auto denied = std::find_if(outputs.begin(), outputs.end(), [&](const auto& value) { return value.at("call_id") == callId; });
        require(denied != outputs.end() && Json::parse(denied->at("output").get<std::string>()).at("error").at("code") ==
            std::string{Domain::ErrorCodes::Unauthorized}, "The forged legacy continuity call reached a router or lacked an explicit denied result.");
    }
    runs.shutdown();
    auto restartedStore = fixture.store();
    Application::ManagedRunService reconnected{transport, *restartedStore, fixture.clock, {&catalog, &router, &fixture.authority}};
    Mcp::AgentWorkerToolDependencies readback{[&] { return &reconnected; }, agents, catalog, fixture.uuids};
    const auto recovered = call("agent_poll", {{"run_id", id}}, fixture.token, readback);
    require(recovered["output"] == report && recovered["evidence_sha256"] == final["evidence_sha256"] && recovered["worker_interrupted"] == false && server.requests().size() == 3U,
        "Reconnect truncated, replayed, or changed a sealed worker outcome.");
    const auto unicodePage = call("agent_poll", {{"run_id", id}, {"output_offset", 8192U}, {"max_output_bytes", 2U}}, fixture.token, readback);
    require(unicodePage["output"] == "\xCE\xA9" && unicodePage["next_output_offset"] == 8194U, "Worker pagination split Unicode.");
    auto tooSmall = Mcp::executeAgentWorkerTool("agent_poll", Json{{"run_id", id}, {"output_offset", 8194U}, {"max_output_bytes", 1U}}.dump(), fixture.token, context(), readback);
    requireError(tooSmall, Domain::ErrorCodes::InvalidRequest, "A too-small Unicode page did not report lack of progress.");
    reconnected.shutdown(); server.requireHealthy();
}
void narrowedReadScopeDoesNotRegainWrite() {
    StorageFixture fixture; auto store = fixture.store(); Agents agents; Catalog catalog; Router router{fixture.authority};
    const auto destination = fixture.tree.root / L"narrowed-existing.txt"; write(destination, "owner original content");
    const auto narrow = take(fixture.authority.narrow(fixture.token, fixture.token.trustedRoots(),
        {Domain::FileAccess::Read}, false, fixture.token.generation() + 1U, context()));
    const auto first = Json{{"id", "worker-narrowed-tool"}, {"status", "completed"}, {"output", Json::array({
        tool("narrowed-write", "fs_write", {{"path", pathText(destination).value()}})})},
        {"usage", {{"input_tokens", 10U}, {"output_tokens", 2U}}}}.dump();
    LoopbackHttpServer server{{modelInventory(), {"POST", "/v1/responses", 200U, first},
        {"POST", "/v1/responses", 200U, reply("worker-narrowed-final", "The attempted write was denied.")}}};
    LMStudioResponsesTransport transport{transportConfig(server)};
    Application::ManagedRunService runs{transport, *store, fixture.clock, {&catalog, &router, &fixture.authority}};
    Mcp::AgentWorkerToolDependencies deps{[&] { return &runs; }, agents, catalog, fixture.uuids};
    const auto started = call("agent_spawn", {{"task", "Observe the narrowed native write boundary."}, {"authorization", "Fixture owner"}}, narrow, deps);
    const auto id = started.at("run_id").get<std::string>(); const auto final = terminal(id, narrow, deps);
    require(final["state"] == "completed" && final["evidence_integrity"] == "verified" && read(destination) == "owner original content" &&
        router.calls == 1U && router.lastGrants == std::vector<Domain::FileAccess>{Domain::FileAccess::Read},
        "Resolving the wider owner issuer restored a Write grant to the narrowed worker. State: " + final["state"].dump() + "; error: " + final["error"].dump());
    const auto record = take(runs.status(parse<Domain::SessionId>(id), context())).record;
    require(record.workerScope && record.workerScope->grants == narrow.grants() && record.workerScope->trustedRoots == narrow.trustedRoots() &&
        record.authorityGeneration == narrow.generation() && !record.workerScope->shellEnabled,
        "The exact frozen native caller scope was not retained in the sealed worker receipt.");
    require(server.waitUntilHandled(3U, 2s) && server.requests()[2].body.find("unauthorized") != std::string::npos,
        "The provider did not receive the actual native denied write result.");
    runs.shutdown(); server.requireHealthy();
}
void nativeImageReadReachesManagedVisionContent() {
    for (const int variant : {0, 1, 2, 3}) {
        const bool reviewer = variant == 1;
        StorageFixture fixture;
        auto store = fixture.store(reviewer ? ManagedReceiptPurpose::ReadOnlyReviewer : ManagedReceiptPurpose::IndependentWorker);
        ForgeConductor::NativeTools::Windows::WindowsDesktopArtifactService images{fixture.authority, fixture.files};
        const auto imagePath = pathText(fixture.tree.root / L"managed-image.png").value();
        const auto created = Json::parse(take(images.execute("image_write", Json{{"path", imagePath},
            {"width", 320}, {"height", 180}, {"background", "#FFFFFF"}, {"elements", Json::array({
                Json{{"type", "rectangle"}, {"x", 12}, {"y", 18}, {"width", 32}, {"height", 24}, {"color", "#FF0000"}}})}}.dump(),
            fixture.token, context())));
        require(created.at("ok") == true, "The native managed-vision fixture was not created.");
        Catalog catalog;
        catalog.descriptors = {{Domain::ToolDescriptor{"image_read", "Read an authorized image.", "fixture",
            Domain::ToolEffect::Read, Domain::ToolAvailability::Available, true, false},
            R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"}};
        catalog.descriptors.push_back({Domain::ToolDescriptor{"image_analyze", "Start independent image analysis.", "fixture",
            Domain::ToolEffect::Write, Domain::ToolAvailability::Available, true, false}, R"({"type":"object"})"});
        NativeImageRouter router{images, variant > 1 ? variant - 1 : 0};
        const auto first = Json{{"id", "native-image-tool"}, {"status", "completed"},
            {"output", Json::array({tool("native-image-call", "image_read", {{"path", imagePath}}),
                tool("recursive-image-call", "image_analyze", {{"path", imagePath}, {"authorization", "Forged nested analysis"}})})},
            {"usage", {{"input_tokens", 10U}, {"output_tokens", 2U}}}}.dump();
        LoopbackHttpServer server{{modelInventory(), {"POST", "/v1/responses", 200U, first},
            {"POST", "/v1/responses", 200U, reply("native-image-final", "The provider fixture accepted the tool result.")}}};
        LMStudioResponsesTransport transport{transportConfig(server)};
        Application::ManagedRunService runs{transport, *store, fixture.clock, {&catalog, &router, &fixture.authority}};
        auto operation = context(); operation.operationId = Domain::OperationId{take(fixture.uuids.next())};
        Domain::ManagedRunStartRequest request{Domain::SessionId{take(fixture.uuids.next())}, project(), fixture.token.callerId(),
            operation.operationId, operation.correlationId, fixture.token.generation(), "Inspect the native fixture image.", true, false, reviewer, 600U};
        if (!reviewer) request.workerScope = Domain::ManagedRunWorkerScope{fixture.token.trustedRoots(), fixture.token.grants(),
            fixture.token.denials(), fixture.token.shellEnabled(), Application::managedWorkerToolNames(catalog), 600U};
        const auto admitted = take(runs.start(request, operation));
        require(admitted.record.runId == request.runId && admitted.record.state == Domain::ManagedRunState::Running &&
            admitted.record.readOnlyTools == reviewer, "Native image run admission lost its identity, state, or review mode.");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        auto final = take(runs.status(request.runId, context()));
        while (final.record.state == Domain::ManagedRunState::Running || final.record.state == Domain::ManagedRunState::Cancelling) {
            require(std::chrono::steady_clock::now() < deadline, "The managed native image loop did not terminate.");
            std::this_thread::sleep_for(5ms); final = take(runs.status(request.runId, context()));
        }
        require(final.record.state == Domain::ManagedRunState::Completed && !final.record.lastError && router.calls == 1U,
            "Native image promotion failed or repeated the tool effect.");
        require(server.waitUntilHandled(3U, 2s), "The image-bearing provider continuation was not received.");
        const auto requests = server.requests();
        const auto opening = Json::parse(requests[1].body);
        require(opening.at("tools").size() == 1U && opening.at("tools").front().at("name") == "image_read",
            "Independent image analysis was recursively advertised to a managed worker or read-only reviewer.");
        const auto followup = Json::parse(requests[2].body);
        require(followup.at("previous_response_id") == "native-image-tool" && followup.at("input").size() == 2U,
            "Managed image output lost its originating provider identity or created an unrelated user message.");
        const auto& denied = followup.at("input").at(1U);
        require(denied.at("call_id") == "recursive-image-call" && denied.at("output").is_string() &&
            Json::parse(denied.at("output").get<std::string>()).at("error").at("code") == std::string{Domain::ErrorCodes::Unauthorized},
            "A fabricated image analysis call reached the native router or lacked an explicit denied result.");
        const auto& output = followup.at("input").front();
        require(output.at("type") == "function_call_output" && output.at("call_id") == "native-image-call",
            "Managed image output lost its exact function call identity.");
        if (variant > 1) {
            require(output.at("output").is_string() && Json::parse(output.at("output").get<std::string>()).at("error").at("code") ==
                std::string{Domain::ErrorCodes::InternalFailure}, "Invalid or missing native image MIME silently became a successful text-only tool result.");
        } else {
            const auto& content = output.at("output");
            require(content.is_array() && content.size() == 2U && content[0].at("type") == "input_text" &&
                content[1].at("type") == "input_image" && content[1].at("detail") == "auto",
                "Native tool preview was sent as base64 text instead of image content.");
            const auto metadata = Json::parse(content[0].at("text").get<std::string>());
            auto original = Json::parse(router.nativePayload);
            const auto encoded = original.at("image_base64").get<std::string>();
            original.erase("image_base64"); original["image_content_block"] = true;
            require(metadata == original && metadata.at("image_mime_type") == "image/png" && metadata.at("width") == 320 &&
                metadata.at("height") == 180 && metadata.at("preview_width") == 256 && metadata.at("preview_height") == 144,
                "Native image promotion changed or discarded path, MIME, or original/preview dimensions.");
            require(content[1].at("image_url") == "data:image/png;base64," + encoded,
                "The exact WIC PNG preview bytes did not reach the provider as image data.");
        }
        runs.shutdown(); server.requireHealthy();
    }
}
void admitsTwoIndependentWorkersAndCancelsExactRun() {
    StorageFixture fixture; auto store = fixture.store(); Agents agents; Catalog catalog; Router router{fixture.authority};
    ResponseScript first{"POST", "/v1/responses", 200U, reply("parallel-one", "first independent result")}; first.blockUntilReleased = true; first.allowClientDisconnect = true;
    LoopbackHttpServer server{{modelInventory(), first, {"POST", "/v1/responses", 200U, reply("parallel-two", "second independent result")}}};
    LMStudioResponsesTransport transport{transportConfig(server)};
    Application::ManagedRunService runs{transport, *store, fixture.clock, {&catalog, &router, &fixture.authority}};
    Mcp::AgentWorkerToolDependencies deps{[&] { return &runs; }, agents, catalog, fixture.uuids};
    const auto one = call("agent_spawn", {{"task", "First independent task"}, {"authorization", "Fixture owner"}}, fixture.token, deps);
    require(server.waitForRequests(2U, 2s), "First worker never submitted its HTTP request. Status: " +
        call("agent_poll", {{"run_id", one["run_id"]}}, fixture.token, deps).dump());
    const auto two = call("agent_spawn", {{"task", "Second independent task"}, {"authorization", "Fixture owner"}}, fixture.token, deps);
    require(one["run_id"] != two["run_id"] && call("agent_poll", {{"run_id", two["run_id"]}}, fixture.token, deps)["state"] == "running",
        "Second worker could not be admitted while the first provider request was active.");
    const auto cancelled = call("agent_cancel", {{"run_id", one["run_id"]}, {"authorization", "Cancel only first fixture run"}}, fixture.token, deps);
    require(cancelled["cancellation_requested"] == true, "Cancellation was not attached to the first run.");
    const auto stopped = terminal(one["run_id"].get<std::string>(), fixture.token, deps);
    require(stopped["state"] == "cancelled" && stopped["evidence_integrity"] == "verified", "Active worker cancellation was not sealed.");
    server.releaseBlockedResponses();
    const auto second = terminal(two["run_id"].get<std::string>(), fixture.token, deps);
    require(second["state"] == "completed" && second["output"] == "second independent result", "Cancelling one worker cancelled or contaminated the second.");
    require(server.waitUntilHandled(3U, 2s), "Second independent provider request was not received.");
    const auto requests = server.requests();
    for (const auto& request : requests) {
        if (request.method != "POST") continue;
        const auto encoded = Json::parse(request.body);
        require(!encoded.contains("previous_response_id") || encoded["previous_response_id"].is_null(), "Parallel worker inherited another provider context.");
    }
    require(requests[1].body.find("First independent task") != std::string::npos && requests[2].body.find("Second independent task") != std::string::npos,
        "Independent worker tasks were mixed.");
    runs.shutdown(); server.requireHealthy();
}
void enforcesTotalBudgetAndOwnership() {
    StorageFixture fixture; auto store = fixture.store(); Agents agents; Catalog catalog; Router router{fixture.authority};
    ResponseScript delayed{"POST", "/v1/responses", 200U, reply("late-worker", "must not complete")}; delayed.delay = 2500ms; delayed.allowClientDisconnect = true;
    LoopbackHttpServer server{{modelInventory(), delayed}}; LMStudioResponsesTransport transport{transportConfig(server)};
    Application::ManagedRunService runs{transport, *store, fixture.clock, {&catalog, &router, &fixture.authority}};
    Mcp::AgentWorkerToolDependencies deps{[&] { return &runs; }, agents, catalog, fixture.uuids};
    const auto start = std::chrono::steady_clock::now();
    const auto spawned = call("agent_spawn", {{"task", "Bounded fixture wait"}, {"authorization", "Fixture owner"}, {"timeout_sec", 1U}}, fixture.token, deps);
    const auto final = terminal(spawned["run_id"].get<std::string>(), fixture.token, deps);
    require(final["state"] == "failed" && final["error"]["code"].get<std::string>() == Domain::ErrorCodes::DeadlineExceeded && final["worker_interrupted"] == false &&
        std::chrono::steady_clock::now() - start < 1800ms, "Total worker timeout accepted late success or masqueraded as interruption. State: " + final["state"].dump() + "; error: " + final["error"].dump());
    WindowsWorkspaceAuthority other{{WindowsWorkspaceAuthorityPolicy{
        parse<Domain::AuthorityId>("30000000-0000-4000-8000-000000000001"), project(), parse<Domain::ClientId>("different-worker-owner"),
        fixture.token.trustedRoots(), Domain::FileAccess::Read, {Domain::FileAccess::Read}, {}, false, 1U}}};
    const auto otherToken = take(other.authorityFor(project(), context()));
    requireError(Mcp::executeAgentWorkerTool("agent_poll", Json{{"run_id", spawned["run_id"]}}.dump(), otherToken, context(), deps),
        Domain::ErrorCodes::OwnershipConflict, "Another caller read a private worker result.");
    requireError(Mcp::executeAgentWorkerTool("agent_cancel", Json{{"run_id", spawned["run_id"]}, {"authorization", "Other owner"}}.dump(), otherToken, context(), deps),
        Domain::ErrorCodes::OwnershipConflict, "Another caller cancelled a private worker.");
    for (const auto& args : std::vector<Json>{{{"task", "task"}}, {{"task", "task"}, {"authorization", "owner"}, {"timeout_sec", 0U}},
        {{"task", "task"}, {"authorization", "owner"}, {"timeout_sec", 3601U}}})
        requireError(Mcp::executeAgentWorkerTool("agent_spawn", args.dump(), fixture.token, context(), deps), Domain::ErrorCodes::InvalidRequest,
            "Worker admission accepted missing authorization or an invalid budget.");
    runs.shutdown(); server.requireHealthy();
}
} // namespace
int main() {
    TestRegistry tests;
    addTest(tests, "workers.image_analysis_policy", excludesManagedImageAnalysisFromWorkerCatalogs);
    addTest(tests, "workers.real_http_mutation_reconnect", mutatesWithFreshContextAndReconnectsSealedOutput);
    addTest(tests, "workers.parent_router_lease_overlap", dispatchesWorkerWhileParentRouterLeaseRemainsActive);
    addTest(tests, "workers.narrowed_native_scope", narrowedReadScopeDoesNotRegainWrite);
    addTest(tests, "workers.native_image_vision_content", nativeImageReadReachesManagedVisionContent);
    addTest(tests, "workers.parallel_exact_cancellation", admitsTwoIndependentWorkersAndCancelsExactRun);
    addTest(tests, "workers.budget_owner_validation", enforcesTotalBudgetAndOwnership);
    std::size_t passed{};
    for (const auto& [name, test] : tests) { try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; } }
    std::cout << "SUMMARY passed=" << passed << " failed=" << (tests.size() - passed) << '\n';
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}
