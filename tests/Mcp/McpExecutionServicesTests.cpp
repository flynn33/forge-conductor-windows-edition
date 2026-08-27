#include "ForgeConductor/Mcp/McpExecutionServices.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
namespace Fakes = ForgeConductor::Tests::Fakes;

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error{                                             \
                std::string{"Requirement failed: "} + #condition};              \
        }                                                                         \
    } while (false)

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
[[nodiscard]] T id(const std::string_view value)
{
    return take(T::parse(value));
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{1'735'789'855s};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return now_;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

private:
    Domain::MonotonicTimePoint now_{1s};
};

class ClientWorkspaceContextFake final
    : public Contracts::IMcpClientWorkspaceContext {
public:
    void setSnapshot(Domain::ClientWorkspaceSnapshot snapshot)
    {
        snapshot_ = std::move(snapshot);
    }

    [[nodiscard]] Domain::Result<Domain::ClientWorkspaceAdoption> adopt(
        const Domain::ClientId&,
        const Domain::LegacyContinuityRecord&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ClientWorkspaceAdoption>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The execution resolver test does not adopt contexts."));
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ClientWorkspaceSnapshot>> snapshot(
        const Domain::ClientId& clientId,
        const Domain::OperationContext&) noexcept override
    {
        ++snapshotCalls_;
        if (snapshot_ && snapshot_->clientId == clientId) {
            return Domain::Result<
                std::optional<Domain::ClientWorkspaceSnapshot>>::success(
                    snapshot_);
        }
        return Domain::Result<
            std::optional<Domain::ClientWorkspaceSnapshot>>::success(
                std::nullopt);
    }

    void clear(const Domain::ClientId&) noexcept override
    {
        snapshot_.reset();
    }

    void shutdown() noexcept override { snapshot_.reset(); }

    [[nodiscard]] std::size_t snapshotCalls() const noexcept
    {
        return snapshotCalls_;
    }

private:
    std::optional<Domain::ClientWorkspaceSnapshot> snapshot_;
    std::size_t snapshotCalls_{};
};

[[nodiscard]] Domain::ProjectId defaultProject()
{
    return id<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
}

[[nodiscard]] Domain::ClientId client(
    const std::string_view value = "lm-studio")
{
    return id<Domain::ClientId>(value);
}

[[nodiscard]] Domain::OperationContext context(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        id<Domain::OperationId>(
            "22222222-2222-4222-8222-222222222222"),
        Domain::MonotonicTimePoint{10s},
        cancellation,
        id<Domain::CorrelationId>("mcp-execution-test")};
}

[[nodiscard]] Domain::ToolCallRequest request(
    std::optional<Domain::ProjectId> project = std::nullopt,
    const Domain::ClientId& caller = client())
{
    return Domain::ToolCallRequest{
        Domain::McpRequestMetadata{
            id<Domain::RequestId>("request-1"),
            id<Domain::CorrelationId>("mcp-execution-test"),
            caller,
            std::move(project),
            "2025-03-26"},
        "fs_write",
        R"json({"content":"value","path":"file.txt"})json"};
}

[[nodiscard]] Fakes::DeterministicWorkspaceAuthority authority(
    const Domain::FileAccess intent,
    std::vector<Domain::FileAccess> grants,
    std::vector<Domain::FileAccess> denials = {})
{
    return Fakes::DeterministicWorkspaceAuthority{
        id<Domain::AuthorityId>(
            "33333333-3333-4333-8333-333333333333"),
        client(),
        {take(Domain::PathText::create("C:\\workspace"))},
        intent,
        std::move(grants),
        std::move(denials),
        true,
        7U};
}

void resolvesDefaultAndExplicitProjectScopes()
{
    FixedClock clock;
    auto issuer = authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute});
    Mcp::McpExecutionContextResolver resolver{
        issuer, defaultProject(), clock};

    const auto active = context();
    const auto read = take(resolver.resolve(
        request(), Domain::ToolEffect::Read, active));
    REQUIRE(read.projectId() == defaultProject());
    REQUIRE(read.intent() == Domain::FileAccess::Write);

    const auto explicitProject = id<Domain::ProjectId>(
        "44444444-4444-4444-8444-444444444444");
    const auto writeRequest = request(explicitProject);
    const auto write = take(resolver.resolve(
        writeRequest, Domain::ToolEffect::Write, active));
    REQUIRE(write.projectId() == explicitProject);
    REQUIRE(write.callerId() == writeRequest.metadata.clientId);
}

