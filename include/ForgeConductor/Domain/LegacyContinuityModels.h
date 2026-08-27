#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

enum class LegacyHandoffSource {
    Model,
    Budget,
    User,
    Automatic
};

[[nodiscard]] std::string_view wireName(LegacyHandoffSource source) noexcept;
[[nodiscard]] Result<LegacyHandoffSource> legacyHandoffSourceFromWire(
    std::string_view value) noexcept;

struct LegacyContinuityLimits final {
    static constexpr std::uint32_t SchemaVersion = 1U;
    static constexpr std::size_t MaximumNarrativeCharacters = 4'000U;
    static constexpr std::size_t MaximumAgentSnapshots = 128U;
    static constexpr std::size_t MaximumListLimit = 100U;
    static constexpr std::size_t DefaultListLimit = 10U;
    static constexpr std::size_t MaximumConflictRetries = 8U;
    static constexpr std::size_t MaximumRepairRows = 10'000U;
    static constexpr std::size_t MaximumPacketBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumTextBytes = 64U * 1024U;
    static constexpr std::size_t MaximumResumeSeedBytes = 256U * 1024U;
    static constexpr std::size_t MaximumCollectionItems = 256U;
    static constexpr std::size_t MaximumItemBytes = 32U * 1024U;
};

// This is deliberately distinct from AgentContinuitySnapshot. Legacy packet
// imports preserve their source-compatible status text and do not imply that
// agent-session ownership has moved to the client reading the packet.
struct LegacyAgentContinuitySnapshot final {
    SessionId sessionId;
    AgentId agentId;
    std::string goal;
    std::optional<std::string> workingDirectory;
    std::string status{"open"};
    std::optional<UtcTimePoint> updatedAt;
    std::string resumeHint;

    bool operator==(const LegacyAgentContinuitySnapshot&) const = default;
};

struct LegacyActiveBindingSnapshot final {
    SessionId sessionId;
    std::string goal;
    std::optional<std::string> workingDirectory;

    bool operator==(const LegacyActiveBindingSnapshot&) const = default;
};

struct LegacyHandoffPacket final {
    LegacyHandoffId id;
    std::uint32_t schemaVersion{LegacyContinuityLimits::SchemaVersion};
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
    LegacyHandoffSource source{LegacyHandoffSource::Model};
    bool resumeReady{};
    std::optional<std::string> chatLabel;
    std::optional<ClientId> clientId;

    std::string goal;
    std::string status{"in_progress"};
    std::optional<std::string> projectSlug;
    std::optional<std::string> workingDirectory;
    std::vector<std::string> blockers;
    std::vector<std::string> nextActions;

    std::vector<std::string> keyFiles;
    std::vector<std::string> decisions;
    std::vector<LegacyAgentContinuitySnapshot> agents;

    std::string narrative;
    std::string resumeSeed;
    bool resumeSeedIsCustom{};

    bool operator==(const LegacyHandoffPacket&) const = default;
};

// Raw document members are opaque compatibility evidence. Domain and
// Application never parse them. Persistence validates/produces them through
// its injected continuity document codec and retains packetJson for imported
// macOS rows while preferring payloadJson on reads when it is present.
struct LegacyContinuityDocuments final {
    std::optional<std::string> packetJson;
    std::optional<std::string> payloadJson;
    std::optional<Sha256Digest> contentSha256;

    bool operator==(const LegacyContinuityDocuments&) const = default;
};

struct LegacyContinuityRecord final {
    LegacyHandoffPacket packet;
    std::uint64_t writeSequence{};
    LegacyContinuityDocuments documents;

    bool operator==(const LegacyContinuityRecord&) const = default;
};

// Every optional member represents a caller-explicit field. A CAS retry
// reloads the newest durable record and reapplies only these members so
// disjoint concurrent edits survive.
struct LegacyContinuityPatch final {
    std::optional<std::string> goal;
    std::optional<std::string> status;
    std::optional<std::string> projectSlug;
    std::optional<std::string> workingDirectory;
    std::optional<std::string> chatLabel;
    std::optional<std::string> narrative;
    std::optional<std::string> resumeSeed;
    std::optional<std::vector<std::string>> blockers;
    std::optional<std::vector<std::string>> nextActions;
    std::optional<std::vector<std::string>> keyFiles;
    std::optional<std::vector<std::string>> decisions;

    bool operator==(const LegacyContinuityPatch&) const = default;
};

struct LegacyContinuityWriteRequest final {
    std::optional<LegacyHandoffId> handoffId;
    LegacyContinuityPatch patch;
};

struct LegacyContinuityAutomaticRequest final {
    LegacyContinuityPatch inferred;
    std::string reason;
    bool finalize{};
};

