#include "ForgeConductor/Application/ContinuityAutomation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

constexpr std::size_t MaximumTrackedProjects = 128U;

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagate(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message), retryable));
}

[[nodiscard]] bool isCheckpointPersistedState(
    const Domain::ContinuityState state) noexcept
{
    switch (state) {
    case Domain::ContinuityState::CheckpointPersisted:
    case Domain::ContinuityState::SuccessorCreating:
    case Domain::ContinuityState::SuccessorCreated:
    case Domain::ContinuityState::BootstrapSending:
    case Domain::ContinuityState::Acknowledged:
    case Domain::ContinuityState::PredecessorSealing:
    case Domain::ContinuityState::Completed:
        return true;
    case Domain::ContinuityState::Idle:
    case Domain::ContinuityState::CheckpointPreparing:
    case Domain::ContinuityState::RetryWait:
    case Domain::ContinuityState::FailedRecoverable:
    case Domain::ContinuityState::Cancelling:
    case Domain::ContinuityState::Cancelled:
        return false;
    }
    return false;
}

[[nodiscard]] bool isActivatedHostState(
    const Domain::HostSessionStatus state) noexcept
{
    return state == Domain::HostSessionStatus::Active ||
           state == Domain::HostSessionStatus::Ready;
}

[[nodiscard]] Domain::Result<void> validateBudget(
    const Domain::ContextBudget& budget)
{
    if (budget.capacity == 0U || budget.reserved >= budget.capacity ||
        !std::isfinite(budget.confidence) || budget.confidence < 0.0 ||
        budget.confidence > 1.0) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The continuity automation budget observation is invalid."));
    }
    const auto usedAndReserved = budget.used > budget.capacity - budget.reserved
        ? budget.capacity
        : budget.used + budget.reserved;
    if (budget.remaining != budget.capacity - usedAndReserved) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The continuity automation budget remaining value is inconsistent."));
    }
    switch (budget.action) {
    case Domain::ContextBudgetAction::Normal:
    case Domain::ContextBudgetAction::Checkpoint:
    case Domain::ContextBudgetAction::Rollover:
    case Domain::ContextBudgetAction::Emergency:
        return Domain::Result<void>::success();
    }
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "The continuity automation budget action is unsupported."));
}

[[nodiscard]] Domain::Result<void> validateCheckpointBinding(
    const Domain::ContinuityHandoff& requested,
    const Domain::CheckpointOutcome& checkpoint)
{
    const auto& operation = checkpoint.operation;
    const auto& handoff = checkpoint.handoff;
    if (!isCheckpointPersistedState(operation.state) ||
        operation.projectId != requested.project.projectId ||
        operation.operationId != requested.operationId ||
        operation.handoffId != requested.handoffId ||
        operation.predecessorSessionId !=
            requested.predecessorSession.sessionId ||
        operation.adapterId != requested.hostState.adapterId ||
        handoff.project.projectId != requested.project.projectId ||
        handoff.operationId != requested.operationId ||
        handoff.handoffId != requested.handoffId ||
        handoff.predecessorSession.sessionId !=
            requested.predecessorSession.sessionId ||
        handoff.hostState.adapterId != requested.hostState.adapterId ||
        operation.projectId != handoff.project.projectId ||
        operation.operationId != handoff.operationId ||
        operation.handoffId != handoff.handoffId) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The continuity checkpoint result does not match its observation."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<Domain::SessionId> validateRolloverBinding(
    const Domain::CheckpointOutcome& checkpoint,
    const Domain::RolloverOutcome& rollover)
{
    const auto& operation = rollover.operation;
    if (operation.state != Domain::ContinuityState::Completed ||
        !rollover.acknowledged || !rollover.predecessorSealed ||
        operation.projectId != checkpoint.operation.projectId ||
        operation.operationId != checkpoint.operation.operationId ||
        operation.handoffId != checkpoint.operation.handoffId ||
        operation.predecessorSessionId !=
            checkpoint.operation.predecessorSessionId ||
        operation.adapterId != checkpoint.operation.adapterId ||
        !operation.successorSessionId ||
        operation.acknowledgedSessionId != operation.successorSessionId ||
        operation.acknowledgedHandoffId != operation.handoffId) {
        return failure<Domain::SessionId>(
            Domain::ErrorCodes::IntegrityFailure,
            "The autonomous rollover did not produce an exact acknowledged successor.");
    }
    const auto successorId = *operation.successorSessionId;
    if (rollover.successor) {
        const Domain::SessionCreationRequest request{
            operation.operationId,
            operation.projectId,
            operation.predecessorSessionId,
            operation.idempotencyKey};
        auto valid = Domain::validateHostSessionBinding(
            *rollover.successor, request);
        if (!valid || rollover.successor->id != successorId ||
            !isActivatedHostState(rollover.successor->status)) {
            return failure<Domain::SessionId>(
                Domain::ErrorCodes::IntegrityFailure,
                "The autonomous rollover host session is not the acknowledged successor.");
        }
    }
    return Domain::Result<Domain::SessionId>::success(successorId);
}

