#pragma once

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::string_view ContinuityHandoffSchemaVersion = "1.0";
inline constexpr std::size_t MaximumContinuityHandoffEncodedBytes = 128 * 1024;
inline constexpr std::size_t MaximumContinuityHandoffListItems = 128;

enum class ContinuityState {
    Idle,
    CheckpointPreparing,
    CheckpointPersisted,
    SuccessorCreating,
    SuccessorCreated,
    BootstrapSending,
    Acknowledged,
    PredecessorSealing,
    Completed,
    RetryWait,
    FailedRecoverable,
    Cancelling,
    Cancelled
};

enum class LegacyContinuityState {
    Active,
    CheckpointPreparing,
    CheckpointPersisted,
    SuccessorRequested,
    SuccessorCreated,
    SuccessorBootstrapping,
    SuccessorAcknowledged,
    PredecessorSealed
};

[[nodiscard]] std::string_view wireName(ContinuityState value) noexcept;
[[nodiscard]] Result<ContinuityState> parseContinuityStateWireName(
    std::string_view value);
[[nodiscard]] bool isTerminal(ContinuityState value) noexcept;
[[nodiscard]] bool isRetryResumeState(ContinuityState value) noexcept;
[[nodiscard]] bool isAllowedContinuityTransition(
    ContinuityState from,
    ContinuityState to) noexcept;
[[nodiscard]] ContinuityState canonicalState(LegacyContinuityState value) noexcept;

struct ContinuityProject final {
    ProjectId projectId;
    std::string displayName;
    PathText repositoryRoot;
    std::string branch;
    std::string commit;
    std::vector<std::string> dirtySummary;
};

struct ContinuitySession final {
    SessionId sessionId;
    std::optional<ProviderSessionId> providerSessionId;
    std::optional<std::string> model;
    std::optional<std::string> provider;
};

struct ContinuityCurrentWork final {
    std::string phaseId;
    std::string workItemId;
    std::string summary;
    std::vector<PathText> activeFiles;
};

struct ContinuityWorkEntry final {
    std::optional<std::string> workItemId;
    std::string summary;
    std::optional<std::string> status;
};

struct ContinuityDecision final {
    std::string decision;
    std::optional<std::string> rationale;
};

struct ContinuityCommandEvidence final {
    std::string command;
    std::int32_t exitCode{};
    std::optional<EvidenceId> evidenceId;
};

struct ContinuityValidation final {
    std::vector<std::string> passedGates;
    std::vector<std::string> openGates;
    std::vector<ContinuityCommandEvidence> commands;
};

struct ContinuityNextAction final {
    std::uint32_t order{};
    std::string action;
    std::string command;
    std::string successCondition;

    ContinuityNextAction() = default;
    ContinuityNextAction(
        std::uint32_t actionOrder,
        std::string actionText,
        std::string commandText,
        std::string condition) noexcept
        : order{actionOrder},
          action{std::move(actionText)},
          command{std::move(commandText)},
          successCondition{std::move(condition)}
    {
    }

    // Schema 1.0 requires command to be present, but an empty command is
    // meaningful. Keep older call sites source-compatible while storing a
    // required string rather than an optional value.
    ContinuityNextAction(
        std::uint32_t actionOrder,
        std::string actionText,
        std::nullopt_t,
        std::string condition) noexcept
        : ContinuityNextAction{
              actionOrder,
              std::move(actionText),
              std::string{},
              std::move(condition)}
    {
    }
};

struct ContinuityRetryState final {
    std::uint32_t attempt{};
    std::optional<std::string> lastError;
    std::optional<UtcTimePoint> retryAt;
    std::optional<ContinuityState> retryResumeState;
    std::optional<std::string> persistedRetryResumeStateName;
};

struct ContinuityHostState final {
    AdapterId adapterId;
    ContinuityState continuityState{ContinuityState::Idle};
    std::string contextBudgetSource;
    ContinuityRetryState retry;
    // Imported documents retain their exact state spelling so their canonical
    // bytes and digest survive a lossless read/write cycle.
    std::optional<std::string> persistedContinuityStateName;
    std::optional<double> remainingBudgetEstimate;
};

