#include "ForgeConductor/Domain/ContinuityModels.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] bool isBlank(const std::string_view value)
{
    return value.empty() || std::all_of(value.begin(), value.end(), [](const unsigned char value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    });
}

[[nodiscard]] bool isSafeText(const std::string_view value) noexcept
{
    return value.find('\0') == std::string_view::npos && isValidUtf8(value);
}

[[nodiscard]] bool isValidBudgetSource(const std::string_view value) noexcept
{
    return !isBlank(value) && value.size() <= 128U && isSafeText(value);
}

template <typename T>
[[nodiscard]] bool withinListCap(const std::vector<T>& values) noexcept
{
    return values.size() <= MaximumContinuityHandoffListItems;
}

} // namespace

std::string_view wireName(const ContinuityState value) noexcept
{
    switch (value) {
    case ContinuityState::Idle: return "idle";
    case ContinuityState::CheckpointPreparing: return "checkpoint_preparing";
    case ContinuityState::CheckpointPersisted: return "checkpoint_persisted";
    case ContinuityState::SuccessorCreating: return "successor_creating";
    case ContinuityState::SuccessorCreated: return "successor_created";
    case ContinuityState::BootstrapSending: return "bootstrap_sending";
    case ContinuityState::Acknowledged: return "acknowledged";
    case ContinuityState::PredecessorSealing: return "predecessor_sealing";
    case ContinuityState::Completed: return "completed";
    case ContinuityState::RetryWait: return "retry_wait";
    case ContinuityState::FailedRecoverable: return "failed_recoverable";
    case ContinuityState::Cancelling: return "cancelling";
    case ContinuityState::Cancelled: return "cancelled";
    }
    return "idle";
}

Result<ContinuityState> parseContinuityStateWireName(
    const std::string_view value)
{
    static constexpr std::pair<std::string_view, ContinuityState> Names[]{
        {"idle", ContinuityState::Idle},
        {"active", ContinuityState::Idle},
        {"checkpoint_preparing", ContinuityState::CheckpointPreparing},
        {"checkpointPreparing", ContinuityState::CheckpointPreparing},
        {"checkpoint_persisted", ContinuityState::CheckpointPersisted},
        {"checkpointPersisted", ContinuityState::CheckpointPersisted},
        {"successor_creating", ContinuityState::SuccessorCreating},
        {"successorRequested", ContinuityState::SuccessorCreating},
        {"successor_created", ContinuityState::SuccessorCreated},
        {"successorCreated", ContinuityState::SuccessorCreated},
        {"bootstrap_sending", ContinuityState::BootstrapSending},
        {"successorBootstrapping", ContinuityState::BootstrapSending},
        {"acknowledged", ContinuityState::Acknowledged},
        {"successorAcknowledged", ContinuityState::Acknowledged},
        {"predecessor_sealing", ContinuityState::PredecessorSealing},
        {"completed", ContinuityState::Completed},
        {"predecessorSealed", ContinuityState::Completed},
        {"retry_wait", ContinuityState::RetryWait},
        {"failed_recoverable", ContinuityState::FailedRecoverable},
        {"cancelling", ContinuityState::Cancelling},
        {"cancelled", ContinuityState::Cancelled},
    };
    const auto match = std::find_if(
        std::begin(Names),
        std::end(Names),
        [value](const auto& item) { return item.first == value; });
    if (match == std::end(Names)) {
        return Result<ContinuityState>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Continuity state has an unsupported wire representation."));
    }
    return Result<ContinuityState>::success(match->second);
}

bool isTerminal(const ContinuityState value) noexcept
{
    return value == ContinuityState::Completed || value == ContinuityState::Cancelled;
}

bool isRetryResumeState(const ContinuityState value) noexcept
{
    return value == ContinuityState::CheckpointPreparing ||
           value == ContinuityState::SuccessorCreating ||
           value == ContinuityState::BootstrapSending ||
           value == ContinuityState::PredecessorSealing;
}

