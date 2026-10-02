#pragma once

#include "BoundedFakeSupport.h"
#include "ForgeConductor/Contracts/IContinuityCoordinator.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <limits>
#include <string>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

class ContinuityRepositoryFake final
    : public Contracts::IContinuityRepository {
public:
    explicit ContinuityRepositoryFake(
        Domain::ProjectId projectId,
        const Domain::MonotonicTimePoint now = {})
        : projectId_{std::move(projectId)}, gate_{now}
    {
    }

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept override
    {
        return projectId_;
    }

    [[nodiscard]] Domain::Result<void> seedOperation(
        Domain::ContinuityOperation operation) noexcept
    {
        try {
            if (operation.projectId != projectId_) {
                return projectMismatch<void>(
                    "The continuity operation belongs to another project.");
            }
            auto validRetry = Domain::validateContinuityOperationRetryState(operation);
            if (!validRetry) {
                return Domain::Result<void>::failure(
                    std::move(validRetry).error());
            }
            operation_.emplace(std::move(operation));
            transitionCount_ = 0U;
            if (operation_->state == Domain::ContinuityState::Completed) {
                activeSession_ = operation_->successorSessionId;
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> createOperation(
        const Domain::ContinuityHandoff& handoff,
        const Domain::IdempotencyKey& idempotencyKey,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ContinuityOperation>(
                    std::move(accepted));
            }
            if (handoff.project.projectId != projectId_) {
                return projectMismatch<Domain::ContinuityOperation>(
                    "The continuity handoff belongs to another project.");
            }
            if (operation_) {
                if (operation_->idempotencyKey == idempotencyKey &&
                    operation_->operationId == handoff.operationId &&
                    operation_->handoffId == handoff.handoffId &&
                    operation_->predecessorSessionId ==
                        handoff.predecessorSession.sessionId &&
                    operation_->adapterId == handoff.hostState.adapterId) {
                    return Domain::Result<Domain::ContinuityOperation>::success(
                        *operation_);
                }
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "A different deterministic continuity operation is active."));
            }

            operation_.emplace(Domain::ContinuityOperation{
                handoff.operationId,
                projectId_,
                handoff.predecessorSession.sessionId,
                std::nullopt,
                handoff.handoffId,
                Domain::ContinuityState::Idle,
                0U,
                handoff.hostState.adapterId,
                idempotencyKey,
                std::nullopt,
                std::nullopt,
                handoff.createdAt,
                handoff.createdAt,
                std::nullopt,
                std::nullopt,
                handoff.contentSha256,
                std::nullopt});
            transitionCount_ = 1U;
            return Domain::Result<Domain::ContinuityOperation>::success(*operation_);
        } catch (...) {
            return fakeInternalFailure<Domain::ContinuityOperation>();
        }
    }

    [[nodiscard]] Domain::Result<void> storeHandoff(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return accepted;
            }
            if (handoff.project.projectId != projectId_) {
                return projectMismatch<void>(
                    "The continuity handoff belongs to another project.");
            }
            if (!operation_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "The handoff continuity operation was not found."));
            }
            if (operation_->operationId != handoff.operationId ||
                operation_->handoffId != handoff.handoffId ||
                operation_->predecessorSessionId !=
                    handoff.predecessorSession.sessionId ||
                operation_->adapterId != handoff.hostState.adapterId ||
                (operation_->state != Domain::ContinuityState::Idle &&
                 operation_->state != Domain::ContinuityState::CheckpointPreparing)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The handoff does not match checkpoint preparation."));
            }
            if (handoff_ && handoff_->handoffId != handoff.handoffId) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The deterministic continuity handoff slot is occupied."));
            }
            if (operation_->acknowledgedSessionId) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The deterministic continuity handoff is already acknowledged."));
            }
            handoff_ = handoff;
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>> handoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<
                    std::optional<Domain::ContinuityHandoff>>(std::move(accepted));
            }
            if (projectId != projectId_) {
                return projectMismatch<std::optional<Domain::ContinuityHandoff>>(
                    "The continuity lookup belongs to another project.");
            }
            if (!handoff_ || handoff_->handoffId != handoffId) {
                return Domain::Result<
                    std::optional<Domain::ContinuityHandoff>>::success(std::nullopt);
            }
            return Domain::Result<
                std::optional<Domain::ContinuityHandoff>>::success(handoff_);
        } catch (...) {
            return fakeInternalFailure<std::optional<Domain::ContinuityHandoff>>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    operation(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<
                    std::optional<Domain::ContinuityOperation>>(std::move(accepted));
            }
            if (projectId != projectId_) {
                return projectMismatch<std::optional<Domain::ContinuityOperation>>(
                    "The continuity operation lookup belongs to another project.");
            }
            if (!operation_ || operation_->operationId != operationId) {
                return Domain::Result<
                    std::optional<Domain::ContinuityOperation>>::success(std::nullopt);
            }
            return Domain::Result<
                std::optional<Domain::ContinuityOperation>>::success(operation_);
        } catch (...) {
            return fakeInternalFailure<std::optional<Domain::ContinuityOperation>>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    activeOperation(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<
                    std::optional<Domain::ContinuityOperation>>(std::move(accepted));
            }
            if (projectId != projectId_) {
                return projectMismatch<std::optional<Domain::ContinuityOperation>>(
                    "The active continuity lookup belongs to another project.");
            }
            if (operation_ && Domain::isTerminal(operation_->state)) {
                return Domain::Result<
                    std::optional<Domain::ContinuityOperation>>::success(std::nullopt);
            }
            return Domain::Result<
                std::optional<Domain::ContinuityOperation>>::success(operation_);
        } catch (...) {
            return fakeInternalFailure<std::optional<Domain::ContinuityOperation>>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> compareAndSet(
        const Domain::ContinuityOperationId& operationId,
        const Domain::ContinuityState expected,
        const Domain::ContinuityState next,
        std::optional<Domain::SessionId> successorSessionId,
        std::optional<std::string> evidence,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ContinuityOperation>(
                    std::move(accepted));
            }
            auto found = requireOperation(operationId);
            if (!found) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    std::move(found).error());
            }
            if (operation_->state == next) {
                if (successorSessionId &&
                    operation_->successorSessionId != successorSessionId) {
                    return Domain::Result<Domain::ContinuityOperation>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The completed transition has another successor."));
                }
                return Domain::Result<Domain::ContinuityOperation>::success(
                    *operation_);
            }
            if (operation_->state != expected) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The deterministic continuity state changed concurrently."));
            }
            if (!Domain::isAllowedContinuityTransition(expected, next)) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The requested continuity transition is invalid."));
            }
            if (expected == Domain::ContinuityState::RetryWait &&
                operation_->retryResumeState != next) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The retry operation is bound to another resume state."));
            }
            auto successor = successorSessionId
                ? successorSessionId
                : operation_->successorSessionId;
            if (next == Domain::ContinuityState::SuccessorCreated && !successor) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "A successor session is required for successor-created state."));
            }
            if (next == Domain::ContinuityState::CheckpointPersisted && !handoff_) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Checkpoint persistence requires a durable handoff."));
            }
            if ((next == Domain::ContinuityState::PredecessorSealing ||
                 next == Domain::ContinuityState::Completed) &&
                (!operation_->acknowledgedSessionId ||
                 !operation_->acknowledgedHandoffId || !successor ||
                 operation_->acknowledgedSessionId != successor ||
                 operation_->acknowledgedHandoffId !=
                     std::optional<Domain::ContinuityHandoffId>{
                         operation_->handoffId})) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Predecessor sealing requires an exact acknowledgement."));
            }
            if (operation_->attempt ==
                (std::numeric_limits<std::uint32_t>::max)()) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The continuity transition attempt bound was reached."));
            }
            if (next == Domain::ContinuityState::SuccessorCreated && handoff_ &&
                successor) {
                handoff_->successorSession = Domain::ContinuitySession{
                    *successor, std::nullopt, std::nullopt, std::nullopt};
            }
            operation_->successorSessionId = successor;
            operation_->state = next;
            ++operation_->attempt;
            operation_->lastError.reset();
            operation_->retryResumeState.reset();
            operation_->retryAt.reset();
            if (evidence && evidence->size() > MaximumEvidenceBytes) {
                evidence->resize(MaximumEvidenceBytes);
            }
            lastTransitionEvidence_ = std::move(evidence);
            ++transitionCount_;
            if (next == Domain::ContinuityState::Completed) {
                activeSession_ = operation_->successorSessionId;
            }
            return Domain::Result<Domain::ContinuityOperation>::success(*operation_);
        } catch (...) {
            return fakeInternalFailure<Domain::ContinuityOperation>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> acknowledge(
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastAcknowledgement_ = acknowledgement;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ContinuityOperation>(
                    std::move(accepted));
            }
            auto found = requireOperation(operationId);
            if (!found || !handoff_) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    found ? Domain::makeError(
                                Domain::ErrorCodes::RecordNotFound,
                                "The acknowledgement handoff was not found.")
                          : std::move(found).error());
            }
            auto valid = Domain::validateHandoffAcknowledgement(
                *operation_, *handoff_, acknowledgement);
            if (!valid) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    std::move(valid).error());
            }
            if (operation_->state == Domain::ContinuityState::Acknowledged ||
                operation_->state == Domain::ContinuityState::PredecessorSealing ||
                operation_->state == Domain::ContinuityState::Completed) {
                if (operation_->acknowledgedSessionId !=
                        std::optional<Domain::SessionId>{
                            acknowledgement.successorSessionId} ||
                    operation_->acknowledgedHandoffId !=
                        std::optional<Domain::ContinuityHandoffId>{
                            acknowledgement.handoffId}) {
                    return Domain::Result<Domain::ContinuityOperation>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "Another acknowledgement is already durable."));
                }
                return Domain::Result<Domain::ContinuityOperation>::success(
                    *operation_);
            }
            if (operation_->state != Domain::ContinuityState::BootstrapSending) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The operation is not awaiting a handoff acknowledgement."));
            }
            if (operation_->attempt ==
                (std::numeric_limits<std::uint32_t>::max)()) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The continuity transition attempt bound was reached."));
            }
            operation_->acknowledgedSessionId = acknowledgement.successorSessionId;
            operation_->acknowledgedHandoffId = acknowledgement.handoffId;
            operation_->state = Domain::ContinuityState::Acknowledged;
            ++operation_->attempt;
            operation_->lastError.reset();
            operation_->retryAt.reset();
            operation_->retryResumeState.reset();
            ++transitionCount_;
            return Domain::Result<Domain::ContinuityOperation>::success(*operation_);
        } catch (...) {
            return fakeInternalFailure<Domain::ContinuityOperation>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> recordRetry(
        const Domain::ContinuityOperationId& operationId,
        const Domain::ContinuityState resumeState,
        std::string error,
        std::optional<Domain::UtcTimePoint> retryAt,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ContinuityOperation>(
                    std::move(accepted));
            }
            auto found = requireOperation(operationId);
            if (!found) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    std::move(found).error());
            }
            if (!Domain::isRetryResumeState(resumeState) || error.empty()) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The retry record does not describe a recoverable transition."));
            }
            if (error.size() > MaximumErrorBytes) {
                error.resize(MaximumErrorBytes);
            }
            const auto effectiveRetryAt = retryAt.value_or(
                Domain::UtcTimePoint{} + std::chrono::seconds{1});
            if (operation_->state == Domain::ContinuityState::RetryWait) {
                if (operation_->retryResumeState != resumeState) {
                    return Domain::Result<Domain::ContinuityOperation>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The durable retry is bound to another resume state."));
                }
                operation_->lastError = std::move(error);
                operation_->retryAt = effectiveRetryAt;
                return Domain::Result<Domain::ContinuityOperation>::success(
                    *operation_);
            }
            if (operation_->state != resumeState ||
                !Domain::isAllowedContinuityTransition(
                    operation_->state,
                    Domain::ContinuityState::FailedRecoverable) ||
                operation_->attempt >
                    (std::numeric_limits<std::uint32_t>::max)() - 2U) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The continuity operation cannot enter retry wait from its current state."));
            }
            operation_->state = Domain::ContinuityState::RetryWait;
            operation_->retryResumeState = resumeState;
            operation_->lastError = std::move(error);
            operation_->retryAt = effectiveRetryAt;
            operation_->attempt += 2U;
            transitionCount_ += 2U;
            auto valid = Domain::validateContinuityOperationRetryState(*operation_);
            if (!valid) {
                return Domain::Result<Domain::ContinuityOperation>::failure(
                    std::move(valid).error());
            }
            return Domain::Result<Domain::ContinuityOperation>::success(*operation_);
        } catch (...) {
            return fakeInternalFailure<Domain::ContinuityOperation>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::SessionId>> activeSession(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<std::optional<Domain::SessionId>>(
                    std::move(accepted));
            }
            if (projectId != projectId_) {
                return projectMismatch<std::optional<Domain::SessionId>>(
                    "The active-session lookup belongs to another project.");
            }
            return Domain::Result<std::optional<Domain::SessionId>>::success(
                activeSession_);
        } catch (...) {
            return fakeInternalFailure<std::optional<Domain::SessionId>>();
        }
    }

    [[nodiscard]] Domain::Result<std::size_t> transitionCount(
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<std::size_t>(std::move(accepted));
            }
            return Domain::Result<std::size_t>::success(
                operation_ && operation_->operationId == operationId
                    ? transitionCount_
                    : 0U);
        } catch (...) {
            return fakeInternalFailure<std::size_t>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ContinuityStatus>(
                    std::move(accepted));
            }
            if (projectId != projectId_) {
                return projectMismatch<Domain::ContinuityStatus>(
                    "The continuity status belongs to another project.");
            }
            const auto active = operation_ && !Domain::isTerminal(operation_->state)
                                    ? operation_
                                    : std::nullopt;
            return Domain::Result<Domain::ContinuityStatus>::success(
                Domain::ContinuityStatus{
                    projectId_, active, operation_ ? 1U : 0U, handoff_ ? 1U : 0U,
                    active.has_value()});
        } catch (...) {
            return fakeInternalFailure<Domain::ContinuityStatus>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport> resetContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastResetRequest_ = request;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ContinuityResetReport>(
                    std::move(accepted));
            }
            if (request.projectId != projectId_) {
                return projectMismatch<Domain::ContinuityResetReport>(
                    "The continuity reset belongs to another project.");
            }
            auto validConfirmation = Domain::validateDestructiveConfirmation(
                request.confirmation,
                "reset_project_continuity",
                projectId_.value(),
                "RESET PROJECT CONTINUITY " + projectId_.value());
            if (!validConfirmation) {
                return Domain::Result<Domain::ContinuityResetReport>::failure(
                    std::move(validConfirmation).error());
            }
            const auto handoffs = handoff_ ? 1U : 0U;
            const auto operations = operation_ ? 1U : 0U;
            const auto events = transitionCount_ + (activeSession_ ? 1U : 0U);
            operation_.reset();
            handoff_.reset();
            activeSession_.reset();
            transitionCount_ = 0U;
            lastAcknowledgement_.reset();
            lastTransitionEvidence_.reset();
            return Domain::Result<Domain::ContinuityResetReport>::success(
                Domain::ContinuityResetReport{
                    projectId_,
                    Domain::ResetReport{
                        request.confirmation.action,
                        request.confirmation.scope,
                        1U,
                        handoffs,
                        operations,
                        events,
                        true}});
        } catch (...) {
            return fakeInternalFailure<Domain::ContinuityResetReport>();
        }
    }

    void close() noexcept override { gate_.close(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] const std::optional<Domain::ContinuityHandoff>&
    storedHandoff() const noexcept
    {
        return handoff_;
    }

    [[nodiscard]] const std::optional<Domain::ContinuityOperation>&
    storedOperation() const noexcept
    {
        return operation_;
    }

    [[nodiscard]] const std::optional<Domain::HandoffAcknowledgement>&
    lastAcknowledgement() const noexcept
    {
        return lastAcknowledgement_;
    }

    [[nodiscard]] const std::optional<std::string>&
    lastTransitionEvidence() const noexcept
    {
        return lastTransitionEvidence_;
    }

    [[nodiscard]] const std::optional<Domain::ContinuityResetRequest>&
    lastResetRequest() const noexcept
    {
        return lastResetRequest_;
    }

private:
    static constexpr std::size_t MaximumEvidenceBytes = 2U * 1024U;
    static constexpr std::size_t MaximumErrorBytes = 2U * 1024U;

    template <typename T>
    [[nodiscard]] static Domain::Result<T> projectMismatch(
        std::string message)
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::ProjectScopeMismatch, std::move(message)));
    }

    [[nodiscard]] Domain::Result<void> requireOperation(
        const Domain::ContinuityOperationId& operationId) const
    {
        if (!operation_ || operation_->operationId != operationId) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound,
                "The deterministic continuity operation was not found."));
        }
        return Domain::Result<void>::success();
    }

    Domain::ProjectId projectId_;
    std::optional<Domain::ContinuityHandoff> handoff_;
    std::optional<Domain::ContinuityOperation> operation_;
    std::optional<Domain::SessionId> activeSession_;
    std::optional<Domain::HandoffAcknowledgement> lastAcknowledgement_;
    std::optional<std::string> lastTransitionEvidence_;
    std::optional<Domain::ContinuityResetRequest> lastResetRequest_;
    std::size_t transitionCount_{};
    BoundedFakeOperationGate gate_;
};

} // namespace ForgeConductor::Tests::Fakes
