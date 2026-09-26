#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

struct ProviderBinding final {
    ProjectId projectId;
    std::string providerId;
    std::string endpoint;
    std::string model;
    bool connected{};
    bool automaticContinuityCapable{};
};

struct ProjectRemoteBinding final {
    ProjectId projectId;
    std::string repositoryUrl;
    std::optional<std::string> detectedRemoteName;
};

enum class PackageEntryKind { File, Directory, ReparsePoint, Other };
enum class PackageInterpretationState {
    Interpreted,
    PartiallyInterpreted,
    Opaque,
    Encrypted,
    Damaged,
    Unavailable,
    ChangedDuringRead
};
enum class PackageQueueState {
    Ingesting,
    Ready,
    Active,
    Paused,
    NeedsAttention,
    Completed,
    Removed
};

struct InstructionPackageEntry final {
    InstructionPackageId packageId;
    Sha256Digest revision;
    std::string relativePath;
    PackageEntryKind kind{PackageEntryKind::Other};
    std::uint64_t byteLength{};
    std::optional<Sha256Digest> contentHash;
    PackageInterpretationState interpretation{PackageInterpretationState::Opaque};
    std::optional<std::string> derivedResource;
    std::optional<std::string> coverageDetail;
};

struct PackageExecutionCursor final {
    std::uint64_t entryIndex{};
    std::uint64_t byteOffset{};
    std::optional<std::string> providerCursor;
};

struct InstructionPackageQueueRow final {
    ProjectId projectId;
    PackageQueueRowId queueRowId;
    InstructionPackageId packageId;
    Sha256Digest revision;
    PathText sourceRoot;
    std::uint64_t order{};
    PackageQueueState state{PackageQueueState::Ingesting};
    PackageExecutionCursor cursor;
    std::uint64_t entryCount{};
    std::uint64_t totalBytes{};
    std::uint64_t coverageGapCount{};
    std::uint32_t attempts{};
    std::optional<std::string> lastError;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
};

enum class PolicySourceKind { LocalFolder, RemoteRepository };
enum class PolicyCoverageState {
    Interpreted,
    PartiallyInterpreted,
    Opaque,
    Encrypted,
    Damaged,
    Unavailable,
    ChangedDuringRead,
    ExplicitlyIgnored
};

struct PolicyBinding final {
    ProjectId projectId;
    PolicySourceKind sourceKind{PolicySourceKind::LocalFolder};
    std::string source;
    PolicyRevisionId revision;
    Sha256Digest contentDigest;
    std::uint64_t entryCount{};
    std::uint64_t coverageGapCount{};
    UtcTimePoint boundAt;
};

enum class CluFindingSeverity { Information, Warning, Error, Critical };
enum class CluFindingState { Open, CorrectionRequested, Resolved, OwnerOverride };

struct CluFinding final {
    CluFindingId findingId;
    ProjectId projectId;
    PolicyRevisionId policyRevision;
    std::string ruleId;
    std::string ruleTitle;
    std::optional<std::string> sourceLocation;
    CluFindingSeverity severity{CluFindingSeverity::Warning};
    CluFindingState state{CluFindingState::Open};
    std::string summary;
    std::vector<std::string> evidenceReferences;
    std::string requestedCorrection;
    std::vector<std::string> correctionEvidence;
    double confidence{1.0};
    UtcTimePoint firstObservedAt;
    UtcTimePoint lastObservedAt;
};

struct CluNotificationReceipt final {
    CluNotificationId notificationId;
    CluFindingId findingId;
    ProjectId projectId;
    std::string recipient;
    UtcTimePoint deliveredAt;
    bool acknowledged{};
};

enum class AutomaticContinuityState {
    Off,
    Preparing,
    Active,
    Recovering,
    NeedsAttention
};

struct AutomaticContinuityPreference final {
    ProjectId projectId;
    std::string providerId;
    bool enabled{};
    AutomaticContinuityState state{AutomaticContinuityState::Off};
    std::optional<std::string> detail;
};

struct ContinuityRecordMetadata final {
    ContinuityOperationId operationId;
    ProjectId projectId;
    std::optional<ProviderSessionId> predecessor;
    std::optional<ProviderSessionId> successor;
    std::optional<PackageQueueRowId> queueRowId;
    PackageExecutionCursor packageCursor;
    std::string state;
    bool sealed{};
    bool authoritative{};
    bool deletionEligible{};
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
};

} // namespace ForgeConductor::Domain