bool isAllowedContinuityTransition(
    const ContinuityState from,
    const ContinuityState to) noexcept
{
    if (from == to || isTerminal(from)) {
        return false;
    }
    if (to == ContinuityState::Cancelling && from != ContinuityState::Cancelling) {
        return true;
    }
    if (to == ContinuityState::FailedRecoverable &&
        from != ContinuityState::Cancelling && from != ContinuityState::RetryWait) {
        return true;
    }
    switch (from) {
    case ContinuityState::Idle:
        return to == ContinuityState::CheckpointPreparing;
    case ContinuityState::CheckpointPreparing:
        return to == ContinuityState::CheckpointPersisted;
    case ContinuityState::CheckpointPersisted:
        return to == ContinuityState::SuccessorCreating;
    case ContinuityState::SuccessorCreating:
        return to == ContinuityState::SuccessorCreated;
    case ContinuityState::SuccessorCreated:
        return to == ContinuityState::BootstrapSending;
    case ContinuityState::BootstrapSending:
        return to == ContinuityState::Acknowledged;
    case ContinuityState::Acknowledged:
        return to == ContinuityState::PredecessorSealing;
    case ContinuityState::PredecessorSealing:
        return to == ContinuityState::Completed;
    case ContinuityState::FailedRecoverable:
        return to == ContinuityState::RetryWait;
    case ContinuityState::RetryWait:
        return to == ContinuityState::CheckpointPreparing ||
               to == ContinuityState::SuccessorCreating ||
               to == ContinuityState::BootstrapSending ||
               to == ContinuityState::PredecessorSealing;
    case ContinuityState::Cancelling:
        return to == ContinuityState::Cancelled;
    case ContinuityState::Completed:
    case ContinuityState::Cancelled:
        return false;
    }
    return false;
}

ContinuityState canonicalState(const LegacyContinuityState value) noexcept
{
    switch (value) {
    case LegacyContinuityState::Active: return ContinuityState::Idle;
    case LegacyContinuityState::CheckpointPreparing:
        return ContinuityState::CheckpointPreparing;
    case LegacyContinuityState::CheckpointPersisted:
        return ContinuityState::CheckpointPersisted;
    case LegacyContinuityState::SuccessorRequested:
        return ContinuityState::SuccessorCreating;
    case LegacyContinuityState::SuccessorCreated:
        return ContinuityState::SuccessorCreated;
    case LegacyContinuityState::SuccessorBootstrapping:
        return ContinuityState::BootstrapSending;
    case LegacyContinuityState::SuccessorAcknowledged:
        return ContinuityState::Acknowledged;
    case LegacyContinuityState::PredecessorSealed:
        return ContinuityState::Completed;
    }
    return ContinuityState::Idle;
}

Result<ContextBudget> evaluateContextBudget(
    const std::uint64_t capacity,
    const std::uint64_t used,
    const std::uint64_t reserved,
    const ContextBudgetSource source,
    const double confidence,
    const double checkpointReserveFraction,
    const double rolloverReserveFraction)
{
    if (capacity == 0 || reserved >= capacity || !std::isfinite(confidence) ||
        confidence < 0.0 || confidence > 1.0 ||
        !std::isfinite(checkpointReserveFraction) ||
        !std::isfinite(rolloverReserveFraction) ||
        rolloverReserveFraction <= 0.0 ||
        rolloverReserveFraction > checkpointReserveFraction ||
        checkpointReserveFraction >= 1.0) {
        return Result<ContextBudget>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Context budget values or reserve fractions are invalid."));
    }

    const auto usedAndReserved = used > capacity - reserved ? capacity : used + reserved;
    const auto remaining = capacity - usedAndReserved;
    const auto usable = capacity - reserved;
    const auto fraction = static_cast<double>(remaining) / static_cast<double>(usable);
    auto action = ContextBudgetAction::Normal;
    if (source == ContextBudgetSource::ProviderOverflow) {
        action = ContextBudgetAction::Emergency;
    } else if (fraction <= rolloverReserveFraction) {
        action = ContextBudgetAction::Rollover;
    } else if (fraction <= checkpointReserveFraction) {
        action = ContextBudgetAction::Checkpoint;
    }
    return Result<ContextBudget>::success(ContextBudget{
        capacity, used, reserved, remaining, source, confidence, action});
}

Result<ContextBudget> estimateContextBudget(
    const std::uint64_t capacity,
    const std::size_t serializedBytes,
    const std::uint64_t reserved)
{
    const auto estimated = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(serializedBytes) / 3.5));
    return evaluateContextBudget(
        capacity,
        estimated,
        reserved,
        ContextBudgetSource::SerializedEstimate,
        0.65);
}