[[nodiscard]] Domain::Result<void> validateResumeBinding(
    const Domain::CheckpointOutcome& checkpoint,
    const Domain::SessionId& successorId,
    const Domain::HandoffResumeOutcome& resumed)
{
    const auto& operation = resumed.operation;
    if (operation.state != Domain::ContinuityState::Completed ||
        operation.projectId != checkpoint.operation.projectId ||
        operation.operationId != checkpoint.operation.operationId ||
        operation.handoffId != checkpoint.operation.handoffId ||
        operation.predecessorSessionId !=
            checkpoint.operation.predecessorSessionId ||
        operation.adapterId != checkpoint.operation.adapterId ||
        operation.successorSessionId != successorId ||
        operation.acknowledgedSessionId != successorId ||
        operation.acknowledgedHandoffId != operation.handoffId ||
        resumed.handoff.project.projectId != operation.projectId ||
        resumed.handoff.operationId != operation.operationId ||
        resumed.handoff.handoffId != operation.handoffId ||
        resumed.handoff.predecessorSession.sessionId !=
            operation.predecessorSessionId ||
        resumed.handoff.hostState.adapterId != operation.adapterId ||
        !resumed.handoff.successorSession ||
        resumed.handoff.successorSession->sessionId != successorId ||
        resumed.session.id != successorId ||
        !isActivatedHostState(resumed.session.status)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The autonomous successor activation does not match the durable chain."));
    }
    const Domain::SessionCreationRequest request{
        operation.operationId,
        operation.projectId,
        operation.predecessorSessionId,
        operation.idempotencyKey};
    auto valid = Domain::validateHostSessionBinding(resumed.session, request);
    if (!valid) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The resumed host session binding is invalid."));
    }
    return Domain::Result<void>::success();
}

} // namespace

class ContinuityAutomation::Impl final {
public:
    Impl(
        Contracts::IContinuityCoordinator& coordinator,
        Contracts::IClock& clock)
        : coordinator_{coordinator}, clock_{clock}
    {
        slots_.reserve(MaximumTrackedProjects);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityAutomationOutcome> observe(
        const Domain::ContinuityAutomationObservation& observation,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }
            valid = validateBudget(observation.budget);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }

            Domain::ContinuityAutomationOutcome outcome{
                observation.handoff.project.projectId,
                observation.handoff.handoffId,
                observation.budget.action,
                std::nullopt,
                std::nullopt,
                false,
                false,
                false};
            if (observation.budget.action ==
                Domain::ContextBudgetAction::Normal) {
                return Domain::Result<
                    Domain::ContinuityAutomationOutcome>::success(
                        std::move(outcome));
            }

