#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IToolServices.h"

namespace ForgeConductor::Mcp {

// Resolves the project-scoped authority used by one MCP invocation. The
// composition root owns every referenced service for longer than this object.
class McpExecutionContextResolver final
    : public Contracts::IMcpExecutionContextResolver {
public:
    McpExecutionContextResolver(
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Domain::ProjectId defaultProjectId,
        const Contracts::IClock& clock);

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> resolve(
        const Domain::ToolCallRequest& request,
        Domain::ToolEffect effect,
        const Domain::OperationContext& context) noexcept override;

private:
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    const Domain::ProjectId defaultProjectId_;
    const Contracts::IClock& clock_;
};

// Issues an immutable capability only after the router-selected tool effect,
// workspace authority, caller, project, and operation context agree.
class McpToolAuthorizer final : public Contracts::IToolAuthorizer {
public:
    explicit McpToolAuthorizer(const Contracts::IClock& clock) noexcept;

    [[nodiscard]] Domain::Result<Contracts::AuthorizedToolCall> authorize(
        const Domain::ToolAuthorizationRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

private:
    const Contracts::IClock& clock_;
};

} // namespace ForgeConductor::Mcp
