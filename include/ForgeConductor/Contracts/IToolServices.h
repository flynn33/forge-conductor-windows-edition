#pragma once

#include "ForgeConductor/Contracts/AuthorizedToolCall.h"
#include "ForgeConductor/Domain/Domain.h"

#include <span>

namespace ForgeConductor::Contracts {

class IToolAuthorizer {
public:
    virtual ~IToolAuthorizer() = default;

    [[nodiscard]] virtual Domain::Result<AuthorizedToolCall> authorize(
        const Domain::ToolAuthorizationRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;

protected:
    [[nodiscard]] static Domain::Result<AuthorizedToolCall>
    issueAuthorizedToolCall(
        const Domain::ToolAuthorizationRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const auto& call = request.call;
            if (call.metadata.clientId != authority.callerId()) {
                return Domain::Result<AuthorizedToolCall>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The tool caller does not match workspace authority."));
            }
            if (call.metadata.correlationId != context.correlationId) {
                return Domain::Result<AuthorizedToolCall>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The tool correlation does not match the operation."));
            }
            if (call.metadata.projectId &&
                call.metadata.projectId.value() != authority.projectId()) {
                return Domain::Result<AuthorizedToolCall>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectScopeMismatch,
                        "The tool project does not match workspace authority."));
            }
            if (request.authority.authorityId != authority.authorityId() ||
                request.authority.generation != authority.generation()) {
                return Domain::Result<AuthorizedToolCall>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The tool authority reference is stale or mismatched."));
            }
            if (call.toolName.empty() || call.canonicalArguments.empty()) {
                return Domain::Result<AuthorizedToolCall>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The canonical tool request is incomplete."));
            }
            return Domain::Result<AuthorizedToolCall>::success(
                AuthorizedToolCall{
                    call,
                    request.effect,
                    authority.authorityId(),
                    authority.generation()});
        } catch (...) {
            return Domain::Result<AuthorizedToolCall>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The authorized tool capability could not be issued."));
        }
    }
};

class IToolHandler {
public:
    virtual ~IToolHandler() = default;

    [[nodiscard]] virtual std::span<const Domain::McpToolDescriptor> tools() const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ToolCallOutcome> handle(
        const AuthorizedToolCall& authorizedCall,
        const Domain::OperationContext& context) noexcept = 0;
};

class IToolCatalog {
public:
    virtual ~IToolCatalog() = default;

    [[nodiscard]] virtual std::span<const Domain::McpToolDescriptor> tools() const noexcept = 0;
};

class IToolRouter {
public:
    virtual ~IToolRouter() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
