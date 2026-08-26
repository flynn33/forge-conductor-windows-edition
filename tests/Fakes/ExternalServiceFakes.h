#pragma once

#include "DeterministicResult.h"
#include "ForgeConductor/Contracts/IGraphicsServices.h"
#include "ForgeConductor/Contracts/IInstallerDeploymentService.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Contracts/ILMStudioEnvironment.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

class BoundedOperationGate final {
public:
    [[nodiscard]] Domain::Result<void> enter(
        const Domain::OperationContext& context) noexcept
    {
        try {
            ++calls_;
            lastOperationId_ = context.operationId;
            lastCorrelationId_ = context.correlationId;
            lastDeadline_ = context.deadline;
            if (shutdown_ || context.isCancellationRequested() ||
                (cancelledOperation_ &&
                 cancelledOperation_.value() == context.operationId)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic external operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic external operation deadline expired."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic external operation could not be recorded."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            cancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void shutdown() noexcept { shutdown_ = true; }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastOperationId() const noexcept
    {
        return lastOperationId_;
    }

    [[nodiscard]] const std::optional<Domain::CorrelationId>&
    lastCorrelationId() const noexcept
    {
        return lastCorrelationId_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint lastDeadline() const noexcept
    {
        return lastDeadline_;
    }

private:
    std::optional<Domain::OperationId> lastOperationId_;
    std::optional<Domain::CorrelationId> lastCorrelationId_;
    std::optional<Domain::OperationId> cancelledOperation_;
    Domain::MonotonicTimePoint lastDeadline_{};
    Domain::MonotonicTimePoint now_{};
    std::size_t calls_{};
    bool shutdown_{};
};

template <typename T>
[[nodiscard]] Domain::Result<T> finishExternalCall(
    BoundedOperationGate& gate,
    const Domain::OperationContext& context,
    const DeterministicResult<T>& scriptedResult) noexcept
{
    auto accepted = gate.enter(context);
    if (!accepted) {
        return Domain::Result<T>::failure(std::move(accepted).error());
    }
    try {
        return scriptedResult.get();
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic external result could not be copied."));
    }
}

template <typename T>
[[nodiscard]] Domain::Result<T> externalRecordingFailure() noexcept
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The deterministic external request could not be recorded."));
}
template <typename T>
[[nodiscard]] Domain::Result<T> externalAuthorizationFailure(
    const char* const message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::Unauthorized, message));
}


class RecordingLMStudioEnvironmentFake final
    : public Contracts::ILMStudioEnvironment {
public:
    DeterministicResult<Domain::LMStudioEnvironmentStatus> inspectResult;
    DeterministicResult<Domain::LMStudioConnectionHealth> connectionHealthResult;

    [[nodiscard]] Domain::Result<Domain::LMStudioEnvironmentStatus> inspect(
        const std::optional<Domain::PathText>& explicitConfigurationPath,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++inspectCalls_;
            lastExplicitConfigurationPath_ = explicitConfigurationPath;
            lastReadAuthority_.emplace(readAuthority);
            return finishExternalCall(gate_, context, inspectResult);
        } catch (...) {
            return externalRecordingFailure<Domain::LMStudioEnvironmentStatus>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioConnectionHealth>
    connectionHealth(
        const Domain::OperationContext& context) noexcept override
    {
        ++connectionHealthCalls_;
        return finishExternalCall(gate_, context, connectionHealthResult);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] std::size_t inspectCalls() const noexcept { return inspectCalls_; }
    [[nodiscard]] std::size_t connectionHealthCalls() const noexcept
    {
        return connectionHealthCalls_;
    }

    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastReadAuthority() const noexcept
    {
        return lastReadAuthority_;
    }

private:
    BoundedOperationGate gate_;
    std::optional<Domain::PathText> lastExplicitConfigurationPath_;
    std::optional<Contracts::WorkspaceAuthority> lastReadAuthority_;
    std::size_t inspectCalls_{};
    std::size_t connectionHealthCalls_{};
};

class RecordingLMStudioDeploymentServiceFake final
    : public Contracts::ILMStudioDeploymentService {
public:
    DeterministicResult<Domain::LMStudioPluginStatus> statusResult;
    DeterministicResult<Domain::LMStudioInstallResult> deployResult;
    DeterministicResult<Domain::LMStudioHostActivationResult> activateResult;

    [[nodiscard]] Domain::Result<Domain::LMStudioPluginStatus> status(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++statusCalls_;
            lastDeploymentRequest_ = request;
            lastAuthority_.emplace(readAuthority);
            lastAuthorization_.reset();
            return finishExternalCall(gate_, context, statusResult);
        } catch (...) {
            return externalRecordingFailure<Domain::LMStudioPluginStatus>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++deployCalls_;
            lastDeploymentRequest_ = request;
            lastAuthority_.emplace(writeAuthority);
            lastAuthorization_.emplace(authorization);
            if (writeAuthority.intent() != Domain::FileAccess::Write ||
                !authorization.matches(writeAuthority, context) ||
                !authorization.matchesProject(writeAuthority.projectId()) ||
                authorization.effect() != Domain::ToolEffect::Write) {
                return externalAuthorizationFailure<Domain::LMStudioInstallResult>(
                    "The LM Studio deployment capability is mismatched.");
            }
            return finishExternalCall(gate_, context, deployResult);
        } catch (...) {
            return externalRecordingFailure<Domain::LMStudioInstallResult>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioHostActivationRequest& request,
        const Contracts::WorkspaceAuthority& executionAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++activateCalls_;
            lastActivationRequest_ = request;
            lastAuthority_.emplace(executionAuthority);
            lastAuthorization_.emplace(authorization);
            if (executionAuthority.intent() != Domain::FileAccess::Execute ||
                !authorization.matches(executionAuthority, context) ||
                !authorization.matchesProject(executionAuthority.projectId()) ||
                authorization.effect() != Domain::ToolEffect::Execute) {
                return externalAuthorizationFailure<
                    Domain::LMStudioHostActivationResult>(
                        "The LM Studio activation capability is mismatched.");
            }
            return finishExternalCall(gate_, context, activateResult);
        } catch (...) {
            return externalRecordingFailure<Domain::LMStudioHostActivationResult>();
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        gate_.cancel(operationId);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] std::size_t statusCalls() const noexcept { return statusCalls_; }
    [[nodiscard]] std::size_t deployCalls() const noexcept { return deployCalls_; }
    [[nodiscard]] std::size_t activateCalls() const noexcept { return activateCalls_; }

    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastAuthority() const noexcept
    {
        return lastAuthority_;
    }

    [[nodiscard]] const std::optional<Contracts::AuthorizedToolCall>&
    lastAuthorization() const noexcept
    {
        return lastAuthorization_;
    }

private:
    BoundedOperationGate gate_;
    std::optional<Domain::LMStudioDeploymentRequest> lastDeploymentRequest_;
    std::optional<Domain::LMStudioHostActivationRequest> lastActivationRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastAuthority_;
    std::optional<Contracts::AuthorizedToolCall> lastAuthorization_;
    std::size_t statusCalls_{};
    std::size_t deployCalls_{};
    std::size_t activateCalls_{};
};

class RecordingGraphicsDeviceServiceFake final
    : public Contracts::IGraphicsDeviceService {
public:
    DeterministicResult<Domain::GraphicsDeviceStatus> initializeResult;
    DeterministicResult<Domain::GraphicsDeviceStatus> statusResult;
    DeterministicResult<Domain::GraphicsDeviceStatus> recoverResult;

    [[nodiscard]] Domain::Result<Domain::GraphicsDeviceStatus> initialize(
        const Domain::OperationContext& context) noexcept override
    {
        ++initializeCalls_;
        return finishExternalCall(gate_, context, initializeResult);
    }

    [[nodiscard]] Domain::Result<Domain::GraphicsDeviceStatus> status(
        const Domain::OperationContext& context) noexcept override
    {
        ++statusCalls_;
        return finishExternalCall(gate_, context, statusResult);
    }

    [[nodiscard]] Domain::Result<Domain::GraphicsDeviceStatus> recover(
        const Domain::OperationContext& context) noexcept override
    {
        ++recoverCalls_;
        return finishExternalCall(gate_, context, recoverResult);
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        gate_.cancel(operationId);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] std::size_t initializeCalls() const noexcept
    {
        return initializeCalls_;
    }
    [[nodiscard]] std::size_t statusCalls() const noexcept { return statusCalls_; }
    [[nodiscard]] std::size_t recoverCalls() const noexcept { return recoverCalls_; }

private:
    BoundedOperationGate gate_;
    std::size_t initializeCalls_{};
    std::size_t statusCalls_{};
    std::size_t recoverCalls_{};
};

class RecordingRenderServiceFake final
    : public Contracts::IRenderService {
public:
    DeterministicResult<Domain::RenderOutcome> renderResult;

    [[nodiscard]] Domain::Result<Domain::RenderOutcome> render(
        const Domain::RenderRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++renderCalls_;
            lastRequest_ = request;
            return finishExternalCall(gate_, context, renderResult);
        } catch (...) {
            return externalRecordingFailure<Domain::RenderOutcome>();
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        gate_.cancel(operationId);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] std::size_t renderCalls() const noexcept { return renderCalls_; }

    [[nodiscard]] const std::optional<Domain::RenderRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

private:
    BoundedOperationGate gate_;
    std::optional<Domain::RenderRequest> lastRequest_;
    std::size_t renderCalls_{};
};

class RecordingInstallerDeploymentServiceFake final
    : public Contracts::IInstallerDeploymentService {
public:
    DeterministicResult<Domain::DeploymentStatus> statusResult;
    DeterministicResult<Domain::DeploymentReport> executeResult;

    [[nodiscard]] Domain::Result<Domain::DeploymentStatus> status(
        const Domain::OperationContext& context) noexcept override
    {
        ++statusCalls_;
        return finishExternalCall(gate_, context, statusResult);
    }

    [[nodiscard]] Domain::Result<Domain::DeploymentReport> execute(
        const Domain::DeploymentRequest& request,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++executeCalls_;
            lastRequest_ = request;
            lastAuthority_.emplace(mutationAuthority);
            lastAuthorization_.emplace(authorization);
            const auto expectedEffect =
                request.action == Domain::DeploymentAction::Purge
                    ? Domain::ToolEffect::Destructive
                    : Domain::ToolEffect::Write;
            if (mutationAuthority.intent() != Domain::FileAccess::Write ||
                !authorization.matches(mutationAuthority, context) ||
                !authorization.matchesProject(mutationAuthority.projectId()) ||
                authorization.effect() != expectedEffect) {
                return externalAuthorizationFailure<Domain::DeploymentReport>(
                    "The installer deployment capability is mismatched.");
            }
            return finishExternalCall(gate_, context, executeResult);
        } catch (...) {
            return externalRecordingFailure<Domain::DeploymentReport>();
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        gate_.cancel(operationId);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] std::size_t statusCalls() const noexcept { return statusCalls_; }
    [[nodiscard]] std::size_t executeCalls() const noexcept { return executeCalls_; }

    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastAuthority() const noexcept
    {
        return lastAuthority_;
    }

    [[nodiscard]] const std::optional<Contracts::AuthorizedToolCall>&
    lastAuthorization() const noexcept
    {
        return lastAuthorization_;
    }

private:
    BoundedOperationGate gate_;
    std::optional<Domain::DeploymentRequest> lastRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastAuthority_;
    std::optional<Contracts::AuthorizedToolCall> lastAuthorization_;
    std::size_t statusCalls_{};
    std::size_t executeCalls_{};
};

} // namespace ForgeConductor::Tests::Fakes
