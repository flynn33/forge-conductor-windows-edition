#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <optional>

namespace ForgeConductor::Contracts {

class ILMStudioServeVerifier {
public:
    virtual ~ILMStudioServeVerifier() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioConnectorHealth> verify(
        const Domain::PathText& binaryPath,
        const Domain::PathText& forgeHome,
        Domain::LMStudioConnectorRole role,
        const std::optional<Domain::DeploymentId>& deploymentId,
        const WorkspaceAuthority& executionAuthority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
