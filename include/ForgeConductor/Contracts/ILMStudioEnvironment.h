#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <optional>

namespace ForgeConductor::Contracts {

class ILMStudioEnvironment {
public:
    virtual ~ILMStudioEnvironment() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioEnvironmentStatus> inspect(
        const std::optional<Domain::PathText>& explicitConfigurationPath,
        const WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioConnectionHealth>
    connectionHealth(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