struct ContinuityEvidenceReference final {
    std::optional<EvidenceId> evidenceId;
    std::optional<PathText> path;
};

struct ContinuityHandoff final {
    ContinuityHandoffId handoffId;
    ContinuityOperationId operationId;
    UtcTimePoint createdAt;
    ContinuityProject project;
    ContinuitySession predecessorSession;
    std::optional<ContinuitySession> successorSession;
    std::string mission;
    std::vector<std::string> constraints;
    ContinuityCurrentWork currentWork;
    std::vector<ContinuityWorkEntry> completedWork;
    std::vector<ContinuityWorkEntry> openWork;
    std::vector<ContinuityDecision> decisions;
    ContinuityValidation validation;
    std::vector<MemoryRecordId> memoryReferences;
    std::vector<ContinuityEvidenceReference> evidenceReferences;
    std::vector<ContinuityNextAction> nextActions;
    ContinuityHostState hostState;
    Sha256Digest contentSha256;
    bool redactionComplete{true};
};

struct ContinuityOperation final {
    ContinuityOperationId operationId;
    ProjectId projectId;
    SessionId predecessorSessionId;
    std::optional<SessionId> successorSessionId;
    ContinuityHandoffId handoffId;
    ContinuityState state{ContinuityState::Idle};
    std::uint32_t attempt{};
    AdapterId adapterId;
    IdempotencyKey idempotencyKey;
    std::optional<SessionId> acknowledgedSessionId;
    std::optional<ContinuityHandoffId> acknowledgedHandoffId;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
    std::optional<std::string> lastError;
    std::optional<UtcTimePoint> retryAt;
    Sha256Digest stateChecksum;
    std::optional<ContinuityState> retryResumeState;
};

enum class ContextBudgetSource {
    ProviderRemaining,
    ProviderUsage,
    ConfiguredModelEstimate,
    SerializedEstimate,
    ProviderOverflow
};

enum class ContextBudgetAction { Normal, Checkpoint, Rollover, Emergency };

struct ContextBudget final {
    std::uint64_t capacity{};
    std::uint64_t used{};
    std::uint64_t reserved{};
    std::uint64_t remaining{};
    ContextBudgetSource source{ContextBudgetSource::SerializedEstimate};
    double confidence{};
    ContextBudgetAction action{ContextBudgetAction::Normal};
};

using ContextBudgetStatus = ContextBudget;

// All available context signals for one observation. The resolver chooses the
// strongest trustworthy source in the architecture-defined order instead of
// allowing a caller to accidentally prefer a lower-confidence estimate.
struct ContextBudgetSignals final {
    std::uint64_t capacity{};
    std::uint64_t reserved{};
    std::optional<std::uint64_t> providerRemaining;
    std::optional<std::uint64_t> providerUsed;
    std::optional<std::uint64_t> configuredModelUsed;
    std::optional<std::size_t> serializedBytes;
    bool providerOverflow{};
};

[[nodiscard]] Result<ContextBudget> evaluateContextBudget(
    std::uint64_t capacity,
    std::uint64_t used,
    std::uint64_t reserved,
    ContextBudgetSource source,
    double confidence,
    double checkpointReserveFraction = 0.20,
    double rolloverReserveFraction = 0.10);

[[nodiscard]] Result<ContextBudget> estimateContextBudget(
    std::uint64_t capacity,
    std::size_t serializedBytes,
    std::uint64_t reserved);

[[nodiscard]] Result<ContextBudget> resolveContextBudget(
    const ContextBudgetSignals& signals,
    double checkpointReserveFraction = 0.20,
    double rolloverReserveFraction = 0.10);

