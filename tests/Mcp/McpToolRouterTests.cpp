#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Mcp/McpToolRouter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +     \
                                     #condition};                                \
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

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code)
{
    REQUIRE(!result.hasValue());
    REQUIRE(result.error().code == code);
}

template <typename Id>
[[nodiscard]] Id id(const std::string_view value)
{
    return take(Id::parse(value));
}

[[nodiscard]] Domain::McpToolDescriptor descriptor(
    std::string name,
    const Domain::ToolEffect effect = Domain::ToolEffect::Read,
    const bool requiresProject = true,
    const bool requiresShell = false)
{
    return Domain::McpToolDescriptor{
        Domain::ToolDescriptor{
            std::move(name),
            "Test tool descriptor.",
            "TestToolPack",
            effect,
            Domain::ToolAvailability::Available,
            requiresProject,
            requiresShell},
        R"({"type":"object"})"};
}

[[nodiscard]] Domain::ToolCallRequest requestFor(
    const Domain::McpToolDescriptor& tool,
    const std::size_t ordinal = 1U,
    const bool includeProject = true)
{
    const auto suffix = std::to_string(ordinal);
    return Domain::ToolCallRequest{
        Domain::McpRequestMetadata{
            id<Domain::RequestId>("request-" + suffix),
            id<Domain::CorrelationId>("correlation-" + suffix),
            id<Domain::ClientId>("client-a"),
            includeProject
                ? std::optional<Domain::ProjectId>{
                      id<Domain::ProjectId>(
                          "20000000-0000-0000-0000-000000000002")}
                : std::nullopt,
            "2025-11-25"},
        tool.tool.name,
        R"({"path":"C:\\secret\\input.txt","value":"sensitive"})"};
}

[[nodiscard]] Domain::OperationContext contextFor(
    const Domain::ToolCallRequest& request,
    const std::size_t ordinal = 1U,
    const std::stop_token cancellation = {},
    const Domain::MonotonicTimePoint deadline =
        Domain::MonotonicTimePoint{} + std::chrono::minutes{5})
{
    std::string value{"30000000-0000-0000-0000-"};
    auto digits = std::to_string(ordinal);
    value.append(12U - digits.size(), '0');
    value += digits;
    return Domain::OperationContext{
        id<Domain::OperationId>(value),
        deadline,
        cancellation,
        request.metadata.correlationId};
}

[[nodiscard]] Domain::ToolCallOutcome successOutcome(
    const Domain::ToolCallRequest& request,
    std::string payload = R"({"ok":true})")
{
    return Domain::ToolCallOutcome{
        Domain::ToolExecutionReceipt{
            request.metadata.requestId,
            request.toolName,
            true,
            std::nullopt,
            std::chrono::milliseconds{1}},
        std::move(payload)};
}

[[nodiscard]] Domain::ToolCallOutcome errorOutcome(
    const Domain::ToolCallRequest& request,
    const std::string_view code)
{
    return Domain::ToolCallOutcome{
        Domain::ToolExecutionReceipt{
            request.metadata.requestId,
            request.toolName,
            false,
            Domain::makeError(code, "Policy blocked the tool call."),
            std::chrono::milliseconds{}},
        std::string{"{\"ok\":false,\"code\":\""} +
            std::string{code} + "\"}"};
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{} + std::chrono::seconds{100};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        const auto call = monotonicCalls_.fetch_add(
            1U, std::memory_order_relaxed) + 1U;
        const auto seconds = call > advanceAfterCalls_.load(
            std::memory_order_relaxed)
            ? advancedSeconds_.load(std::memory_order_relaxed)
            : 100;
        return Domain::MonotonicTimePoint{} + std::chrono::seconds{seconds};
    }

    void advanceAfterMonotonicCalls(
        const std::size_t completedCalls,
        const std::int64_t seconds) noexcept
    {
        advancedSeconds_.store(seconds, std::memory_order_relaxed);
        advanceAfterCalls_.store(completedCalls, std::memory_order_relaxed);
    }

private:
    mutable std::atomic_size_t monotonicCalls_{};
    std::atomic_size_t advanceAfterCalls_{
        (std::numeric_limits<std::size_t>::max)()};
    std::atomic_int64_t advancedSeconds_{100};
};

