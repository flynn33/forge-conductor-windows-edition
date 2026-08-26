#pragma once
#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

struct ProjectMemoryLimits final {
    std::size_t maximumTitleBytes{512};
    std::size_t maximumSummaryBytes{4 * 1024};
    std::size_t maximumBodyBytes{256 * 1024};
    std::size_t maximumSourceReferenceBytes{2 * 1024};
    std::size_t maximumTagCount{32};
    std::size_t maximumTagBytes{128};
    std::size_t maximumRelatedIdCount{32};
    std::size_t maximumBatchCount{50};
    std::size_t maximumBatchBytes{1024 * 1024};
    std::size_t maximumQueryBytes{4 * 1024};
    std::size_t maximumPageCount{100};
    std::size_t defaultPageCount{20};
    std::size_t maximumResponseBytes{256 * 1024};
    std::size_t defaultResponseBytes{64 * 1024};
    std::size_t maximumOpenProjects{8};
    std::size_t maximumArtifactRecords{10'000};
    std::size_t maximumArtifactBytes{32 * 1024 * 1024};

    bool operator==(const ProjectMemoryLimits&) const = default;
};

inline constexpr std::uint32_t ProjectMemoryCapabilityVersion = 1;
inline constexpr std::uint32_t ProjectMemorySchemaVersion = 1;
inline constexpr std::chrono::milliseconds MinimumProjectMemoryDeadline{1};
inline constexpr std::chrono::milliseconds MaximumProjectMemoryDeadline{60'000};

[[nodiscard]] ProjectMemoryLimits projectMemoryLimitsForProfile(
    ResourceProfile profile) noexcept;

enum class MemoryWriteDisposition { Inserted, Deduplicated, Updated };
enum class ForgetDisposition { Tombstoned, NotFound };
enum class LinkDisposition { Inserted, Deduplicated };
enum class ImportDisposition { Preview, Imported };

[[nodiscard]] std::string_view wireName(MemoryWriteDisposition value) noexcept;
[[nodiscard]] std::string_view wireName(ForgetDisposition value) noexcept;
[[nodiscard]] std::string_view wireName(LinkDisposition value) noexcept;
[[nodiscard]] std::string_view wireName(ImportDisposition value) noexcept;

struct ProjectMemoryDescriptor final {
    ProjectId id;
    std::string displayName;
    std::optional<std::string> repositoryIdentity;
    std::vector<PathText> aliases;
};

struct ProjectMemoryRecord final {
    MemoryRecordId id;
    ProjectId projectId;
    std::uint32_t version{};
    std::string kind;
    std::string title;
    std::string summary;
    std::optional<std::string> body;
    std::vector<std::string> tags;
    double importance{0.5};
    double confidence{1.0};
    std::string sourceKind;
    std::optional<std::string> sourceReference;
    std::optional<SessionId> sessionId;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
    UtcTimePoint lastAccessedAt;
    std::optional<UtcTimePoint> expiresAt;
    Sha256Digest contentHash;
    bool isTombstone{};
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
};

struct ProjectMemoryWrite final {
    std::string kind;
    std::string title;
    std::string summary;
    std::optional<std::string> body;
    std::vector<std::string> tags;
    double importance{0.5};
    double confidence{1.0};
    std::string sourceKind{"external_integration"};
    std::optional<std::string> sourceReference;
    std::optional<SessionId> sessionId;
    std::optional<UtcTimePoint> expiresAt;
    std::vector<MemoryRecordId> relatedIds;
    std::optional<IdempotencyKey> idempotencyKey;
};

struct InitializeProjectRequest final {
    PathText projectPath;
    std::optional<ProjectId> requestedProjectId;
    std::optional<std::string> displayName;
    std::optional<std::string> repositoryIdentity;
    std::optional<IdempotencyKey> idempotencyKey;
};

struct ProjectInitialization final {
    ProjectMemoryDescriptor project;
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
    std::uint32_t capabilityVersion{ProjectMemoryCapabilityVersion};
    ProjectMemoryLimits limits;
    bool lexicalSearchAvailable{true};
    bool fullTextSearchAvailable{};
    bool migrationCurrent{true};
};

struct RememberProjectMemoryRequest final {
    ProjectId projectId;
    ProjectMemoryWrite write;
};

struct MemoryWriteOutcome final {
    ProjectId projectId;
    MemoryRecordId recordId;
    std::uint32_t recordVersion{};
    MemoryWriteDisposition disposition{MemoryWriteDisposition::Inserted};
    Sha256Digest contentHash;
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
    std::uint32_t capabilityVersion{ProjectMemoryCapabilityVersion};
};

struct RememberProjectMemoryBatchRequest final {
    ProjectId projectId;
    std::vector<ProjectMemoryWrite> writes;
};

struct MemoryBatchOutcome final {
    ProjectId projectId;
    std::vector<MemoryWriteOutcome> results;
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
    std::uint32_t capabilityVersion{ProjectMemoryCapabilityVersion};
};

struct SearchProjectMemoryRequest final {
    ProjectId projectId;
    std::string query;
    std::vector<std::string> kinds;
    std::vector<std::string> tags;
    std::optional<SessionId> sessionId;
    std::size_t limit{20};
    std::optional<std::string> cursor;
    bool includeBody{};
    std::size_t maximumResponseBytes{64 * 1024};
};

struct MemorySearchHit final {
    ProjectMemoryRecord record;
    double score{};
};

struct MemoryPage final {
    ProjectId projectId;
    std::vector<MemorySearchHit> records;
    std::optional<std::string> nextCursor;
    bool truncated{};
    std::size_t encodedBytes{};
    std::size_t maximumResponseBytes{};
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
    std::uint32_t capabilityVersion{ProjectMemoryCapabilityVersion};
};

struct GetProjectMemoryRequest final {
    ProjectId projectId;
    std::vector<MemoryRecordId> ids;
    bool includeBody{};
    // This is an application-internal response budget. The P14 wire schema
    // retains the source-compatible fields and supplies this default.
    std::size_t maximumResponseBytes{64 * 1024};
};

struct MemoryRecords final {
    ProjectId projectId;
    std::vector<ProjectMemoryRecord> records;
    std::size_t encodedBytes{};
    std::size_t maximumResponseBytes{};
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
    std::uint32_t capabilityVersion{ProjectMemoryCapabilityVersion};
};

struct UpdateProjectMemoryRequest final {
    ProjectId projectId;
    MemoryRecordId recordId;
    std::uint32_t expectedVersion{};
    std::optional<std::string> title;
    std::optional<std::string> summary;
    std::optional<std::string> body;
    std::optional<std::vector<std::string>> tags;
};

struct ForgetProjectMemoryRequest final {
    ProjectId projectId;
    MemoryRecordId recordId;
};

struct ForgetOutcome final {
    ProjectId projectId;
    MemoryRecordId recordId;
    ForgetDisposition disposition{ForgetDisposition::NotFound};
};

struct ListRecentProjectMemoryRequest final {
    ProjectId projectId;
    std::vector<std::string> kinds;
    std::optional<SessionId> sessionId;
    std::size_t limit{20};
    std::optional<std::string> cursor;
    bool includeBody{};
    std::size_t maximumResponseBytes{64 * 1024};
};

struct LinkProjectMemoryRequest final {
    ProjectId projectId;
    MemoryRecordId sourceId;
    MemoryRecordId targetId;
    std::string relation;
};

struct LinkOutcome final {
    ProjectId projectId;
    LinkDisposition disposition{LinkDisposition::Inserted};
};

struct ExportProjectMemoryRequest final {
    ProjectId projectId;
};

struct ProjectMemoryExport final {
    ProjectId projectId;
    PathText artifact;
    Sha256Digest checksum;
    std::size_t recordCount{};
};

// Owns the exact, bounded bytes read from one retained project-memory artifact.
// Parsing remains a repository concern so preview and committed import share the
// same hostile-input validation path.
struct ProjectMemoryArtifactDocument final {
    PathText artifact;
    std::vector<std::byte> content;
};

struct ImportProjectMemoryRequest final {
    ProjectId projectId;
    PathText artifact;
    bool preview{true};
    bool allowCrossProjectMerge{};
};

struct ProjectMemoryImport final {
    ProjectId projectId;
    ImportDisposition disposition{ImportDisposition::Preview};
    std::size_t recordCount{};
    std::size_t importableCount{};
    Sha256Digest checksum;
    std::vector<MemoryWriteOutcome> imported;
};

struct ProjectMemoryStatusRequest final {
    ProjectId projectId;
};

struct ProjectMemoryStatus final {
    ProjectId projectId;
    std::uint32_t schemaVersion{ProjectMemorySchemaVersion};
    std::uint32_t capabilityVersion{ProjectMemoryCapabilityVersion};
    std::size_t recordCount{};
    std::size_t tombstoneCount{};
    std::size_t eventCount{};
    std::uint64_t databaseBytes{};
    std::uint64_t writeAheadLogBytes{};
    bool fullTextSearchAvailable{};
    bool integrityOk{};
    std::size_t openRepositories{};
    ProjectMemoryLimits limits;
};

struct DestructiveConfirmation final {
    std::string action;
    std::string scope;
    std::string token;
};

struct ResetReport final {
    std::string action;
    std::string scope;
    std::size_t projectsAffected{};
    std::size_t recordsRemoved{};
    std::size_t linksRemoved{};
    std::size_t eventsRemoved{};
    bool verified{};
};

[[nodiscard]] Result<ProjectMemoryWrite> validateProjectMemoryWrite(
    ProjectMemoryWrite write,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<std::vector<ProjectMemoryWrite>> validateProjectMemoryBatch(
    std::vector<ProjectMemoryWrite> writes,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<std::vector<std::string>> normalizeProjectMemoryTags(
    std::vector<std::string> tags,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<void> validateProjectMemoryQuery(
    std::string_view query,
    const ProjectMemoryLimits& limits);

// Windows caps aggregate kind-filter count at maximumPageCount. Each value
// follows the 1...64-byte ASCII identifier rule used for stored memory kinds.
[[nodiscard]] Result<SearchProjectMemoryRequest> validateSearchProjectMemoryRequest(
    SearchProjectMemoryRequest request,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<void> validateGetProjectMemoryRequest(
    const GetProjectMemoryRequest& request,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<UpdateProjectMemoryRequest> validateUpdateProjectMemoryRequest(
    UpdateProjectMemoryRequest request,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<ListRecentProjectMemoryRequest>
validateListRecentProjectMemoryRequest(
    ListRecentProjectMemoryRequest request,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<LinkProjectMemoryRequest> validateLinkProjectMemoryRequest(
    LinkProjectMemoryRequest request);

// Artifact paths do not reveal encoded size. Callers supply the observed byte
// count after bounded metadata/read inspection; no filesystem access occurs in Domain.
[[nodiscard]] Result<void> validateExportProjectMemoryRequest(
    const ExportProjectMemoryRequest& request,
    std::size_t observedArtifactBytes,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<void> validateImportProjectMemoryRequest(
    const ImportProjectMemoryRequest& request,
    std::size_t observedArtifactBytes,
    const ProjectMemoryLimits& limits);

[[nodiscard]] Result<void> validateProjectMemoryStatusRequest(
    const ProjectMemoryStatusRequest& request) noexcept;

[[nodiscard]] Result<void> validateProjectMemoryDeadline(
    std::chrono::milliseconds deadline);

[[nodiscard]] Result<void> validateDestructiveConfirmation(
    const DestructiveConfirmation& confirmation,
    std::string_view expectedAction,
    std::string_view expectedScope,
    std::string_view expectedToken);

[[nodiscard]] std::size_t normalizeProjectMemoryPageLimit(
    std::size_t requested,
    const ProjectMemoryLimits& limits) noexcept;

[[nodiscard]] std::size_t normalizeProjectMemoryResponseLimit(
    std::size_t requested,
    const ProjectMemoryLimits& limits) noexcept;

} // namespace ForgeConductor::Domain
