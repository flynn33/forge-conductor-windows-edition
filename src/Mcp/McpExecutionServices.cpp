#include "ForgeConductor/Mcp/McpExecutionServices.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    const char* const message)
{
    return Domain::Result<T>::failure(Domain::makeError(code, message));
}

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess expected) noexcept
{
    return std::find(values.begin(), values.end(), expected) != values.end();
}

[[nodiscard]] Domain::FileAccess requiredAccess(
    const Domain::ToolEffect effect) noexcept
{
    switch (effect) {
    case Domain::ToolEffect::Read:
        return Domain::FileAccess::Read;
    case Domain::ToolEffect::Write:
        return Domain::FileAccess::Write;
    case Domain::ToolEffect::Execute:
        return Domain::FileAccess::Execute;
    case Domain::ToolEffect::Destructive:
        return Domain::FileAccess::Delete;
    }
    return Domain::FileAccess::Read;
}

[[nodiscard]] bool validIntent(
    const Domain::ToolEffect effect,
    const Domain::FileAccess intent) noexcept
{
    switch (effect) {
    case Domain::ToolEffect::Read:
        return intent == Domain::FileAccess::Read ||
            intent == Domain::FileAccess::Write;
    case Domain::ToolEffect::Write:
        return intent == Domain::FileAccess::Write;
    case Domain::ToolEffect::Execute:
        return intent == Domain::FileAccess::Execute;
    case Domain::ToolEffect::Destructive:
        return intent == Domain::FileAccess::Write ||
            intent == Domain::FileAccess::Delete;
    }
    return false;
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock) noexcept
{
    if (context.isCancellationRequested()) {
        return failure<void>(
            Domain::ErrorCodes::Cancelled,
            "The MCP execution operation was cancelled.");
    }
    if (context.isExpired(clock.monotonicNow())) {
        return failure<void>(
            Domain::ErrorCodes::DeadlineExceeded,
            "The MCP execution operation exceeded its deadline.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateAuthority(
    const Domain::ToolCallRequest& request,
    const Domain::ToolEffect effect,
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    if (authority.callerId() != request.metadata.clientId) {
        return failure<void>(
            Domain::ErrorCodes::Unauthorized,
            "The MCP caller does not match workspace authority.");
    }
    if (request.metadata.projectId &&
        request.metadata.projectId.value() != authority.projectId()) {
        return failure<void>(
            Domain::ErrorCodes::ProjectScopeMismatch,
            "The MCP project does not match workspace authority.");
    }

    const auto access = requiredAccess(effect);
    if (!validIntent(effect, authority.intent()) ||
        !containsAccess(authority.grants(), access) ||
        containsAccess(authority.denials(), access)) {
        return failure<void>(
            Domain::ErrorCodes::Unauthorized,
            "Workspace authority does not grant the selected MCP tool effect.");
    }
    return Domain::Result<void>::success();
}

} // namespace

McpExecutionContextResolver::McpExecutionContextResolver(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Domain::ProjectId defaultProjectId,
    const Contracts::IClock& clock,
    Contracts::IMcpClientWorkspaceContext* const clientWorkspaceContext)
    : workspaceAuthority_{workspaceAuthority},
      defaultProjectId_{std::move(defaultProjectId)},
      clock_{clock},
      clientWorkspaceContext_{clientWorkspaceContext}
{
}

Domain::Result<Contracts::WorkspaceAuthority>
McpExecutionContextResolver::resolve(
    const Domain::ToolCallRequest& request,
    const Domain::ToolEffect effect,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto current = validateContext(context, clock_);
        if (!current) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(current).error());
        }
        if (request.metadata.correlationId != context.correlationId) {
            return failure<Contracts::WorkspaceAuthority>(
                Domain::ErrorCodes::Unauthorized,
                "The MCP request correlation does not match its operation.");
        }

        std::optional<Domain::ClientWorkspaceSnapshot> adoptedWorkspace;
        if (!request.metadata.projectId && clientWorkspaceContext_ != nullptr) {
            auto snapshot = clientWorkspaceContext_->snapshot(
                request.metadata.clientId, context);
            if (!snapshot) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    std::move(snapshot).error());
            }
            adoptedWorkspace = std::move(snapshot).value();
            if (adoptedWorkspace &&
                adoptedWorkspace->clientId != request.metadata.clientId) {
                return failure<Contracts::WorkspaceAuthority>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The recovered MCP workspace belongs to another client.");
            }
        }

        const auto& projectId = request.metadata.projectId
            ? request.metadata.projectId.value()
            : adoptedWorkspace
                ? adoptedWorkspace->projectId
                : defaultProjectId_;
        auto issued = workspaceAuthority_.authorityFor(projectId, context);
        if (!issued) {
            return issued;
        }

        current = validateContext(context, clock_);
        if (!current) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(current).error());
        }
        auto authority = std::move(issued).value();
        if (adoptedWorkspace) {
            if (adoptedWorkspace->projectId != projectId) {
                return failure<Contracts::WorkspaceAuthority>(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The recovered MCP workspace belongs to another project.");
            }
            if (std::find(
                    authority.trustedRoots().begin(),
                    authority.trustedRoots().end(),
                    adoptedWorkspace->authorityRoot) ==
                authority.trustedRoots().end()) {
                return failure<Contracts::WorkspaceAuthority>(
                    Domain::ErrorCodes::Unauthorized,
                    "The recovered MCP workspace is no longer a trusted project root.");
            }
            const auto generation = (std::max)(
                authority.generation(), adoptedWorkspace->generation);
            if (generation ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return failure<Contracts::WorkspaceAuthority>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The recovered MCP workspace generation is exhausted.");
            }
            auto narrowed = workspaceAuthority_.narrow(
                authority,
                std::vector<Domain::PathText>{
                    adoptedWorkspace->authorityRoot},
                authority.grants(),
                authority.shellEnabled(),
                generation + 1U,
                context);
            if (!narrowed) {
                return narrowed;
            }
            auto recoveredAuthority = std::move(narrowed).value();
            auto binding = validateAuthority(
                request, effect, recoveredAuthority);
            if (!binding) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    std::move(binding).error());
            }
            return Domain::Result<Contracts::WorkspaceAuthority>::success(
                std::move(recoveredAuthority));
        }
        auto binding = validateAuthority(request, effect, authority);
        if (!binding) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(binding).error());
        }
        return Domain::Result<Contracts::WorkspaceAuthority>::success(
            std::move(authority));
    } catch (...) {
        return failure<Contracts::WorkspaceAuthority>(
            Domain::ErrorCodes::InternalFailure,
            "The MCP execution context could not be resolved.");
    }
}

McpToolAuthorizer::McpToolAuthorizer(
    const Contracts::IClock& clock) noexcept
    : clock_{clock}
{
}

Domain::Result<Contracts::AuthorizedToolCall> McpToolAuthorizer::authorize(
    const Domain::ToolAuthorizationRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto current = validateContext(context, clock_);
        if (!current) {
            return Domain::Result<Contracts::AuthorizedToolCall>::failure(
                std::move(current).error());
        }
        auto binding = validateAuthority(request.call, request.effect, authority);
        if (!binding) {
            return Domain::Result<Contracts::AuthorizedToolCall>::failure(
                std::move(binding).error());
        }

        auto authorized = issueAuthorizedToolCall(request, authority, context);
        if (!authorized) {
            return authorized;
        }
        current = validateContext(context, clock_);
        if (!current) {
            return Domain::Result<Contracts::AuthorizedToolCall>::failure(
                std::move(current).error());
        }
        return authorized;
    } catch (...) {
        return failure<Contracts::AuthorizedToolCall>(
            Domain::ErrorCodes::InternalFailure,
            "The MCP tool capability could not be authorized.");
    }
}

} // namespace ForgeConductor::Mcp