class FixedHasher final : public Contracts::IHasher {
public:
    bool fail{};

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        const std::span<const std::byte> bytes) noexcept override
    {
        ++calls_;
        lastByteCount_.store(bytes.size(), std::memory_order_release);
        if (fail) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted hasher failed."));
        }
        return Domain::Sha256Digest::parse(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_.load(); }
    [[nodiscard]] std::size_t lastByteCount() const noexcept
    {
        return lastByteCount_.load(std::memory_order_acquire);
    }

private:
    std::atomic_size_t calls_{};
    std::atomic_size_t lastByteCount_{};
};

class AuditRepositoryFake final : public Contracts::IAuditRepository {
public:
    bool fail{};

    [[nodiscard]] Domain::Result<void> append(
        const Domain::AuditEvent& event,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++appendCalls_;
            if (fail) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::StorageFull,
                    "The scripted audit store failed."));
            }
            events_.push_back(event);
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The scripted audit store failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> recent(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto count = (std::min)(maximumCount, events_.size());
            return Domain::Result<std::vector<Domain::AuditEvent>>::success(
                std::vector<Domain::AuditEvent>{
                    events_.end() - static_cast<std::ptrdiff_t>(count),
                    events_.end()});
        } catch (...) {
            return Domain::Result<std::vector<Domain::AuditEvent>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted audit query failed safely."));
        }
    }

    void close() noexcept override { closed_ = true; }

    [[nodiscard]] std::vector<Domain::AuditEvent> events() const
    {
        std::lock_guard lock{mutex_};
        return events_;
    }

    [[nodiscard]] std::size_t appendCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return appendCalls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Domain::AuditEvent> events_;
    std::size_t appendCalls_{};
    std::atomic_bool closed_{};
};

class CatalogFake final : public Contracts::IToolCatalog {
public:
    explicit CatalogFake(std::vector<Domain::McpToolDescriptor> tools)
        : tools_{std::move(tools)}
    {
    }

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept override
    {
        return tools_;
    }

private:
    std::vector<Domain::McpToolDescriptor> tools_;
};

class AuthorityIssuer final : private Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] Contracts::WorkspaceAuthority issue(
        const Domain::ToolCallRequest& request,
        const Domain::FileAccess intent,
        std::vector<Domain::FileAccess> grants,
        std::vector<Domain::FileAccess> denials = {},
        const bool shellEnabled = false,
        const std::uint64_t generation = 1U)
    {
        return take(issueAuthority(
            id<Domain::AuthorityId>(
                "10000000-0000-0000-0000-000000000001"),
            request.metadata.projectId.value_or(id<Domain::ProjectId>(
                "20000000-0000-0000-0000-000000000002")),
            request.metadata.clientId,
            {take(Domain::PathText::create("C:\\workspace"))},
            intent,
            std::move(grants),
            std::move(denials),
            shellEnabled,
            generation));
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
};

class AuthorizerFake final : public Contracts::IToolAuthorizer {
public:
    std::optional<Domain::Error> denial;
    bool mismatchedCapability{};

    [[nodiscard]] Domain::Result<Contracts::AuthorizedToolCall> authorize(
        const Domain::ToolAuthorizationRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            if (denial) {
                return Domain::Result<Contracts::AuthorizedToolCall>::failure(
                    *denial);
            }
            auto candidate = request;
            if (mismatchedCapability) {
                candidate.call.toolName += ".mismatched";
            }
            return issueAuthorizedToolCall(candidate, authority, context);
        } catch (...) {
            return Domain::Result<Contracts::AuthorizedToolCall>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted authorizer failed safely."));
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_.load(); }

private:
    std::atomic_size_t calls_{};
};

class InvocationGuardFake final : public Contracts::IToolInvocationGuard {
public:
    std::optional<Domain::Error> beforeFailure;
    std::optional<Domain::Error> afterFailure;
    std::optional<std::string> immediateErrorCode;
    std::optional<std::string> augmentedPayload;

