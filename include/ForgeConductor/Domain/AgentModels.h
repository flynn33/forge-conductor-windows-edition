#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

enum class SessionStatus {
    Open,
    Active,
    Running,
    Started,
    Closed,
    Completed,
    Failed
};

[[nodiscard]] bool isOpen(SessionStatus status) noexcept;
[[nodiscard]] std::string_view wireName(SessionStatus status) noexcept;
[[nodiscard]] Result<SessionStatus> sessionStatusFromWire(
    std::string_view value) noexcept;

struct AgentSession final {
    SessionId id;
    AgentId agentId;
    std::optional<ClientId> clientId;
    SessionStatus status{SessionStatus::Open};
    std::optional<std::string> summary;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
};

struct AgentSpec final {
    AgentId id;
    std::string displayName;
    std::string description;
    std::vector<std::string> tools;
    std::vector<std::string> toolsForbidden;
    std::vector<std::string> whenToUse;
    std::vector<std::string> firstMoves;
    std::vector<std::string> doneDefinition;
    std::vector<std::string> outputSchema;
    std::vector<std::string> handoff;
    std::vector<std::string> qualityBar;
    std::string body;
    std::string source{"builtin"};
};

struct ActiveBinding final {
    SessionId sessionId;
    AgentId agentId;
    std::string goal;
    std::vector<std::string> toolsPrimary;
    std::vector<std::string> toolsForbidden;
    std::vector<std::string> outputSchema;
    std::vector<std::string> doneDefinition;
    std::optional<PathText> workingDirectory;
};