            auto slotResult = slotFor(observation.handoff.project.projectId);
            if (!slotResult) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(slotResult));
            }
            auto slot = std::move(slotResult).value();
            std::unique_lock execution{slot->execution, std::try_to_lock};
            if (!execution.owns_lock()) {
                return failure<Domain::ContinuityAutomationOutcome>(
                    Domain::ErrorCodes::DatabaseBusy,
                    "Another continuity observation is active for this project.",
                    true);
            }
            valid = validateContext(context);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }
            ActiveCall active{*slot, context.operationId};
            valid = validateContext(context);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }

            auto checkpointResult = coordinator_.checkpoint(
                Domain::CheckpointRequest{observation.handoff}, context);
            if (!checkpointResult) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(checkpointResult));
            }
            auto checkpoint = std::move(checkpointResult).value();
            valid = validateCheckpointBinding(
                observation.handoff, checkpoint);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }
            outcome.operationId = checkpoint.operation.operationId;
            outcome.checkpointPersisted = true;
            if (observation.budget.action ==
                Domain::ContextBudgetAction::Checkpoint) {
                return Domain::Result<
                    Domain::ContinuityAutomationOutcome>::success(
                        std::move(outcome));
            }

            valid = validateContext(context);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }
            auto rolloverResult = coordinator_.requestRollover(
                Domain::RolloverRequest{
                    checkpoint.operation.projectId,
                    checkpoint.operation.operationId},
                context);
            if (!rolloverResult) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(rolloverResult));
            }
            auto rollover = std::move(rolloverResult).value();
            auto successorResult = validateRolloverBinding(
                checkpoint, rollover);
            if (!successorResult) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(successorResult));
            }
            auto successorId = std::move(successorResult).value();
            outcome.rolloverRequested = true;
            outcome.successorSessionId = successorId;

            valid = validateContext(context);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }
            auto resumeResult = coordinator_.resume(
                Domain::HandoffResumeRequest{
                    checkpoint.operation.projectId,
                    checkpoint.operation.handoffId,
                    successorId},
                context);
            if (!resumeResult) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(resumeResult));
            }
            auto resumed = std::move(resumeResult).value();
            valid = validateResumeBinding(checkpoint, successorId, resumed);
            if (!valid) {
                return propagate<Domain::ContinuityAutomationOutcome>(
                    std::move(valid));
            }
            outcome.successorActivated = true;
            return Domain::Result<
                Domain::ContinuityAutomationOutcome>::success(
                    std::move(outcome));
        } catch (...) {
            return failure<Domain::ContinuityAutomationOutcome>(
                Domain::ErrorCodes::InternalFailure,
                "The continuity automation observation failed safely.");
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            coordinator_.cancel(operationId);
        } catch (...) {
        }
    }

    [[nodiscard]] std::size_t trackedProjectCount() const noexcept
    {
        try {
            std::lock_guard lock{slotsMutex_};
            return slots_.size();
        } catch (...) {
            return 0U;
        }
    }

    void shutdown() noexcept
    {
        try {
            if (shutdownRequested_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            std::array<std::shared_ptr<ProjectSlot>, MaximumTrackedProjects>
                slots;
            std::size_t slotCount{};
            {
                std::lock_guard lock{slotsMutex_};
                slotCount = slots_.size();
                for (std::size_t index = 0U; index < slotCount; ++index) {
                    slots[index] = slots_[index];
                }
                slots_.clear();
            }
            for (std::size_t index = 0U; index < slotCount; ++index) {
                try {
                    std::optional<Domain::OperationId> operationId;
                    {
                        std::lock_guard lock{slots[index]->stateMutex};
                        operationId = slots[index]->activeOperationId;
                    }
                    if (operationId) {
                        coordinator_.cancel(*operationId);
                    }
                } catch (...) {
                    // Continue cancelling the remaining bounded operations.
                }
            }
        } catch (...) {
            // Shutdown is idempotent and best-effort at the noexcept boundary.
        }
    }

private:
    struct ProjectSlot final {
        explicit ProjectSlot(Domain::ProjectId value)
            : projectId{std::move(value)}
        {
        }

        Domain::ProjectId projectId;
        std::mutex execution;
        std::mutex stateMutex;
        std::optional<Domain::OperationId> activeOperationId;
    };

    class ActiveCall final {
    public:
        ActiveCall(ProjectSlot& slot, const Domain::OperationId& operationId)
            : slot_{slot}
        {
            std::lock_guard lock{slot_.stateMutex};
            slot_.activeOperationId = operationId;
        }

        ~ActiveCall() noexcept
        {
            try {
                std::lock_guard lock{slot_.stateMutex};
                slot_.activeOperationId.reset();
            } catch (...) {
            }
        }

        ActiveCall(const ActiveCall&) = delete;
        ActiveCall& operator=(const ActiveCall&) = delete;
        ActiveCall(ActiveCall&&) = delete;
        ActiveCall& operator=(ActiveCall&&) = delete;

    private:
        ProjectSlot& slot_;
    };

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept
    {
        try {
            if (shutdownRequested_.load(std::memory_order_acquire)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The continuity automation is shutting down."));
            }
            if (context.isCancellationRequested()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The continuity automation observation was cancelled."));
            }
            if (context.isExpired(clock_.monotonicNow())) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The continuity automation observation exceeded its deadline."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The continuity automation context could not be validated."));
        }
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<ProjectSlot>> slotFor(
        const Domain::ProjectId& projectId)
    {
        std::lock_guard lock{slotsMutex_};
        if (shutdownRequested_.load(std::memory_order_acquire)) {
            return failure<std::shared_ptr<ProjectSlot>>(
                Domain::ErrorCodes::TransportClosed,
                "The continuity automation is shutting down.");
        }
        const auto found = std::find_if(
            slots_.begin(),
            slots_.end(),
            [&](const auto& slot) { return slot->projectId == projectId; });
        if (found != slots_.end()) {
            return Domain::Result<std::shared_ptr<ProjectSlot>>::success(*found);
        }
        if (slots_.size() >= MaximumTrackedProjects) {
            return failure<std::shared_ptr<ProjectSlot>>(
                Domain::ErrorCodes::LimitExceeded,
                "The continuity automation project bound of 128 was reached.");
        }
        auto slot = std::make_shared<ProjectSlot>(projectId);
        slots_.push_back(slot);
        return Domain::Result<std::shared_ptr<ProjectSlot>>::success(
            std::move(slot));
    }

    Contracts::IContinuityCoordinator& coordinator_;
    Contracts::IClock& clock_;
    std::atomic_bool shutdownRequested_{};
    mutable std::mutex slotsMutex_;
    std::vector<std::shared_ptr<ProjectSlot>> slots_;
};

ContinuityAutomation::ContinuityAutomation(
    Contracts::IContinuityCoordinator& coordinator,
    Contracts::IClock& clock)
    : implementation_{std::make_unique<Impl>(coordinator, clock)}
{
}

ContinuityAutomation::~ContinuityAutomation() noexcept
{
    shutdown();
}

Domain::Result<Domain::ContinuityAutomationOutcome>
ContinuityAutomation::observe(
    const Domain::ContinuityAutomationObservation& observation,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ContinuityAutomationOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity automation has no implementation.");
    }
    return implementation_->observe(observation, context);
}

void ContinuityAutomation::cancel(
    const Domain::OperationId& operationId) noexcept
{
    if (implementation_) {
        implementation_->cancel(operationId);
    }
}

std::size_t ContinuityAutomation::trackedProjectCount() const noexcept
{
    return implementation_ ? implementation_->trackedProjectCount() : 0U;
}

void ContinuityAutomation::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Application