    [[nodiscard]] Domain::Result<Domain::ToolInvocationAdmission> beforeInvoke(
        const Domain::ToolCallRequest& request,
        const Domain::ToolDescriptor&,
        const Domain::OperationContext&) noexcept override
    {
        try {
            ++beforeCalls_;
            if (beforeFailure) {
                return Domain::Result<Domain::ToolInvocationAdmission>::failure(
                    *beforeFailure);
            }
            Domain::ToolInvocationAdmission admission;
            if (immediateErrorCode) {
                admission.immediateOutcome = errorOutcome(
                    request, *immediateErrorCode);
            }
            return Domain::Result<Domain::ToolInvocationAdmission>::success(
                std::move(admission));
        } catch (...) {
            return Domain::Result<Domain::ToolInvocationAdmission>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted invocation guard failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> afterInvoke(
        const Domain::ToolCallRequest&,
        const Domain::ToolDescriptor&,
        Domain::Result<Domain::ToolCallOutcome> outcome,
        const Domain::OperationContext&) noexcept override
    {
        try {
            ++afterCalls_;
            if (afterFailure) {
                return Domain::Result<Domain::ToolCallOutcome>::failure(
                    *afterFailure);
            }
            if (outcome && augmentedPayload) {
                outcome.value().canonicalPayload = *augmentedPayload;
            }
            return outcome;
        } catch (...) {
            return Domain::Result<Domain::ToolCallOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted invocation guard failed safely."));
        }
    }

    void cancel(const Domain::OperationId&) noexcept override { ++cancelCalls_; }
    void shutdown() noexcept override { shutdownCalled_ = true; }

    [[nodiscard]] std::size_t beforeCalls() const noexcept
    {
        return beforeCalls_.load();
    }
    [[nodiscard]] std::size_t afterCalls() const noexcept
    {
        return afterCalls_.load();
    }
    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        return cancelCalls_.load();
    }
    [[nodiscard]] bool shutdownCalled() const noexcept
    {
        return shutdownCalled_.load();
    }

private:
    std::atomic_size_t beforeCalls_{};
    std::atomic_size_t afterCalls_{};
    std::atomic_size_t cancelCalls_{};
    std::atomic_bool shutdownCalled_{};
};

class HandlerFake final : public Contracts::IToolHandler {
public:
    explicit HandlerFake(std::vector<Domain::McpToolDescriptor> tools)
        : tools_{std::move(tools)}
    {
    }

    std::optional<Domain::Error> failure;
    bool blockUntilCancelled{};
    bool returnMismatchedReceipt{};

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept override
    {
        return tools_;
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> handle(
        const Contracts::AuthorizedToolCall& authorizedCall,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            {
                std::unique_lock lock{mutex_};
                ++calls_;
                lastGeneration_ = authority.generation();
                lastProjectId_ = authorizedCall.projectId();
                capabilityMatched_ =
                    authorizedCall.matches(authority, context) &&
                    authorizedCall.authorityId() == authority.authorityId();
                callsChanged_.notify_all();
                if (blockUntilCancelled) {
                    const std::stop_callback cancellationWake{
                        context.cancellation,
                        [this] { callsChanged_.notify_all(); }};
                    callsChanged_.wait(lock, [&] {
                        return context.isCancellationRequested();
                    });
                }
            }
            if (context.isCancellationRequested()) {
                return Domain::Result<Domain::ToolCallOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The scripted handler observed cancellation."));
            }
            if (failure) {
                return Domain::Result<Domain::ToolCallOutcome>::failure(*failure);
            }
            auto outcome = successOutcome(authorizedCall.request());
            if (returnMismatchedReceipt) {
                outcome.receipt.toolName += ".mismatched";
            }
            return Domain::Result<Domain::ToolCallOutcome>::success(
                std::move(outcome));
        } catch (...) {
            return Domain::Result<Domain::ToolCallOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The scripted handler failed safely."));
        }
    }

