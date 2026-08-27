#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Mcp/McpJsonCodec.h"
#include "ForgeConductor/Mcp/McpProtocol.h"
#include "ForgeConductor/Mcp/McpServer.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
using Json = nlohmann::json;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename Id>
[[nodiscard]] Id id(const std::string_view value)
{
    return take(Id::parse(value));
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return {};
    }
};

class SequenceUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            std::ostringstream text;
            text << "00000000-0000-0000-0000-"
                 << std::hex << std::setw(12) << std::setfill('0') << next_++;
            return Domain::Uuid::parse(text.str());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The UUID test sequence failed."));
        }
    }

    [[nodiscard]] std::size_t consumed() const noexcept
    {
        std::lock_guard lock{mutex_};
        return next_ - 1U;
    }

private:
    mutable std::mutex mutex_;
    std::size_t next_{1U};
};

class ResolverFake final
    : public Contracts::IMcpExecutionContextResolver,
      private Contracts::IWorkspaceAuthority {
public:
    ResolverFake()
        : authorityId_{id<Domain::AuthorityId>(
              "10000000-0000-0000-0000-000000000001")},
          projectId_{id<Domain::ProjectId>(
              "20000000-0000-0000-0000-000000000002")},
          root_{take(Domain::PathText::create("C:\\workspace"))}
    {
    }

    bool fail{};
    bool waitForCancellation{};

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> resolve(
        const Domain::ToolCallRequest& request,
        const Domain::ToolEffect effect,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            lastRequest_ = request;
            lastEffect_ = effect;
            lastContext_ = context;
            if (waitForCancellation) {
                std::unique_lock lock{waitMutex_};
                resolving_ = true;
                waitChanged_.notify_all();
                const std::stop_callback cancellationCallback{
                    context.cancellation,
                    [this] { waitChanged_.notify_all(); }};
                waitChanged_.wait(lock, [&] {
                    return context.isCancellationRequested();
                });
            }
            if (fail) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The scripted authority resolver denied the call."));
            }
            const auto intent = effect == Domain::ToolEffect::Read
                ? Domain::FileAccess::Read
                : Domain::FileAccess::Write;
            return issueAuthority(
                authorityId_,
                request.metadata.projectId.value_or(projectId_),
                request.metadata.clientId,
                {root_},
                intent,
                {Domain::FileAccess::Read,
                 Domain::FileAccess::Write,
                 Domain::FileAccess::Create,
                 Domain::FileAccess::Delete,
                 Domain::FileAccess::Execute},
                {},
                true,
                ++generation_);
        } catch (...) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted authority resolver failed safely."));
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    void waitUntilResolving()
    {
        std::unique_lock lock{waitMutex_};
        if (!waitChanged_.wait_for(
                lock,
                std::chrono::seconds{5},
                [&] { return resolving_; })) {
            throw std::runtime_error{
                "The authority resolver did not start."};
        }
    }

    [[nodiscard]] const std::optional<Domain::ToolCallRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

private:
    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "Unused."));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "Unused."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "Unused."));
    }

    Domain::AuthorityId authorityId_;
    Domain::ProjectId projectId_;
    Domain::PathText root_;
    std::optional<Domain::ToolCallRequest> lastRequest_;
    std::optional<Domain::OperationContext> lastContext_;
    Domain::ToolEffect lastEffect_{Domain::ToolEffect::Read};
    std::mutex waitMutex_;
    std::condition_variable waitChanged_;
    bool resolving_{};
    std::uint64_t generation_{};
    std::size_t calls_{};
};

class RouterFake final : public Contracts::IToolRouter {
public:
    enum class Mode {
        Success,
        Failure,
        WaitForCancellation,
        MalformedPayload,
        OversizedPayload,
        DeepPayload,
        OversizedEnvelope,
    };