void resolverRejectsMismatchedOrInsufficientAuthority()
{
    FixedClock clock;
    auto issuer = authority(
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read, Domain::FileAccess::Write});
    Mcp::McpExecutionContextResolver resolver{
        issuer, defaultProject(), clock};

    const auto write = resolver.resolve(
        request(), Domain::ToolEffect::Write, context());
    REQUIRE(!write);
    REQUIRE(write.error().code == Domain::ErrorCodes::Unauthorized);

    const auto wrongCaller = resolver.resolve(
        request(std::nullopt, client("foreign-client")),
        Domain::ToolEffect::Read,
        context());
    REQUIRE(!wrongCaller);
    REQUIRE(wrongCaller.error().code == Domain::ErrorCodes::Unauthorized);
}

void recoveredWorkspaceSelectsImplicitProjectAndNarrowsAuthority()
{
    FixedClock clock;
    auto issuer = authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute});
    ClientWorkspaceContextFake recovered;
    const auto adoptedProject = id<Domain::ProjectId>(
        "55555555-5555-4555-8555-555555555555");
    const auto adoptedRoot = take(Domain::PathText::create("C:\\workspace"));
    recovered.setSnapshot(Domain::ClientWorkspaceSnapshot{
        client(),
        adoptedProject,
        adoptedRoot,
        id<Domain::LegacyHandoffId>("execution-recovered-handoff"),
        17U,
        9U});
    Mcp::McpExecutionContextResolver resolver{
        issuer, defaultProject(), clock, &recovered};

    const auto implicit = take(resolver.resolve(
        request(), Domain::ToolEffect::Read, context()));
    REQUIRE(implicit.projectId() == adoptedProject);
    REQUIRE(implicit.trustedRoots().size() == 1U);
    REQUIRE(implicit.trustedRoots().front() == adoptedRoot);
    REQUIRE(implicit.generation() == 10U);
    REQUIRE(recovered.snapshotCalls() == 1U);

    const auto explicitScope = take(resolver.resolve(
        request(defaultProject()), Domain::ToolEffect::Read, context()));
    REQUIRE(explicitScope.projectId() == defaultProject());
    REQUIRE(explicitScope.generation() == 7U);
    REQUIRE(recovered.snapshotCalls() == 1U);
}

void cancellationDeadlineAndCorrelationFailClosed()
{
    FixedClock clock;
    auto issuer = authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write});
    Mcp::McpExecutionContextResolver resolver{
        issuer, defaultProject(), clock};

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledResult = resolver.resolve(
        request(), Domain::ToolEffect::Read, context(cancelled.get_token()));
    REQUIRE(!cancelledResult);
    REQUIRE(cancelledResult.error().code == Domain::ErrorCodes::Cancelled);

    clock.setNow(Domain::MonotonicTimePoint{10s});
    const auto expired = resolver.resolve(
        request(), Domain::ToolEffect::Read, context());
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);

    clock.setNow(Domain::MonotonicTimePoint{1s});
    auto mismatchedContext = context();
    mismatchedContext.correlationId =
        id<Domain::CorrelationId>("different-correlation");
    const auto mismatch = resolver.resolve(
        request(), Domain::ToolEffect::Read, mismatchedContext);
    REQUIRE(!mismatch);
    REQUIRE(mismatch.error().code == Domain::ErrorCodes::Unauthorized);
}

void authorizerIssuesOnlyBoundCapabilities()
{
    static_assert(std::is_final_v<Mcp::McpExecutionContextResolver>);
    static_assert(std::is_final_v<Mcp::McpToolAuthorizer>);

    FixedClock clock;
    auto issuer = authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write});
    Mcp::McpExecutionContextResolver resolver{
        issuer, defaultProject(), clock};
    Mcp::McpToolAuthorizer authorizer{clock};
    const auto active = context();
    const auto call = request(defaultProject());
    const auto resolved = take(resolver.resolve(
        call, Domain::ToolEffect::Write, active));

    const Domain::ToolAuthorizationRequest authorization{
        call,
        Domain::ToolEffect::Write,
        Domain::AuthorityReference{
            resolved.authorityId(), resolved.generation()}};
    const auto authorized = take(authorizer.authorize(
        authorization, resolved, active));
    REQUIRE(authorized.matches(call));
    REQUIRE(authorized.matches(resolved, active));
    REQUIRE(authorized.effect() == Domain::ToolEffect::Write);

    auto stale = authorization;
    ++stale.authority.generation;
    const auto staleResult = authorizer.authorize(stale, resolved, active);
    REQUIRE(!staleResult);
    REQUIRE(staleResult.error().code == Domain::ErrorCodes::Unauthorized);

    clock.setNow(Domain::MonotonicTimePoint{10s});
    const auto expired = authorizer.authorize(
        authorization, resolved, active);
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);
}

} // namespace

int main()
{
    try {
        resolvesDefaultAndExplicitProjectScopes();
        resolverRejectsMismatchedOrInsufficientAuthority();
        recoveredWorkspaceSelectsImplicitProjectAndNarrowsAuthority();
        cancellationDeadlineAndCorrelationFailClosed();
        authorizerIssuesOnlyBoundCapabilities();
        std::cout << "MCP execution service tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