    void waitForCalls(const std::size_t expected)
    {
        std::unique_lock lock{mutex_};
        if (!callsChanged_.wait_for(
                lock,
                std::chrono::seconds{10},
                [&] { return calls_ >= expected; })) {
            throw std::runtime_error{
                "The scripted handler did not receive the expected calls."};
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] bool capabilityMatched() const noexcept
    {
        std::lock_guard lock{mutex_};
        return capabilityMatched_;
    }

    [[nodiscard]] std::uint64_t lastGeneration() const noexcept
    {
        std::lock_guard lock{mutex_};
        return lastGeneration_;
    }

    [[nodiscard]] std::optional<Domain::ProjectId> lastProjectId() const
    {
        std::lock_guard lock{mutex_};
        return lastProjectId_;
    }

private:
    std::vector<Domain::McpToolDescriptor> tools_;
    mutable std::mutex mutex_;
    std::condition_variable callsChanged_;
    std::size_t calls_{};
    std::uint64_t lastGeneration_{};
    std::optional<Domain::ProjectId> lastProjectId_;
    bool capabilityMatched_{};
};

[[nodiscard]] std::unique_ptr<Mcp::McpToolRouter> createRouter(
    CatalogFake& catalog,
    std::span<Contracts::IToolHandler* const> handlers,
    AuthorizerFake& authorizer,
    InvocationGuardFake& guard,
    AuditRepositoryFake& audit,
    FixedHasher& hasher,
    FixedClock& clock)
{
    return take(Mcp::McpToolRouter::create(
        catalog, handlers, authorizer, guard, audit, hasher, clock));
}

void registrationIsExactAndComplete()
{
    const auto read = descriptor("read_tool");
    const auto write = descriptor(
        "write_tool", Domain::ToolEffect::Write);
    CatalogFake catalog{{read, write}};
    HandlerFake exact{{read, write}};
    std::array<Contracts::IToolHandler*, 1U> exactHandlers{&exact};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, exactHandlers, authorizer, guard, audit, hasher, clock);
    REQUIRE(router != nullptr);

    std::array<Contracts::IToolHandler*, 2U> duplicateHandlers{&exact, &exact};
    auto duplicate = Mcp::McpToolRouter::create(
        catalog,
        duplicateHandlers,
        authorizer,
        guard,
        audit,
        hasher,
        clock);
    requireError(duplicate, Domain::ErrorCodes::Conflict);

    HandlerFake missing{{read}};
    std::array<Contracts::IToolHandler*, 1U> missingHandlers{&missing};
    auto incomplete = Mcp::McpToolRouter::create(
        catalog,
        missingHandlers,
        authorizer,
        guard,
        audit,
        hasher,
        clock);
    requireError(incomplete, Domain::ErrorCodes::IntegrityFailure);

    auto drifted = write;
    drifted.inputSchema = R"({"type":"object","additionalProperties":false})";
    HandlerFake mismatch{{read, drifted}};
    std::array<Contracts::IToolHandler*, 1U> mismatchHandlers{&mismatch};
    auto drift = Mcp::McpToolRouter::create(
        catalog,
        mismatchHandlers,
        authorizer,
        guard,
        audit,
        hasher,
        clock);
    requireError(drift, Domain::ErrorCodes::IntegrityFailure);
}

void routedAuthorityGuardAndAuditAreExact()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);

    const auto request = requestFor(tool);
    AuthorityIssuer issuer;
    const auto authority = issuer.issue(
        request,
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        {},
        false,
        9U);
    auto outcome = router->invoke(
        request, authority, contextFor(request));
    REQUIRE(outcome.hasValue());
    REQUIRE(outcome.value().receipt.ok);
    REQUIRE(outcome.value().canonicalPayload == R"({"ok":true})");
    REQUIRE(authorizer.calls() == 1U);
    REQUIRE(handler.calls() == 1U);
    REQUIRE(handler.capabilityMatched());
    REQUIRE(handler.lastGeneration() == 9U);
    REQUIRE(guard.beforeCalls() == 1U);
    REQUIRE(guard.afterCalls() == 1U);
    REQUIRE(hasher.calls() == 1U);
    REQUIRE(hasher.lastByteCount() == request.canonicalArguments.size());
    const auto events = audit.events();
    REQUIRE(events.size() == 1U);
    REQUIRE(events.front().tool == request.toolName);
    REQUIRE(events.front().clientId == request.metadata.clientId);
    REQUIRE(events.front().argumentsDigest.has_value());
    REQUIRE(events.front().status == "ok");
    REQUIRE(!events.front().error.has_value());
}

