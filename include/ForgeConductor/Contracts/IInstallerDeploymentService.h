#pragma once

#include "ForgeConductor/Contracts/AuthorizedToolCall.h"
#include "ForgeConductor/Domain/DeploymentModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ToolModels.h"

namespace ForgeConductor::Contracts {

class IInstallerDeploymentService {
public:
    virtual ~IInstallerDeploymentService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::DeploymentStatus> status(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::DeploymentReport> execute(
        const Domain::DeploymentRequest& request,
        const WorkspaceAuthority& mutationAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
