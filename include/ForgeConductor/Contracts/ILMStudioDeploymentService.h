#pragma once

#include "ForgeConductor/Contracts/AuthorizedToolCall.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ToolModels.h"

namespace ForgeConductor::Contracts {

class ILMStudioDeploymentService {
public:
    virtual ~ILMStudioDeploymentService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioPluginStatus> status(
        const Domain::LMStudioDeploymentRequest& request,
        const WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::LMStudioDeploymentRequest& request,
        const WorkspaceAuthority& writeAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioHostActivationRequest& request,
        const WorkspaceAuthority& executionAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