void resolvedAuthorityScopesProjectlessToolCall()
{
    const auto tool = descriptor("implicit_project_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);

    const auto projectless = requestFor(tool, 1U, false);
    REQUIRE(!projectless.metadata.projectId.has_value());
    AuthorityIssuer issuer;
    const auto resolvedAuthority = issuer.issue(
        projectless,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});
    const auto outcome = router->invoke(
        projectless,
        resolvedAuthority,
        contextFor(projectless));

    REQUIRE(outcome.hasValue());
    REQUIRE(outcome.value().receipt.ok);
    REQUIRE(handler.calls() == 1U);
    REQUIRE(handler.capabilityMatched());
    REQUIRE(handler.lastProjectId().has_value());
    REQUIRE(handler.lastProjectId().value() ==
            resolvedAuthority.projectId());
    REQUIRE(authorizer.calls() == 1U);
    REQUIRE(audit.events().back().status == "ok");
}

void policyAndAuthorityFailuresDoNotDispatch()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);
    AuthorityIssuer issuer;

    const auto authorityRequest = requestFor(tool, 1U);
    const auto readAuthority = issuer.issue(
        authorityRequest,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});
    auto projectMismatch = authorityRequest;
    projectMismatch.metadata.projectId = id<Domain::ProjectId>(
        "20000000-0000-0000-0000-000000000099");
    auto denied = router->invoke(
        projectMismatch, readAuthority, contextFor(projectMismatch));
    requireError(denied, Domain::ErrorCodes::ProjectScopeMismatch);
    REQUIRE(handler.calls() == 0U);
    REQUIRE(authorizer.calls() == 0U);
    REQUIRE(guard.beforeCalls() == 1U);
    REQUIRE(guard.afterCalls() == 1U);
    auto events = audit.events();
    REQUIRE(events.back().status == "denied");
    REQUIRE(events.back().argumentsDigest.has_value());

    const auto request = requestFor(tool, 2U);
    const auto wrongEffect = issuer.issue(
        request,
        Domain::FileAccess::Write,
        {Domain::FileAccess::Write});
    denied = router->invoke(
        request, wrongEffect, contextFor(request, 2U));
    requireError(denied, Domain::ErrorCodes::Unauthorized);
    REQUIRE(handler.calls() == 0U);
    REQUIRE(authorizer.calls() == 0U);

    authorizer.denial = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "The scripted policy denied the call.");
    const auto validAuthority = issuer.issue(
        request,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});
    denied = router->invoke(
        request, validAuthority, contextFor(request, 3U));
    requireError(denied, Domain::ErrorCodes::Unauthorized);
    REQUIRE(authorizer.calls() == 1U);
    REQUIRE(handler.calls() == 0U);
    REQUIRE(guard.afterCalls() == 3U);

    const auto shell = descriptor(
        "shell_exec", Domain::ToolEffect::Write, true, true);
    CatalogFake shellCatalog{{shell}};
    HandlerFake shellHandler{{shell}};
    std::array<Contracts::IToolHandler*, 1U> shellHandlers{&shellHandler};
    AuthorizerFake shellAuthorizer;
    InvocationGuardFake shellGuard;
    AuditRepositoryFake shellAudit;
    FixedHasher shellHasher;
    auto shellRouter = createRouter(
        shellCatalog,
        shellHandlers,
        shellAuthorizer,
        shellGuard,
        shellAudit,
        shellHasher,
        clock);
    const auto shellRequest = requestFor(shell, 4U);
    const auto shellDisabled = issuer.issue(
        shellRequest,
        Domain::FileAccess::Write,
        {Domain::FileAccess::Write, Domain::FileAccess::Execute},
        {},
        false);
    denied = shellRouter->invoke(
        shellRequest, shellDisabled, contextFor(shellRequest, 4U));
    requireError(denied, Domain::ErrorCodes::ShellDisabled);
    REQUIRE(shellAuthorizer.calls() == 0U);
    REQUIRE(shellHandler.calls() == 0U);

    const auto shellEnabled = issuer.issue(
        shellRequest,
        Domain::FileAccess::Write,
        {Domain::FileAccess::Write, Domain::FileAccess::Execute},
        {},
        true);
    auto executed = shellRouter->invoke(
        shellRequest, shellEnabled, contextFor(shellRequest, 5U));
    REQUIRE(executed.hasValue());
    REQUIRE(executed.value().receipt.ok);
    REQUIRE(shellAuthorizer.calls() == 1U);
    REQUIRE(shellHandler.calls() == 1U);
}