struct LegacyContinuityGetRequest final {
    std::optional<LegacyHandoffId> handoffId;
    bool preferResumeReady{};
};

struct LegacyContinuityListRequest final {
    std::int64_t requestedLimit{
        static_cast<std::int64_t>(LegacyContinuityLimits::DefaultListLimit)};
};

struct LegacyContinuityListItem final {
    LegacyHandoffId id;
    UtcTimePoint updatedAt;
    LegacyHandoffSource source{LegacyHandoffSource::Model};
    bool resumeReady{};
    std::string goal;
    std::string status;
    std::size_t agentCount{};
    std::uint64_t writeSequence{};

    bool operator==(const LegacyContinuityListItem&) const = default;
};

struct LegacyContinuityProjectionReceipt final {
    std::optional<std::string> packetPath;
    std::optional<std::string> currentTaskPath;

    bool operator==(const LegacyContinuityProjectionReceipt&) const = default;
};

struct LegacyContinuityPersistOutcome final {
    LegacyContinuityRecord record;
    bool handoffRequired{};
    bool projectionOk{true};
    bool projectionRepairPending{};
    std::optional<Error> projectionWarning;
    LegacyContinuityProjectionReceipt projection;
};

struct LegacyContinuityGetOutcome final {
    std::optional<LegacyContinuityRecord> record;
    bool explicitIdRequested{};
};

struct LegacyContinuityListOutcome final {
    std::vector<LegacyContinuityListItem> handoffs;
};

struct LegacyContinuityAutomaticStatus final {
    std::uint32_t checkpointEveryTools{50U};
    std::uint32_t handoffEveryTools{200U};
    std::string note{
        "Forge writes checkpoints and handoffs from tool progress; the model does not have to call session_*."};

    bool operator==(const LegacyContinuityAutomaticStatus&) const = default;
};

// Typed projection of ContextContinuityService.statusSummary().
struct LegacyContinuityStatusSummary final {
    std::optional<LegacyHandoffId> latestId;
    std::optional<UtcTimePoint> latestUpdatedAt;
    bool resumeReady{};
    std::optional<LegacyHandoffId> resumeId;
    std::size_t openAgentSessions{};
    std::vector<std::string> tools{
        "session_checkpoint",
        "session_handoff",
        "context_get",
        "context_list"};
    std::string note{
        "New chat bootstrap: call context_get over stdio MCP (forge-conductor)."};
    LegacyContinuityAutomaticStatus automatic;

    bool operator==(const LegacyContinuityStatusSummary&) const = default;
};

struct LegacyContinuityPointerRepairOutcome final {
    std::optional<LegacyHandoffId> latestId;
    std::optional<LegacyHandoffId> resumeReadyId;
    std::size_t pointerRowsChanged{};
};

struct LegacyContinuityProjectionRepairOutcome final {
    std::size_t packetFilesWritten{};
    bool latestWritten{};
    bool currentTaskWritten{};
};

struct LegacyContinuityResetOutcome final {
    std::size_t handoffsRemoved{};
    std::size_t pointerNotesRemoved{};
    std::size_t projectionFilesRemoved{};
    bool authoritativeScopeCommitted{};
    bool projectionScopeCommitted{};
    bool verified{};
    std::optional<Error> projectionWarning;
};

struct LegacyContinuityCompareExchange final {
    LegacyHandoffPacket packet;
    std::optional<std::uint64_t> expectedWriteSequence;
};

[[nodiscard]] std::size_t normalizeLegacyContinuityListLimit(
    std::int64_t requested) noexcept;

[[nodiscard]] Result<void> validateLegacyContinuityPatch(
    const LegacyContinuityPatch& patch) noexcept;
[[nodiscard]] Result<void> validateLegacyAgentContinuitySnapshot(
    const LegacyAgentContinuitySnapshot& snapshot) noexcept;
[[nodiscard]] Result<void> validateLegacyActiveBindingSnapshot(
    const LegacyActiveBindingSnapshot& binding) noexcept;
[[nodiscard]] Result<void> validateLegacyHandoffPacket(
    const LegacyHandoffPacket& packet) noexcept;
[[nodiscard]] Result<void> validateLegacyContinuityRecord(
    const LegacyContinuityRecord& record) noexcept;
[[nodiscard]] Result<void> validateLegacyContinuityList(
    const std::vector<LegacyContinuityRecord>& records,
    std::size_t maximumCount) noexcept;

[[nodiscard]] Result<std::string> truncateLegacyNarrative(
    std::string_view narrative) noexcept;
[[nodiscard]] Result<std::string> makeLegacyDefaultResumeSeed(
    const LegacyHandoffPacket& packet) noexcept;

} // namespace ForgeConductor::Domain
