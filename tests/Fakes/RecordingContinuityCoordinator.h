#pragma once

#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "DeterministicResult.h"

#include <array>
#include <cstddef>
#include <optional>

namespace ForgeConductor::Tests::Fakes {

enum class ContinuityCall : std::size_t {
    Checkpoint,
    PrepareHandoff,
    GetPendingHandoff,
    AcknowledgeHandoff,
    Resume,
    Status,
    RequestRollover,
    RecoverIncompleteOperations,
    ResetProjectContinuity,
    Count
};

class RecordingContinuityCoordinator final
    : public Contracts::IContinuityCoordinator {
public:
    DeterministicResult<Domain::CheckpointOutcome> checkpointResult;
    DeterministicResult<Domain::CheckpointOutcome> prepareHandoffResult;
    DeterministicResult<std::optional<Domain::ContinuityHandoff>>
        pendingHandoffResult;
    DeterministicResult<Domain::ContinuityOperation>
        acknowledgeHandoffResult;
    DeterministicResult<Domain::HandoffResumeOutcome> resumeResult;
    DeterministicResult<Domain::ContinuityStatus> statusResult;
    DeterministicResult<Domain::RolloverOutcome> rolloverResult;
    DeterministicResult<Domain::ContinuityRecoveryReport> recoveryResult;
    DeterministicResult<Domain::ContinuityResetReport> resetResult;

    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> checkpoint(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ContinuityCall::Checkpoint,
            &request.handoff.project.projectId,
            context,
            checkpointResult);
    }

    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> prepareHandoff(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ContinuityCall::PrepareHandoff,
            &request.handoff.project.projectId,
            context,
            prepareHandoffResult);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>>
    getPendingHandoff(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ContinuityCall::GetPendingHandoff,
            &projectId,
            context,
            pendingHandoffResult);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation>
    acknowledgeHandoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastContinuityOperationId_ = operationId;
            lastAcknowledgement_ = acknowledgement;
            return complete(
                ContinuityCall::AcknowledgeHandoff,
                &projectId,
                context,
                acknowledgeHandoffResult);
        } catch (...) {
            return recordingFailure<Domain::ContinuityOperation>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HandoffResumeOutcome> resume(
        const Domain::HandoffResumeRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ContinuityCall::Resume,
            &request.projectId,
            context,
            resumeResult);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ContinuityCall::Status,
            &projectId,
            context,
            statusResult);
    }

    [[nodiscard]] Domain::Result<Domain::RolloverOutcome> requestRollover(
        const Domain::RolloverRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastContinuityOperationId_ = request.operationId;
            return complete(
                ContinuityCall::RequestRollover,
                &request.projectId,
                context,
                rolloverResult);
        } catch (...) {
            return recordingFailure<Domain::RolloverOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityRecoveryReport>
    recoverIncompleteOperations(
        const Domain::ContinuityRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        const auto* projectId = request.projectId
            ? &request.projectId.value()
            : nullptr;
        return complete(
            ContinuityCall::RecoverIncompleteOperations,
            projectId,
            context,
            recoveryResult);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport>
    resetProjectContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ContinuityCall::ResetProjectContinuity,
            &request.projectId,
            context,
            resetResult);
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            lastCancelledOperationId_ = operationId;
            ++cancelCalls_;
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        shutdown_ = true;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        now_ = now;
    }

    [[nodiscard]] std::size_t callCount(
        const ContinuityCall call) const noexcept
    {
        return calls_[static_cast<std::size_t>(call)];
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        return cancelCalls_;
    }

    [[nodiscard]] const std::optional<Domain::ProjectId>&
    lastProjectId() const noexcept
    {
        return lastProjectId_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastOperationId() const noexcept
    {
        return lastOperationId_;
    }

    [[nodiscard]] const std::optional<Domain::ContinuityOperationId>&
    lastContinuityOperationId() const noexcept
    {
        return lastContinuityOperationId_;
    }

    [[nodiscard]] const std::optional<Domain::HandoffAcknowledgement>&
    lastAcknowledgement() const noexcept
    {
        return lastAcknowledgement_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastCancelledOperationId() const noexcept
    {
        return lastCancelledOperationId_;
    }

private:
    template <typename T>
    [[nodiscard]] Domain::Result<T> complete(
        const ContinuityCall call,
        const Domain::ProjectId* const projectId,
        const Domain::OperationContext& context,
        const DeterministicResult<T>& result) noexcept
    {
        try {
            ++calls_[static_cast<std::size_t>(call)];
            lastOperationId_ = context.operationId;
            if (projectId != nullptr) {
                lastProjectId_ = *projectId;
            } else {
                lastProjectId_.reset();
            }
            if (shutdown_ || context.isCancellationRequested() ||
                (lastCancelledOperationId_ &&
                 lastCancelledOperationId_.value() == context.operationId)) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic continuity operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic continuity deadline expired."));
            }
            return result.get();
        } catch (...) {
            return recordingFailure<T>();
        }
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> recordingFailure() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic continuity call could not be recorded."));
    }

    std::array<
        std::size_t,
        static_cast<std::size_t>(ContinuityCall::Count)> calls_{};
    std::optional<Domain::ProjectId> lastProjectId_;
    std::optional<Domain::OperationId> lastOperationId_;
    std::optional<Domain::ContinuityOperationId> lastContinuityOperationId_;
    std::optional<Domain::HandoffAcknowledgement> lastAcknowledgement_;
    std::optional<Domain::OperationId> lastCancelledOperationId_;
    Domain::MonotonicTimePoint now_{};
    std::size_t cancelCalls_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