void guardOutcomesAndAuditFailureAreBoundarySafe()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);
    const auto request = requestFor(tool);
    AuthorityIssuer issuer;
    const auto authority = issuer.issue(
        request,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});

    guard.immediateErrorCode = "identical_call_loop";
    auto outcome = router->invoke(
        request, authority, contextFor(request));
    REQUIRE(outcome.hasValue());
    REQUIRE(!outcome.value().receipt.ok);
    REQUIRE(outcome.value().receipt.error->code == "identical_call_loop");
    REQUIRE(authorizer.calls() == 0U);
    REQUIRE(handler.calls() == 0U);
    REQUIRE(guard.afterCalls() == 0U);

    guard.immediateErrorCode.reset();
    guard.augmentedPayload =
        R"({"ok":true,"handoff_required":true})";
    audit.fail = true;
    outcome = router->invoke(
        request, authority, contextFor(request, 2U));
    REQUIRE(outcome.hasValue());
    REQUIRE(outcome.value().canonicalPayload ==
            R"({"ok":true,"handoff_required":true})");
    REQUIRE(handler.calls() == 1U);
    REQUIRE(audit.appendCalls() == 2U);

    handler.returnMismatchedReceipt = true;
    guard.augmentedPayload.reset();
    outcome = router->invoke(
        request, authority, contextFor(request, 3U));
    requireError(outcome, Domain::ErrorCodes::IntegrityFailure);
}

void cancellationDuplicateAndShutdownDrain()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    handler.blockUntilCancelled = true;
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);
    const auto request = requestFor(tool);
    AuthorityIssuer issuer;
    const auto authority = issuer.issue(
        request,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});
    const auto context = contextFor(request);

    auto running = std::async(std::launch::async, [&] {
        return router->invoke(request, authority, context);
    });
    handler.waitForCalls(1U);
    REQUIRE(router->activeOperationCount() == 1U);
    auto duplicate = router->invoke(request, authority, context);
    requireError(duplicate, Domain::ErrorCodes::OwnershipConflict);
    router->cancel(context.operationId);
    REQUIRE(running.wait_for(std::chrono::seconds{5}) ==
            std::future_status::ready);
    auto cancelled = running.get();
    requireError(cancelled, Domain::ErrorCodes::Cancelled);
    REQUIRE(router->activeOperationCount() == 0U);
    REQUIRE(guard.cancelCalls() == 1U);
    const auto events = audit.events();
    REQUIRE(events.size() == 1U);
    REQUIRE(events.front().error == Domain::ErrorCodes::Cancelled);

    auto second = std::async(std::launch::async, [&] {
        return router->invoke(
            request, authority, contextFor(request, 2U));
    });
    handler.waitForCalls(2U);
    router->shutdown();
    REQUIRE(second.wait_for(std::chrono::seconds{5}) ==
            std::future_status::ready);
    requireError(second.get(), Domain::ErrorCodes::Cancelled);
    REQUIRE(guard.shutdownCalled());
    auto afterShutdown = router->invoke(
        request, authority, contextFor(request, 3U));
    requireError(afterShutdown, Domain::ErrorCodes::TransportClosed);
}

