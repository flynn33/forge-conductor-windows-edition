#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <chrono>

namespace ForgeConductor::Contracts {

class ILMStudioHostPlatform {
public:
    virtual ~ILMStudioHostPlatform() = default;

    [[nodiscard]] virtual Domain::Result<bool> isRunning(
        const Domain::PathText& applicationExecutable,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> launch(
        const Domain::PathText& applicationExecutable,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> waitForObservation(
        std::chrono::milliseconds maximumWait,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class ILMStudioHostActivator {
public:
    virtual ~ILMStudioHostActivator() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioEnvironmentStatus& environment,
        const Domain::LMStudioHostActivationRequest& request,
        const WorkspaceAuthority& executionAuthority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