struct AgentSessionLimits final {
    static constexpr std::size_t MaximumGoalBytes = 512U;
    static constexpr std::size_t MaximumReportJsonBytes = 512U * 1024U;
    static constexpr std::size_t MaximumReportFields = 128U;
    static constexpr std::size_t MaximumSchemaItems = 128U;
    static constexpr std::size_t MaximumBindingItems = 256U;
    static constexpr std::size_t MaximumItemBytes = 512U;
    static constexpr std::size_t MaximumSummaryUnits = 4'000U;
    static constexpr std::size_t MaximumSessionQueryRows = 10'000U;
    static constexpr std::size_t MaximumMemoryBindings = 128U;
    static constexpr std::chrono::seconds DefaultIdleTtl{14'400};
    static constexpr std::chrono::seconds AbandonRiskThreshold{300};
};

// Durable run metadata is deliberately separate from AgentSession so the
// released AgentSession aggregate layout and its existing initializers remain
// source-compatible.
struct AgentRunRecord final {
    AgentSession session;
    std::optional<ProjectId> projectId;
    std::optional<std::string> goal;
    std::optional<PathText> workingDirectory;
    std::vector<std::string> outputSchema;
    std::vector<std::string> firstMoves;
    std::optional<std::string> reportJson;
};

struct AgentRunStartRequest final {
    AgentId agentId;
    std::optional<ClientId> clientId;
    std::optional<ProjectId> projectId;
    std::string goal;
    std::optional<PathText> workingDirectory;
};

// One repository transaction must supersede prior open sessions, insert the
// run, and update both legacy projections represented by this mutation.
struct AgentRunStartMutation final {
    AgentRunRecord run;
    std::optional<ActiveBinding> activeBinding;
    std::string supersedeSummary;
};

struct AgentRunStartPersistenceOutcome final {
    AgentRunRecord run;
    std::optional<ActiveBinding> activeBinding;
    std::size_t supersededSessions{};
};

struct AgentRunStartOutcome final {
    AgentRunRecord run;
    std::optional<ActiveBinding> activeBinding;
    AgentSpec agent;
    std::size_t supersededSessions{};
    bool mustComplete{true};
};

struct AgentRunStatusRequest final {
    SessionId sessionId;
    ClientId clientId;
};

struct AgentRunStatusOutcome final {
    std::optional<AgentRunRecord> run;
    bool mustComplete{};
    std::optional<std::int64_t> idleSeconds;
    bool abandonRisk{};
    bool reattached{};
    std::optional<ActiveBinding> activeBinding;
};

enum class AgentReportValueKind {
    Null,
    String,
    Array,
    Object,
    Boolean,
    Number
};

// logicalSize is the decoded string length or collection member count. Null,
// boolean, and number values are present even when logicalSize is zero.
struct AgentReportField final {
    std::string key;
    AgentReportValueKind kind{AgentReportValueKind::Null};
    std::size_t logicalSize{};
};

struct AgentCompletionReport final {
    std::string canonicalJson{"{}"};
    std::vector<AgentReportField> fields;
};

struct AgentRunCompleteRequest final {
    SessionId sessionId;
    std::optional<ClientId> clientId;
    AgentCompletionReport report;
};

struct AgentRunCompleteMutation final {
    SessionId sessionId;
    std::optional<ClientId> expectedClientId;
    std::string reportJson;
    std::string summary;
    std::vector<std::string> missingSchemaKeys;
    UtcTimePoint completedAt;
};

struct AgentRunCompletePersistenceOutcome final {
    AgentRunRecord run;
    bool activeProjectionCleared{};
};

struct AgentRunCompleteOutcome final {
    AgentRunRecord run;
    AgentCompletionReport report;
    bool schemaComplete{};
    std::vector<std::string> missingSchemaKeys;
};

struct AgentRunReattachRequest final {
    SessionId sessionId;
    ClientId clientId;
};

struct AgentRunReattachMutation final {
    SessionId sessionId;
    std::optional<ClientId> expectedClientId;
    ClientId clientId;
    ActiveBinding binding;
    std::string supersedeSummary;
    UtcTimePoint attachedAt;
};

struct AgentRunReattachOutcome final {
    AgentRunRecord run;
    ActiveBinding binding;
    std::optional<ClientId> previousClientId;
    std::size_t supersededSessions{};
    bool ownershipChanged{};
};

struct AgentProjectionRepairRequest final {
    ClientId clientId;
    AgentRunRecord run;
    ActiveBinding binding;
};

struct AgentProjectionRepairOutcome final {
    ActiveBinding binding;
    bool repaired{};
};

struct AgentRunRecoveryRequest final {
    ClientId clientId;
};

struct AgentRunRecoveryOutcome final {
    std::optional<AgentRunRecord> run;
    std::optional<ActiveBinding> binding;
    bool usedActiveProjection{};
    bool projectionNeedsRepair{};
};

struct AgentStaleCloseRequest final {
    UtcTimePoint now;
    UtcTimePoint cutoff;
    std::size_t maximumCount{AgentSessionLimits::MaximumSessionQueryRows};
};

struct AgentStaleCloseOutcome final {
    std::vector<AgentRunRecord> closedRuns;
};

[[nodiscard]] bool isMissingAgentReportField(
    const AgentReportField& field) noexcept;
[[nodiscard]] Result<void> validateAgentRunStartRequest(
    const AgentRunStartRequest& request) noexcept;
[[nodiscard]] Result<void> validateAgentCompletionReport(
    const AgentCompletionReport& report) noexcept;
[[nodiscard]] Result<void> validateAgentRunRecord(
    const AgentRunRecord& run) noexcept;
[[nodiscard]] Result<void> validateActiveBinding(
    const ActiveBinding& binding) noexcept;
[[nodiscard]] Result<void> validateAgentRunReattachRequest(
    const AgentRunReattachRequest& request) noexcept;
[[nodiscard]] Result<void> validateAgentRunRecoveryRequest(
    const AgentRunRecoveryRequest& request) noexcept;
[[nodiscard]] std::string makeAgentSupersedeSummary(
    std::string_view eventMessage,
    const std::optional<AgentId>& newAgentId,
    const std::optional<SessionId>& reattachedSessionId);
[[nodiscard]] std::string makeAgentStaleSummary(
    std::chrono::seconds age);
[[nodiscard]] Result<std::string> makeAgentCompletionSummary(
    std::string_view goal,
    const AgentCompletionReport& report,
    const std::vector<std::string>& missingSchemaKeys) noexcept;
[[nodiscard]] std::string truncateAgentSummaryUtf8(
    std::string_view value,
    std::size_t maximumUnits = AgentSessionLimits::MaximumSummaryUnits) noexcept;

struct AgentContinuitySnapshot final {
    SessionId sessionId;
    AgentId agentId;
    std::string goal;
    std::optional<PathText> workingDirectory;
    SessionStatus status{SessionStatus::Open};
    std::optional<UtcTimePoint> updatedAt;
    std::string resumeHint;
};

} // namespace ForgeConductor::Domain