void activeOperationCapacityIsHardBounded()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    handler.blockUntilCancelled = true;
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);
    AuthorityIssuer issuer;

    std::vector<std::jthread> calls;
    std::vector<std::optional<Domain::Result<Domain::ToolCallOutcome>>> results(
        Mcp::McpToolRouter::MaximumActiveOperations);
    std::vector<Domain::OperationId> operationIds;
    calls.reserve(Mcp::McpToolRouter::MaximumActiveOperations);
    operationIds.reserve(Mcp::McpToolRouter::MaximumActiveOperations);
    for (std::size_t index{};
         index < Mcp::McpToolRouter::MaximumActiveOperations;
         ++index) {
        auto request = requestFor(tool, index + 1U);
        auto authority = issuer.issue(
            request,
            Domain::FileAccess::Read,
            {Domain::FileAccess::Read});
        auto context = contextFor(request, index + 1U);
        operationIds.push_back(context.operationId);
        calls.emplace_back(
            [&, index, request = std::move(request),
             authority = std::move(authority),
             context = std::move(context)] {
                results[index].emplace(
                    router->invoke(request, authority, context));
            });
    }
    handler.waitForCalls(Mcp::McpToolRouter::MaximumActiveOperations);
    REQUIRE(router->activeOperationCount() ==
            Mcp::McpToolRouter::MaximumActiveOperations);

    const auto overflowRequest = requestFor(tool, 65U);
    const auto overflowAuthority = issuer.issue(
        overflowRequest,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});
    auto overflow = router->invoke(
        overflowRequest,
        overflowAuthority,
        contextFor(overflowRequest, 65U));
    requireError(overflow, Domain::ErrorCodes::LimitExceeded);

    for (const auto& operationId : operationIds) {
        router->cancel(operationId);
    }
    for (auto& call : calls) {
        call.join();
    }
    for (auto& result : results) {
        REQUIRE(result.has_value());
        requireError(*result, Domain::ErrorCodes::Cancelled);
    }
    REQUIRE(router->activeOperationCount() == 0U);
    REQUIRE(audit.events().size() ==
            Mcp::McpToolRouter::MaximumActiveOperations);
}

void deadlinesHashFailureAndCapabilityMismatchFailClosed()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);
    const auto request = requestFor(tool);
    AuthorityIssuer issuer;
    const auto authority = issuer.issue(
        request,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});

    auto expired = router->invoke(
        request,
        authority,
        contextFor(
            request,
            1U,
            {},
            Domain::MonotonicTimePoint{} + std::chrono::seconds{100}));
    requireError(expired, Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(authorizer.calls() == 0U);
    REQUIRE(handler.calls() == 0U);

    hasher.fail = true;
    auto hashFailure = router->invoke(
        request, authority, contextFor(request, 2U));
    requireError(hashFailure, Domain::ErrorCodes::InternalFailure);
    REQUIRE(handler.calls() == 0U);
    REQUIRE(audit.events().back().error == Domain::ErrorCodes::InternalFailure);

    hasher.fail = false;
    authorizer.mismatchedCapability = true;
    auto mismatch = router->invoke(
        request, authority, contextFor(request, 3U));
    requireError(mismatch, Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(handler.calls() == 0U);
    REQUIRE(guard.afterCalls() == 1U);
}

void postAdmissionExpiryCancelsGuardState()
{
    const auto tool = descriptor("read_tool");
    CatalogFake catalog{{tool}};
    HandlerFake handler{{tool}};
    std::array<Contracts::IToolHandler*, 1U> handlers{&handler};
    AuthorizerFake authorizer;
    InvocationGuardFake guard;
    AuditRepositoryFake audit;
    FixedHasher hasher;
    FixedClock clock;
    clock.advanceAfterMonotonicCalls(2U, 400);
    auto router = createRouter(
        catalog, handlers, authorizer, guard, audit, hasher, clock);
    const auto request = requestFor(tool);
    AuthorityIssuer issuer;
    const auto authority = issuer.issue(
        request,
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});

    auto expired = router->invoke(
        request, authority, contextFor(request));
    requireError(expired, Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(guard.beforeCalls() == 1U);
    REQUIRE(guard.afterCalls() == 0U);
    REQUIRE(guard.cancelCalls() == 1U);
    REQUIRE(authorizer.calls() == 0U);
    REQUIRE(handler.calls() == 0U);
}

} // namespace

int main()
{
    try {
        registrationIsExactAndComplete();
        routedAuthorityGuardAndAuditAreExact();
        resolvedAuthorityScopesProjectlessToolCall();
        policyAndAuthorityFailuresDoNotDispatch();
        guardOutcomesAndAuditFailureAreBoundarySafe();
        cancellationDuplicateAndShutdownDrain();
        activeOperationCapacityIsHardBounded();
        deadlinesHashFailureAndCapabilityMismatchFailClosed();
        postAdmissionExpiryCancelsGuardState();
        std::cout << "MCP tool router tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP tool router tests failed after " << assertions
                  << " assertions: " << error.what() << '\n';
        return 1;
    }
}