    explicit RouterFake(std::deque<Mode> modes = {})
        : modes_{std::move(modes)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            Mode mode{Mode::Success};
            {
                std::unique_lock lock{mutex_};
                ++calls_;
                lastRequest_ = request;
                lastGeneration_ = authority.generation();
                started_ = true;
                startedChanged_.notify_all();
                if (!modes_.empty()) {
                    mode = modes_.front();
                    modes_.pop_front();
                }
                if (mode == Mode::WaitForCancellation) {
                    cancelledChanged_.wait(lock, [&] {
                        return shutdown_ || context.isCancellationRequested() ||
                            cancelledOperation_ == context.operationId;
                    });
                }
            }
            if (mode == Mode::Failure) {
                return Domain::Result<Domain::ToolCallOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The scripted router rejected the call."));
            }
            if (context.isCancellationRequested()) {
                return Domain::Result<Domain::ToolCallOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The scripted router observed cancellation."));
            }
            std::string payload{R"({"value":7,"ok":true})"};
            if (mode == Mode::MalformedPayload) {
                payload = "{";
            } else if (mode == Mode::OversizedPayload) {
                payload.assign(
                    Mcp::McpJsonCodec::MaximumDocumentBytes + 1U,
                    'x');
            } else if (mode == Mode::DeepPayload) {
                payload.clear();
                for (std::size_t index{};
                     index < Mcp::McpJsonCodec::MaximumNestingDepth + 2U;
                     ++index) {
                    payload.append("{\"a\":");
                }
                payload.push_back('0');
                payload.append(
                    Mcp::McpJsonCodec::MaximumNestingDepth + 2U,
                    '}');
            } else if (mode == Mode::OversizedEnvelope) {
                payload = Json{
                    {"value", std::string(600'000U, 'x')}}.dump();
            }
            return Domain::Result<Domain::ToolCallOutcome>::success(
                Domain::ToolCallOutcome{
                    Domain::ToolExecutionReceipt{
                        request.metadata.requestId,
                        request.toolName,
                        true,
                        std::nullopt,
                        std::chrono::milliseconds{1}},
                    std::move(payload)});
        } catch (...) {
            return Domain::Result<Domain::ToolCallOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted router failed safely."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            cancelledOperation_ = operationId;
            ++cancelCalls_;
            cancelledChanged_.notify_all();
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        shutdown_ = true;
        cancelledChanged_.notify_all();
    }

    void waitUntilStarted()
    {
        std::unique_lock lock{mutex_};
        if (!startedChanged_.wait_for(
                lock,
                std::chrono::seconds{5},
                [&] { return started_; })) {
            throw std::runtime_error{"The scripted router did not start."};
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return cancelCalls_;
    }

    [[nodiscard]] bool shutdownCalled() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdown_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable startedChanged_;
    std::condition_variable cancelledChanged_;
    std::deque<Mode> modes_;
    std::optional<Domain::ToolCallRequest> lastRequest_;
    std::optional<Domain::OperationId> cancelledOperation_;
    std::uint64_t lastGeneration_{};
    std::size_t calls_{};
    std::size_t cancelCalls_{};
    bool started_{};
    bool shutdown_{};
};

struct Inbound final {
    std::optional<std::string> frame;
    std::optional<Domain::Error> error;

    [[nodiscard]] static Inbound json(std::string value)
    {
        return Inbound{std::move(value), std::nullopt};
    }

    [[nodiscard]] static Inbound failure(Domain::Error value)
    {
        return Inbound{std::nullopt, std::move(value)};
    }
};

class ScriptedTransport final : public Contracts::IMcpTransport {
public:
    ScriptedTransport(
        std::vector<Inbound> inbound,
        const std::size_t responsesBeforeEof,
        std::function<void(std::size_t)> beforeReceive = {})
        : inbound_{std::move(inbound)},
          responsesBeforeEof_{responsesBeforeEof},
          beforeReceive_{std::move(beforeReceive)}
    {
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::McpFrame>> receive(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::size_t inputIndex{};
            {
                std::lock_guard lock{mutex_};
                inputIndex = nextInbound_;
                receiveEntered_ = true;
                receiveChanged_.notify_all();
            }
            if (beforeReceive_) {
                beforeReceive_(inputIndex);
            }

            std::unique_lock lock{mutex_};
            if (nextInbound_ < inbound_.size()) {
                auto input = std::move(inbound_[nextInbound_++]);
                if (input.error) {
                    return Domain::Result<std::optional<Domain::McpFrame>>::failure(
                        std::move(input.error.value()));
                }
                return Domain::Result<std::optional<Domain::McpFrame>>::success(
                    std::optional<Domain::McpFrame>{
                        Domain::McpFrame{std::move(input.frame.value())}});
            }
            if (!outputChanged_.wait_for(
                    lock,
                    std::chrono::seconds{10},
                    [&] {
                        return shutdown_ ||
                            outbound_.size() >= responsesBeforeEof_;
                    })) {
                return Domain::Result<std::optional<Domain::McpFrame>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::DeadlineExceeded,
                        "The scripted transport timed out."));
            }
            return Domain::Result<std::optional<Domain::McpFrame>>::success(
                std::nullopt);
        } catch (...) {
            return Domain::Result<std::optional<Domain::McpFrame>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted transport receive failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<void> send(
        const Domain::McpFrame& frame,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            outbound_.push_back(frame.utf8Json);
            outputChanged_.notify_all();
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The scripted transport send failed safely."));
        }
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        shutdown_ = true;
        outputChanged_.notify_all();
    }

    [[nodiscard]] std::vector<std::string> outbound() const
    {
        std::lock_guard lock{mutex_};
        return outbound_;
    }

    [[nodiscard]] bool shutdownCalled() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdown_;
    }

    void waitUntilReceiving()
    {
        std::unique_lock lock{mutex_};
        if (!receiveChanged_.wait_for(
                lock,
                std::chrono::seconds{5},
                [&] { return receiveEntered_; })) {
            throw std::runtime_error{
                "The scripted transport did not enter receive."};
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable outputChanged_;
    std::condition_variable receiveChanged_;
    std::vector<Inbound> inbound_;
    std::vector<std::string> outbound_;
    std::size_t responsesBeforeEof_{};
    std::size_t nextInbound_{};
    std::function<void(std::size_t)> beforeReceive_;
    bool receiveEntered_{};
    bool shutdown_{};
};

struct SessionResult final {
    Domain::Result<void> result;
    std::vector<std::string> output;
    bool transportShutdown{};
};

[[nodiscard]] SessionResult serve(
    Contracts::IToolCatalog& catalog,
    RouterFake& router,
    ResolverFake& resolver,
    SequenceUuidGenerator& uuids,
    const Domain::McpRole role,
    std::vector<Inbound> inbound,
    const std::size_t responses,
    std::function<void(std::size_t)> beforeReceive = {})
{
    FixedClock clock;
    ScriptedTransport transport{
        std::move(inbound), responses, std::move(beforeReceive)};
    Mcp::McpServer server{catalog, router, resolver, uuids, clock};
    const Domain::OperationContext context{
        id<Domain::OperationId>("30000000-0000-0000-0000-000000000003"),
        Domain::MonotonicTimePoint{} + std::chrono::minutes{5},
        {},
        id<Domain::CorrelationId>("test-correlation")};
    auto result = server.run(
        transport,
        role,
        id<Domain::DeploymentId>("test-deployment"),
        id<Domain::ClientId>("test-client"),
        context);
    return SessionResult{
        std::move(result),
        transport.outbound(),
        transport.shutdownCalled()};
}

[[nodiscard]] Json parse(const std::string& value)
{
    REQUIRE(value.find('\n') == std::string::npos);
    return Json::parse(value);
}

[[nodiscard]] std::string request(
    const Json& identifier,
    const std::string_view method,
    Json params = Json())
{
    Json value{
        {"id", identifier},
        {"jsonrpc", "2.0"},
        {"method", method}};
    if (!params.is_null()) {
        value["params"] = std::move(params);
    }
    return value.dump();
}

[[nodiscard]] std::string notification(
    const std::string_view method,
    Json params = Json())
{
    Json value{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) {
        value["params"] = std::move(params);
    }
    return value.dump();
}

void testInitializeNegotiationAndRoles(Contracts::IToolCatalog& catalog)
{
    for (const auto role : {Domain::McpRole::Primary, Domain::McpRole::Fallback}) {
        RouterFake router;
        ResolverFake resolver;
        SequenceUuidGenerator uuids;
        std::vector<Inbound> inbound;
        std::size_t identifier{1U};
        for (const auto version : Mcp::McpProtocol::SupportedVersions) {
            inbound.push_back(Inbound::json(request(
                identifier++,
                "initialize",
                Json{{"protocolVersion", version}})));
        }
        inbound.push_back(Inbound::json(request(
            identifier,
            "initialize",
            Json{{"protocolVersion", "2099-01-01"}})));
        auto session = serve(
            catalog, router, resolver, uuids, role, std::move(inbound), 5U);
        REQUIRE(session.result.hasValue());
        REQUIRE(session.transportShutdown);
        REQUIRE(router.shutdownCalled());
        REQUIRE(session.output.size() == 5U);
        for (std::size_t index{};
             index < Mcp::McpProtocol::SupportedVersions.size();
             ++index) {
            const auto response = parse(session.output[index]);
            REQUIRE(response.at("result").at("protocolVersion") ==
                    std::string{Mcp::McpProtocol::SupportedVersions[index]});
        }
        const auto response = parse(session.output.front());
        REQUIRE(response.at("result").at("serverInfo").at("version") == "0.9.0");
        REQUIRE(response.at("result").at("serverInfo").at("name") ==
            (role == Domain::McpRole::Primary
                 ? "forge-conductor"
                 : "forge-conductor-fallback"));
        REQUIRE(response.at("result").at("capabilities").at("tools").at("listChanged") == false);
        REQUIRE(response.at("result").at("capabilities").at("projectMemory").at("capabilityVersion") == 1U);
        REQUIRE(parse(session.output.back()).at("result").at("protocolVersion") ==
                std::string{Mcp::McpProtocol::SupportedVersions.front()});
    }
}

void testMethodsNotificationsAndExactList(Contracts::IToolCatalog& catalog)
{
    RouterFake router;
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    auto session = serve(
        catalog,
        router,
        resolver,
        uuids,
        Domain::McpRole::Primary,
        {
            Inbound::json(request("string-id", "ping")),
            Inbound::json(notification("notifications/initialized")),
            Inbound::json(request(2, "tools/list")),
            Inbound::json(request(3, "resources/list")),
            Inbound::json(request(4, "prompts/list")),
            Inbound::json(request(5, "missing/method")),
            Inbound::json(notification("notifications/vendor-event")),
        },
        5U);
    REQUIRE(session.result.hasValue());
    REQUIRE(session.output.size() == 5U);
    REQUIRE(parse(session.output[0]).at("id").is_string());
    REQUIRE(parse(session.output[0]).at("id") == "string-id");
    const auto listed = parse(session.output[1]).at("result").at("tools");
    REQUIRE(listed.size() == Mcp::McpToolCatalog::ExpectedToolCount);
    REQUIRE(listed.front().at("name") == "agent_context");
    REQUIRE(listed.back().at("name") == "shell_exec");
    REQUIRE(parse(session.output[2]).at("result").at("resources").empty());
    REQUIRE(parse(session.output[3]).at("result").at("prompts").empty());
    REQUIRE(parse(session.output[4]).at("error").at("code") == -32601);
}

void testMalformedAndTransportRecovery(Contracts::IToolCatalog& catalog)
{
    std::string tooDeep;
    for (std::size_t index{};
         index < Mcp::McpJsonCodec::MaximumNestingDepth + 2U;
         ++index) {
        tooDeep.append("{\"a\":");
    }
    tooDeep.append("0");
    tooDeep.append(Mcp::McpJsonCodec::MaximumNestingDepth + 2U, '}');

    RouterFake router;
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    auto session = serve(
        catalog,
        router,
        resolver,
        uuids,
        Domain::McpRole::Primary,
        {
            Inbound::json("{"),
            Inbound::json(request(1, "ping")),
            Inbound::json(std::move(tooDeep)),
            Inbound::json("[1]"),
            Inbound::failure(Domain::makeError(
                Domain::ErrorCodes::MalformedMessage,
                "A malformed line was discarded.")),
            Inbound::json(request(2, "ping")),
            Inbound::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "An oversized line was discarded.")),
            Inbound::json(request(3, "ping")),
        },
        8U);
    REQUIRE(session.result.hasValue());
    REQUIRE(session.output.size() == 8U);
    REQUIRE(parse(session.output[0]).at("error").at("code") == -32700);
    REQUIRE(parse(session.output[1]).at("result").is_object());
    REQUIRE(parse(session.output[2]).at("error").at("code") == -32000);
    REQUIRE(parse(session.output[3]).at("error").at("code") == -32600);
    REQUIRE(parse(session.output[4]).at("error").at("code") == -32700);
    REQUIRE(parse(session.output[5]).at("id") == 2);
    REQUIRE(parse(session.output[6]).at("error").at("code") == -32000);
    REQUIRE(parse(session.output[7]).at("id") == 3);
}

void testToolSuccessFailureAndAuthority(Contracts::IToolCatalog& catalog)
{
    RouterFake router{{
        RouterFake::Mode::Success,
        RouterFake::Mode::Failure}};
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    auto session = serve(
        catalog,
        router,
        resolver,
        uuids,
        Domain::McpRole::Primary,
        {
            Inbound::json(request(
                1,
                "tools/call",
                Json{{"arguments", Json{{"z", 1}, {"a", 2}}},
                     {"name", "agent_list"}})),
            Inbound::json(request(
                "failure",
                "tools/call",
                Json{{"arguments", Json::object()},
                     {"name", "agent_list"}})),
        },
        2U);
    REQUIRE(session.result.hasValue());
    REQUIRE(router.calls() == 2U);
    REQUIRE(resolver.calls() == 2U);
    REQUIRE(uuids.consumed() == 6U);
    REQUIRE(resolver.lastRequest().has_value());
    REQUIRE(resolver.lastRequest()->metadata.requestId.value().size() == 36U);
    const auto success = parse(session.output[0]);
    REQUIRE(success.at("id").is_number());
    REQUIRE(success.at("result").at("isError") == false);
    REQUIRE(success.at("result").at("structuredContent").at("value") == 7);
    REQUIRE(Json::parse(success.at("result").at("content")[0].at("text").get<std::string>()) ==
            success.at("result").at("structuredContent"));
    const auto failure = parse(session.output[1]);
    REQUIRE(failure.at("id").is_string());
    REQUIRE(failure.at("result").at("isError") == true);
    REQUIRE(failure.at("result").at("structuredContent").at("code") ==
            std::string{Domain::ErrorCodes::InvalidRequest});

    RouterFake deniedRouter;
    ResolverFake deniedResolver;
    deniedResolver.fail = true;
    SequenceUuidGenerator deniedUuids;
    auto denied = serve(
        catalog,
        deniedRouter,
        deniedResolver,
        deniedUuids,
        Domain::McpRole::Primary,
        {Inbound::json(request(
            9,
            "tools/call",
            Json{{"arguments", Json::object()}, {"name", "agent_list"}}))},
        1U);
    REQUIRE(denied.result.hasValue());
    REQUIRE(deniedRouter.calls() == 0U);
    REQUIRE(parse(denied.output[0]).at("result").at("structuredContent").at("code") ==
            std::string{Domain::ErrorCodes::Unauthorized});

    RouterFake malformedScopeRouter;
    ResolverFake malformedScopeResolver;
    SequenceUuidGenerator malformedScopeUuids;
    auto malformedScope = serve(
        catalog,
        malformedScopeRouter,
        malformedScopeResolver,
        malformedScopeUuids,
        Domain::McpRole::Primary,
        {Inbound::json(request(
            10,
            "tools/call",
            Json{{"arguments", Json{{"project_id", 7}}},
                 {"name", "agent_list"}}))},
        1U);
    REQUIRE(malformedScope.result.hasValue());
    REQUIRE(malformedScopeResolver.calls() == 0U);
    REQUIRE(malformedScopeRouter.calls() == 0U);
    REQUIRE(parse(malformedScope.output[0])
                .at("result")
                .at("structuredContent")
                .at("code") ==
            std::string{Domain::ErrorCodes::InvalidRequest});
}

void testRouterPayloadBoundsAndRecovery(Contracts::IToolCatalog& catalog)
{
    RouterFake router{{
        RouterFake::Mode::MalformedPayload,
        RouterFake::Mode::OversizedPayload,
        RouterFake::Mode::DeepPayload,
        RouterFake::Mode::OversizedEnvelope}};
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    auto session = serve(
        catalog,
        router,
        resolver,
        uuids,
        Domain::McpRole::Primary,
        {
            Inbound::json(request(
                1,
                "tools/call",
                Json{{"arguments", Json::object()}, {"name", "agent_list"}})),
            Inbound::json(request(
                2,
                "tools/call",
                Json{{"arguments", Json::object()}, {"name", "agent_list"}})),
            Inbound::json(request(
                3,
                "tools/call",
                Json{{"arguments", Json::object()}, {"name", "agent_list"}})),
            Inbound::json(request(
                4,
                "tools/call",
                Json{{"arguments", Json::object()}, {"name", "agent_list"}})),
            Inbound::json(request(5, "ping")),
        },
        5U);
    REQUIRE(session.result.hasValue());
    REQUIRE(session.output.size() == 5U);
    REQUIRE(router.calls() == 4U);

    std::vector<Json> responses;
    responses.reserve(session.output.size());
    for (const auto& encoded : session.output) {
        REQUIRE(encoded.size() <= Mcp::McpJsonCodec::MaximumDocumentBytes);
        responses.push_back(parse(encoded));
    }
    const auto responseFor = [&](const int requestId) -> const Json& {
        const auto found = std::find_if(
            responses.begin(),
            responses.end(),
            [&](const Json& response) {
                return response.at("id") == requestId;
            });
        if (found == responses.end()) {
            throw std::runtime_error{"The expected MCP response was not sent."};
        }
        return *found;
    };
    for (const int requestId : {1, 2, 3, 4}) {
        const auto& response = responseFor(requestId);
        REQUIRE(response.at("result").at("isError") == true);
        REQUIRE(response.at("result").at("structuredContent").at("code") ==
                std::string{Domain::ErrorCodes::InternalFailure});
    }
    REQUIRE(responseFor(5).at("result").is_object());
}

void testBoundedIdentifiersAndNames(Contracts::IToolCatalog& catalog)
{
    static_assert(Mcp::McpServer::MaximumRequestIdBytes == 256U);
    static_assert(Mcp::McpServer::MaximumMethodNameBytes == 128U);
    static_assert(Mcp::McpServer::MaximumToolNameBytes == 128U);

    {
        RouterFake router;
        ResolverFake resolver;
        SequenceUuidGenerator uuids;
        auto session = serve(
            catalog,
            router,
            resolver,
            uuids,
            Domain::McpRole::Primary,
            {
                Inbound::json(notification(
                    "notifications/cancelled",
                    Json{{"requestId", 7}})),
                Inbound::json(request("7", "ping")),
                Inbound::json(request(7, "ping")),
            },
            2U);
        REQUIRE(session.result.hasValue());
        REQUIRE(parse(session.output[0]).at("result").is_object());
        REQUIRE(parse(session.output[1]).at("error").at("code") == -32800);
    }

    {
        RouterFake router;
        ResolverFake resolver;
        SequenceUuidGenerator uuids;
        const std::string maximumId(
            Mcp::McpServer::MaximumRequestIdBytes, 'm');
        const std::string oversizedId(
            Mcp::McpServer::MaximumRequestIdBytes + 1U, 'x');
        auto session = serve(
            catalog,
            router,
            resolver,
            uuids,
            Domain::McpRole::Primary,
            {
                Inbound::json(request(oversizedId, "ping")),
                Inbound::json(request(maximumId, "ping")),
            },
            2U);
        REQUIRE(session.result.hasValue());
        REQUIRE(parse(session.output[0]).at("error").at("code") == -32600);
        REQUIRE(parse(session.output[0]).at("id").is_null());
        REQUIRE(parse(session.output[1]).at("id") == maximumId);
    }

    {
        RouterFake router;
        ResolverFake resolver;
        SequenceUuidGenerator uuids;
        std::vector<Inbound> inbound;
        std::vector<std::string> identifiers;
        identifiers.reserve(Mcp::McpServer::MaximumPreCancellationIds + 1U);
        for (std::size_t index{};
             index <= Mcp::McpServer::MaximumPreCancellationIds;
             ++index) {
            auto value = std::to_string(index);
            value.append(
                Mcp::McpServer::MaximumRequestIdBytes - value.size(),
                'i');
            identifiers.push_back(std::move(value));
            inbound.push_back(Inbound::json(notification(
                "notifications/cancelled",
                Json{{"requestId", identifiers.back()}})));
        }
        inbound.push_back(Inbound::json(request(identifiers.front(), "ping")));
        inbound.push_back(Inbound::json(request(identifiers.back(), "ping")));
        auto session = serve(
            catalog,
            router,
            resolver,
            uuids,
            Domain::McpRole::Primary,
            std::move(inbound),
            2U);
        REQUIRE(session.result.hasValue());
        REQUIRE(parse(session.output[0]).at("result").is_object());
        REQUIRE(parse(session.output[1]).at("error").at("code") == -32800);
    }

    {
        RouterFake router;
        ResolverFake resolver;
        SequenceUuidGenerator uuids;
        const std::string hugeName(900'000U, 'n');
        auto session = serve(
            catalog,
            router,
            resolver,
            uuids,
            Domain::McpRole::Primary,
            {
                Inbound::json(request(1, hugeName)),
                Inbound::json(request(
                    2,
                    "tools/call",
                    Json{{"arguments", Json::object()},
                         {"name", hugeName}})),
                Inbound::json(request(3, "ping")),
            },
            3U);
        REQUIRE(session.result.hasValue());
        REQUIRE(parse(session.output[0]).at("error").at("code") == -32600);
        REQUIRE(parse(session.output[1]).at("error").at("code") == -32600);
        REQUIRE(parse(session.output[2]).at("result").is_object());
    }
}

void testPreCancellationAndActiveCancellation(Contracts::IToolCatalog& catalog)
{
    {
        RouterFake router;
        ResolverFake resolver;
        SequenceUuidGenerator uuids;
        auto session = serve(
            catalog,
            router,
            resolver,
            uuids,
            Domain::McpRole::Primary,
            {
                Inbound::json(notification(
                    "notifications/cancelled",
                    Json{{"request_id", "pre"}})),
                Inbound::json(request("pre", "ping")),
            },
            1U);
        REQUIRE(session.result.hasValue());
        REQUIRE(session.output.size() == 1U);
        REQUIRE(parse(session.output[0]).at("error").at("code") == -32800);
    }

    RouterFake router{{RouterFake::Mode::WaitForCancellation}};
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    auto session = serve(
        catalog,
        router,
        resolver,
        uuids,
        Domain::McpRole::Primary,
        {
            Inbound::json(request(
                17,
                "tools/call",
                Json{{"arguments", Json::object()}, {"name", "agent_list"}})),
            Inbound::json(notification(
                "notifications/cancelled",
                Json{{"requestId", 17}})),
        },
        1U,
        [&](const std::size_t index) {
            if (index == 1U) {
                router.waitUntilStarted();
            }
        });
    REQUIRE(session.result.hasValue());
    REQUIRE(router.calls() == 1U);
    REQUIRE(router.cancelCalls() >= 1U);
    REQUIRE(parse(session.output[0]).at("error").at("code") == -32800);

    RouterFake resolverRaceRouter;
    ResolverFake resolverRace;
    resolverRace.waitForCancellation = true;
    SequenceUuidGenerator resolverRaceUuids;
    auto resolverRaceSession = serve(
        catalog,
        resolverRaceRouter,
        resolverRace,
        resolverRaceUuids,
        Domain::McpRole::Primary,
        {
            Inbound::json(request(
                18,
                "tools/call",
                Json{{"arguments", Json::object()},
                     {"name", "agent_list"}})),
            Inbound::json(notification(
                "notifications/cancelled",
                Json{{"requestId", 18}})),
        },
        1U,
        [&](const std::size_t index) {
            if (index == 1U) {
                resolverRace.waitUntilResolving();
            }
        });
    REQUIRE(resolverRaceSession.result.hasValue());
    REQUIRE(resolverRace.calls() == 1U);
    REQUIRE(resolverRaceRouter.calls() == 0U);
    REQUIRE(parse(resolverRaceSession.output[0]).at("error").at("code") ==
            -32800);
}

void testQueueBoundAndCleanEofDrain(Contracts::IToolCatalog& catalog)
{
    static_assert(Mcp::McpServer::MaximumPendingToolCalls == 64U);
    static_assert(Mcp::McpServer::MaximumPreCancellationIds == 256U);

    RouterFake router{{RouterFake::Mode::WaitForCancellation}};
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    std::vector<Inbound> inbound;
    for (std::size_t identifier{1U}; identifier <= 66U; ++identifier) {
        inbound.push_back(Inbound::json(request(
            identifier,
            "tools/call",
            Json{{"arguments", Json::object()}, {"name", "agent_list"}})));
    }
    inbound.push_back(Inbound::json(notification(
        "notifications/cancelled", Json{{"requestId", 1}})));

    auto session = serve(
        catalog,
        router,
        resolver,
        uuids,
        Domain::McpRole::Primary,
        std::move(inbound),
        66U,
        [&](const std::size_t index) {
            if (index == 1U) {
                router.waitUntilStarted();
            }
        });
    REQUIRE(session.result.hasValue());
    REQUIRE(session.output.size() == 66U);
    REQUIRE(router.calls() == 65U);
    std::size_t capacityErrors{};
    std::size_t cancellationErrors{};
    for (const auto& encoded : session.output) {
        const auto response = parse(encoded);
        if (!response.contains("error")) {
            continue;
        }
        if (response.at("error").at("code") == -32000) {
            ++capacityErrors;
        }
        if (response.at("error").at("code") == -32800) {
            ++cancellationErrors;
        }
    }
    REQUIRE(capacityErrors == 1U);
    REQUIRE(cancellationErrors == 1U);
    REQUIRE(session.transportShutdown);
    REQUIRE(router.shutdownCalled());
}

void testDestructorStopsActiveRun(Contracts::IToolCatalog& catalog)
{
    FixedClock clock;
    RouterFake router;
    ResolverFake resolver;
    SequenceUuidGenerator uuids;
    ScriptedTransport transport{{}, 1U};
    auto server = std::make_unique<Mcp::McpServer>(
        catalog, router, resolver, uuids, clock);
    auto* activeServer = server.get();
    const Domain::OperationContext context{
        id<Domain::OperationId>("40000000-0000-0000-0000-000000000004"),
        Domain::MonotonicTimePoint{} + std::chrono::minutes{5},
        {},
        id<Domain::CorrelationId>("destructor-test")};
    std::promise<void> admitted;
    auto running = std::async(std::launch::async, [&] {
        admitted.set_value();
        return activeServer->run(
            transport,
            Domain::McpRole::Primary,
            id<Domain::DeploymentId>("test-deployment"),
            id<Domain::ClientId>("test-client"),
            context);
    });
    admitted.get_future().wait();
    transport.waitUntilReceiving();

    const auto started = std::chrono::steady_clock::now();
    server.reset();
    REQUIRE(std::chrono::steady_clock::now() - started <
            std::chrono::seconds{2});
    REQUIRE(running.wait_for(std::chrono::seconds{2}) ==
            std::future_status::ready);
    REQUIRE(running.get().hasValue());
    REQUIRE(transport.shutdownCalled());
    REQUIRE(router.shutdownCalled());
}

} // namespace

int main()
{
    try {
        auto catalog = take(Mcp::McpToolCatalog::create());
        testInitializeNegotiationAndRoles(*catalog);
        testMethodsNotificationsAndExactList(*catalog);
        testMalformedAndTransportRecovery(*catalog);
        testToolSuccessFailureAndAuthority(*catalog);
        testRouterPayloadBoundsAndRecovery(*catalog);
        testBoundedIdentifiersAndNames(*catalog);
        testPreCancellationAndActiveCancellation(*catalog);
        testQueueBoundAndCleanEofDrain(*catalog);
        testDestructorStopsActiveRun(*catalog);
        std::cout << "MCP protocol server tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP protocol server tests failed after " << assertions
                  << " assertions: " << error.what() << '\n';
        return 1;
    }
}