Result<ContextBudget> resolveContextBudget(
    const ContextBudgetSignals& signals,
    const double checkpointReserveFraction,
    const double rolloverReserveFraction)
{
    if (signals.capacity == 0U || signals.reserved >= signals.capacity) {
        return Result<ContextBudget>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Context budget signal capacity and reserve are invalid."));
    }

    if (signals.providerOverflow) {
        return evaluateContextBudget(
            signals.capacity,
            signals.capacity,
            signals.reserved,
            ContextBudgetSource::ProviderOverflow,
            1.0,
            checkpointReserveFraction,
            rolloverReserveFraction);
    }
    if (signals.providerRemaining) {
        if (*signals.providerRemaining > signals.capacity) {
            return Result<ContextBudget>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Provider-reported remaining context exceeds model capacity."));
        }
        return evaluateContextBudget(
            signals.capacity,
            signals.capacity - *signals.providerRemaining,
            signals.reserved,
            ContextBudgetSource::ProviderRemaining,
            1.0,
            checkpointReserveFraction,
            rolloverReserveFraction);
    }
    if (signals.providerUsed) {
        return evaluateContextBudget(
            signals.capacity,
            *signals.providerUsed,
            signals.reserved,
            ContextBudgetSource::ProviderUsage,
            1.0,
            checkpointReserveFraction,
            rolloverReserveFraction);
    }
    if (signals.configuredModelUsed) {
        return evaluateContextBudget(
            signals.capacity,
            *signals.configuredModelUsed,
            signals.reserved,
            ContextBudgetSource::ConfiguredModelEstimate,
            0.85,
            checkpointReserveFraction,
            rolloverReserveFraction);
    }
    if (signals.serializedBytes) {
        const auto estimated = static_cast<std::uint64_t>(std::ceil(
            static_cast<double>(*signals.serializedBytes) / 3.5));
        return evaluateContextBudget(
            signals.capacity,
            estimated,
            signals.reserved,
            ContextBudgetSource::SerializedEstimate,
            0.65,
            checkpointReserveFraction,
            rolloverReserveFraction);
    }
    return Result<ContextBudget>::failure(makeError(
        ErrorCodes::InvalidRequest,
        "No context budget signal is available."));
}

Result<void> validateContinuityHandoff(
    const ContinuityHandoff& handoff,
    const std::size_t encodedBytes)
{
    if (encodedBytes > MaximumContinuityHandoffEncodedBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            "Continuity handoff exceeds 131072 encoded bytes."));
    }
    if (isBlank(handoff.project.displayName) ||
        isBlank(handoff.project.repositoryRoot.value()) ||
        isBlank(handoff.project.branch) ||
        isBlank(handoff.project.commit) || isBlank(handoff.mission) ||
        isBlank(handoff.currentWork.phaseId) || isBlank(handoff.currentWork.workItemId) ||
        isBlank(handoff.currentWork.summary) || handoff.nextActions.empty()) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Continuity handoff semantic sections are incomplete."));
    }
    if (!withinListCap(handoff.project.dirtySummary) ||
        !withinListCap(handoff.constraints) ||
        !withinListCap(handoff.currentWork.activeFiles) ||
        !withinListCap(handoff.completedWork) || !withinListCap(handoff.openWork) ||
        !withinListCap(handoff.decisions) ||
        !withinListCap(handoff.validation.passedGates) ||
        !withinListCap(handoff.validation.openGates) ||
        !withinListCap(handoff.validation.commands) ||
        !withinListCap(handoff.memoryReferences) ||
        !withinListCap(handoff.evidenceReferences) ||
        !withinListCap(handoff.nextActions)) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Continuity handoff list exceeds 128 items."));
    }
    for (const auto& action : handoff.nextActions) {
        if (action.order == 0 || isBlank(action.action) || isBlank(action.successCondition)) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Continuity next action is incomplete."));
        }
        if (!isSafeText(action.command)) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Continuity next-action command is not valid UTF-8 text."));
        }
    }
    if (!isValidBudgetSource(handoff.hostState.contextBudgetSource)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Continuity context budget source is invalid."));
    }
    if (handoff.hostState.persistedContinuityStateName) {
        const auto parsed = parseContinuityStateWireName(
            *handoff.hostState.persistedContinuityStateName);
        if (!parsed || parsed.value() != handoff.hostState.continuityState) {
            return Result<void>::failure(makeError(
                ErrorCodes::IntegrityFailure,
                "Persisted continuity state spelling does not match its typed state."));
        }
    }
    if (handoff.hostState.continuityState == ContinuityState::RetryWait) {
        if (!handoff.hostState.retry.retryResumeState ||
            !handoff.hostState.retry.retryAt ||
            !isRetryResumeState(*handoff.hostState.retry.retryResumeState)) {
            return Result<void>::failure(makeError(
                ErrorCodes::IntegrityFailure,
                "A retry-wait handoff requires an exact resumable state and retry time."));
        }
        if (handoff.hostState.retry.persistedRetryResumeStateName) {
            const auto parsed = parseContinuityStateWireName(
                *handoff.hostState.retry.persistedRetryResumeStateName);
            if (!parsed || parsed.value() !=
                    *handoff.hostState.retry.retryResumeState) {
                return Result<void>::failure(makeError(
                    ErrorCodes::IntegrityFailure,
                    "Persisted retry-resume state does not match its typed state."));
            }
        }
    } else if (handoff.hostState.retry.retryResumeState ||
               handoff.hostState.retry.persistedRetryResumeStateName) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "Only a retry-wait handoff may retain a retry resume state."));
    }
    for (const auto& reference : handoff.evidenceReferences) {
        if (reference.evidenceId.has_value() == reference.path.has_value()) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Continuity evidence reference must contain exactly one typed value."));
        }
    }
    if (!isSafeText(handoff.project.repositoryRoot.value())) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Continuity repository root is not valid UTF-8 text."));
    }
    if (!handoff.redactionComplete) {
        return Result<void>::failure(makeError(
            ErrorCodes::RedactionRejected,
            "Continuity handoff has not completed redaction."));
    }
    return Result<void>::success();
}