struct HostCapabilities final {
    bool create{};
    bool bootstrap{};
    bool usageReporting{};
    bool resume{};
    bool idempotency{};
    bool queryByIdempotencyKey{};
    bool recovery{};
    bool cancellation{};
};

enum class HostSessionStatus {
    Creating,
    Active,
    Bootstrapping,
    Ready,
    Sealed,
    Failed,
    Cancelled
};

struct HostSession final {
    SessionId id;
    ProjectId projectId;
    ContinuityOperationId operationId;
    SessionId predecessorSessionId;
    IdempotencyKey idempotencyKey;
    std::optional<ProviderSessionId> providerSessionId;
    std::optional<std::string> model;
    HostSessionStatus status{HostSessionStatus::Creating};
};

struct SessionCreationRequest final {
    ContinuityOperationId operationId;
    ProjectId projectId;
    SessionId predecessorSessionId;
    IdempotencyKey idempotencyKey;
};

struct HandoffAcknowledgement final {
    ContinuityHandoffId handoffId;
    SessionId successorSessionId;
    AdapterId adapterId;
    Sha256Digest canonicalHandoffSha256;
};

struct HostPluginManifest final {
    AdapterId adapterId;
    std::string adapterVersion;
    std::uint32_t protocolVersion{1};
    PathText executable;
    HostCapabilities capabilities;
};

struct HostRecoveryRequest final {
    std::optional<ProjectId> projectId;
    std::optional<ContinuityOperationId> operationId;
    bool cancelOrphans{true};
};

struct HostRecoveryReport final {
    std::size_t inspected{};
    std::size_t recovered{};
    std::size_t cancelled{};
    std::size_t failed{};
    std::vector<HostSession> sessions;
};

struct CheckpointRequest final {
    ContinuityHandoff handoff;
    std::optional<IdempotencyKey> idempotencyKey;
};

struct CheckpointOutcome final {
    ContinuityOperation operation;
    ContinuityHandoff handoff;
};

struct RolloverRequest final {
    ProjectId projectId;
    ContinuityOperationId operationId;
};

struct RolloverOutcome final {
    ContinuityOperation operation;
    std::optional<HostSession> successor;
    bool acknowledged{};
    bool predecessorSealed{};
};

struct ContinuityStatus final {
    ProjectId projectId;
    std::optional<ContinuityOperation> activeOperation;
    std::size_t operationCount{};
    std::size_t handoffCount{};
    bool recoveryRequired{};
};

struct HandoffResumeRequest final {
    ProjectId projectId;
    ContinuityHandoffId handoffId;
    SessionId successorSessionId;
};

struct HandoffResumeOutcome final {
    ContinuityOperation operation;
    ContinuityHandoff handoff;
    HostSession session;
};

struct ContinuityRecoveryRequest final {
    std::optional<ProjectId> projectId;
    bool resumeOperations{true};
};

struct ContinuityRecoveryReport final {
    std::size_t inspected{};
    std::size_t resumed{};
    std::size_t cancelled{};
    std::size_t failed{};
    std::vector<ContinuityOperation> operations;
};

struct ContinuityResetRequest final {
    ProjectId projectId;
    DestructiveConfirmation confirmation;
};

struct ContinuityResetReport final {
    ProjectId projectId;
    ResetReport report;
};

[[nodiscard]] Result<void> validateContinuityHandoff(
    const ContinuityHandoff& handoff,
    std::size_t encodedBytes);

[[nodiscard]] Result<void> validateContinuityOperationRetryState(
    const ContinuityOperation& operation);

[[nodiscard]] Result<void> validateHostSessionBinding(
    const HostSession& session,
    const SessionCreationRequest& request);

[[nodiscard]] Result<void> validateBootstrapCompatibility(
    const HostSession& session,
    const ContinuityHandoff& handoff);

[[nodiscard]] Result<void> validateHandoffAcknowledgement(
    const ContinuityOperation& operation,
    const ContinuityHandoff& handoff,
    const HandoffAcknowledgement& acknowledgement);

} // namespace ForgeConductor::Domain