Result<void> validateContinuityOperationRetryState(
    const ContinuityOperation& operation)
{
    if (operation.state == ContinuityState::RetryWait) {
        if (!operation.retryResumeState || !operation.retryAt ||
            !isRetryResumeState(*operation.retryResumeState)) {
            return Result<void>::failure(makeError(
                ErrorCodes::IntegrityFailure,
                "A retry-wait operation requires an exact resumable state and retry time."));
        }
        return Result<void>::success();
    }
    if (operation.retryResumeState) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "Only a retry-wait operation may retain a retry resume state."));
    }
    return Result<void>::success();
}

Result<void> validateHostSessionBinding(
    const HostSession& session,
    const SessionCreationRequest& request)
{
    if (session.projectId != request.projectId) {
        return Result<void>::failure(makeError(
            ErrorCodes::ProjectScopeMismatch,
            "Host session project does not match its creation request."));
    }
    if (session.operationId != request.operationId ||
        session.predecessorSessionId != request.predecessorSessionId ||
        session.idempotencyKey != request.idempotencyKey) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "Host session operation, predecessor, or idempotency binding is invalid."));
    }
    return Result<void>::success();
}

Result<void> validateBootstrapCompatibility(
    const HostSession& session,
    const ContinuityHandoff& handoff)
{
    if (session.projectId != handoff.project.projectId) {
        return Result<void>::failure(makeError(
            ErrorCodes::ProjectScopeMismatch,
            "Host session cannot bootstrap a handoff from another project."));
    }
    if (session.operationId != handoff.operationId ||
        session.predecessorSessionId != handoff.predecessorSession.sessionId ||
        !handoff.successorSession ||
        session.id != handoff.successorSession->sessionId) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "Host session does not match the handoff operation or session chain."));
    }
    return Result<void>::success();
}

Result<void> validateHandoffAcknowledgement(
    const ContinuityOperation& operation,
    const ContinuityHandoff& handoff,
    const HandoffAcknowledgement& acknowledgement)
{
    if (operation.projectId != handoff.project.projectId) {
        return Result<void>::failure(makeError(
            ErrorCodes::ProjectScopeMismatch,
            "Continuity operation and handoff belong to different projects."));
    }
    if (operation.operationId != handoff.operationId ||
        operation.predecessorSessionId != handoff.predecessorSession.sessionId ||
        !operation.successorSessionId || !handoff.successorSession ||
        operation.successorSessionId != handoff.successorSession->sessionId ||
        operation.handoffId != handoff.handoffId ||
        operation.adapterId != handoff.hostState.adapterId) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "Continuity operation does not match the handoff project and session chain."));
    }
    if (!operation.successorSessionId ||
        acknowledgement.handoffId != operation.handoffId ||
        acknowledgement.handoffId != handoff.handoffId ||
        acknowledgement.successorSessionId != *operation.successorSessionId ||
        acknowledgement.adapterId != operation.adapterId ||
        acknowledgement.canonicalHandoffSha256 != handoff.contentSha256) {
        return Result<void>::failure(makeError(
            ErrorCodes::IntegrityFailure,
            "Acknowledgement does not match the exact handoff, successor, adapter, and SHA-256."));
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
