#pragma once

#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/ManagerTelemetryModels.h"
#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ForgeConductor::Manager {

inline constexpr std::uint32_t ManagerProtocolVersion = 1U;

struct ManagerStatusRequest final {
    bool operator==(const ManagerStatusRequest&) const = default;
};

struct ManagerSettingsRequest final {
    bool operator==(const ManagerSettingsRequest&) const = default;
};

struct ManagerTelemetryRequest final {
    std::optional<Domain::SessionId> runId;
};

struct ManagerProjectsListRequest final {
    std::size_t maximumCount{100U};
};

struct ManagerProjectInitializeRequest final {
    Domain::PathText projectPath;
    std::optional<std::string> displayName;
    std::optional<std::string> repositoryIdentity;
};

struct ManagerProjectMemoryRequest final {
    Domain::ProjectId projectId;
    std::string query;
    std::size_t maximumCount{20U};
};

struct ManagerProjectRememberRequest final {
    Domain::ProjectId projectId;
    std::string title;
    std::string summary;
    std::optional<std::string> body;
    std::vector<std::string> tags;
};

struct ManagerLmStudioStatusRequest final {
    bool operator==(const ManagerLmStudioStatusRequest&) const = default;
};

struct ManagerLmStudioRepairRequest final {
    bool operator==(const ManagerLmStudioRepairRequest&) const = default;
};

struct ManagerLmStudioActivateRequest final {
    bool operator==(const ManagerLmStudioActivateRequest&) const = default;
};

struct ManagerToolsRequest final {
    bool operator==(const ManagerToolsRequest&) const = default;
};

struct ManagerToolInvokeRequest final {
    Domain::ProjectId projectId;
    std::string toolName;
    std::string canonicalArguments;
};

enum class ManagerOperationalArea { Agents, Feed, Runtimes, Diagnostics, Manager, Runs, Evidence };
enum class ManagerOperationalAction { Inspect, PruneSessions, CloseSession, VerifyTask };

struct ManagerOperationalRequest final {
    ManagerOperationalArea area{ManagerOperationalArea::Agents};
    ManagerOperationalAction action{ManagerOperationalAction::Inspect};
    std::optional<Domain::SessionId> sessionId;
    std::string summary;
    std::optional<Domain::ProjectId> projectId;
};

enum class ManagerMaintenanceScope {
    ProjectMemory,
    ProjectContinuity,
    ProjectAllData,
    AllProjectsAllData
};

struct ManagerMaintenanceRequest final {
    ManagerMaintenanceScope scope{ManagerMaintenanceScope::ProjectMemory};
    std::optional<Domain::ProjectId> projectId;
    std::string confirmationToken;
};

struct ManagerProjectMemoryRecord final {
    Domain::MemoryRecordId id;
    std::uint32_t version{};
    std::string kind;
    std::string title;
    std::string summary;
    std::optional<std::string> body;
    std::vector<std::string> tags;
    Domain::UtcTimePoint updatedAt;
};

struct ManagerProjectsSnapshot final {
    std::vector<Domain::ProjectMemoryDescriptor> projects;
};

struct ManagerProjectWorkspaceSnapshot final {
    Domain::ProjectMemoryDescriptor project;
    std::size_t recordCount{};
    std::size_t tombstoneCount{};
    std::size_t eventCount{};
    std::uint64_t databaseBytes{};
    std::uint64_t writeAheadLogBytes{};
    bool fullTextSearchAvailable{};
    bool integrityOk{};
    std::vector<ManagerProjectMemoryRecord> records;
    std::optional<std::string> nextCursor;
    bool truncated{};
    std::optional<Domain::MemoryRecordId> writtenRecordId;
};

struct ManagerLmStudioSnapshot final {
    bool lmStudioPresent{};
    bool primaryPluginInstalled{};
    bool fallbackPluginInstalled{};
    bool continuityPluginInstalled{};
    bool mcpConfigurationRegistered{};
    bool binaryExecutable{};
    std::string binaryPath;
    std::string primaryPluginPath;
    std::string fallbackPluginPath;
    std::string continuityPluginPath;
    std::string mcpConfigurationPath;
    std::optional<Domain::DeploymentId> deploymentId;
    bool connectionCheckPerformed{};
    bool primaryConnectorReady{};
    bool fallbackConnectorReady{};
    bool continuityConnectorReady{};
    bool connectedClientObserved{};
    std::size_t managedContinuityProjects{};
    std::string detail;
    std::string actionDetail;
    bool toolAuditChecked{};
    bool primaryToolOutcomeRecorded{};
    bool fallbackToolOutcomeRecorded{};
    bool continuityToolOutcomeRecorded{};
    std::string toolAuditDetail;
};

struct ManagerToolDescriptor final {
    std::string name;
    std::string description;
    std::string pack;
    Domain::ToolEffect effect{Domain::ToolEffect::Read};
    Domain::ToolAvailability availability{Domain::ToolAvailability::Available};
    bool requiresProject{};
    bool requiresShell{};
    std::string inputSchema;
};

struct ManagerToolsSnapshot final {
    bool shellEnabled{};
    std::vector<ManagerToolDescriptor> tools;
};

struct ManagerToolOutcomeSnapshot final {
    Domain::ProjectId projectId;
    std::string toolName;
    bool ok{};
    std::string canonicalPayload;
    std::optional<Domain::Error> error;
    std::chrono::milliseconds elapsed{};
};

struct ManagerOperationalSnapshot final {
    ManagerOperationalArea area{ManagerOperationalArea::Agents};
    std::string title;
    std::vector<std::string> lines;
};

struct ManagerMaintenanceSnapshot final {
    ManagerMaintenanceScope scope{ManagerMaintenanceScope::ProjectMemory};
    std::string affectedScope;
    std::size_t projectsAffected{};
    std::size_t recordsRemoved{};
    std::size_t linksRemoved{};
    std::size_t eventsRemoved{};
    bool verified{};
    std::string detail;
};

struct ManagerSettingsUpdateRequest final {
    Domain::ManagerSettingsPatch patch;
    bool applyImmediately{};
};

struct ManagerCancelRequest final {
    Domain::OperationId operationId;
};

struct ManagerShutdownRequest final {
    bool operator==(const ManagerShutdownRequest&) const = default;
};

struct ManagedRunStartRequest final {
    Domain::SessionId runId;
    Domain::ProjectId projectId;
    Domain::ClientId clientId;
    std::uint64_t authorityGeneration{};
    std::string task;
    bool allowTools{true};
};

struct ManagedRunStatusRequest final {
    Domain::SessionId runId;
};

struct ManagedRunCancelRequest final {
    Domain::SessionId runId;
};

struct ManagedRunPauseRequest final {
    Domain::SessionId runId;
};

struct ManagedRunResumeRequest final {
    Domain::SessionId runId;
};

using ManagerRequestPayload = std::variant<
    ManagerStatusRequest,
    ManagerSettingsRequest,
    ManagerTelemetryRequest,
    ManagerProjectsListRequest,
    ManagerProjectInitializeRequest,
    ManagerProjectMemoryRequest,
    ManagerProjectRememberRequest,
    ManagerLmStudioStatusRequest,
    ManagerLmStudioRepairRequest,
    ManagerLmStudioActivateRequest,
    ManagerToolsRequest,
    ManagerToolInvokeRequest,
    ManagerOperationalRequest,
    ManagerMaintenanceRequest,
    Domain::ManagerControlRequest,
    ManagerSettingsUpdateRequest,
    ManagedRunStartRequest,
    ManagedRunStatusRequest,
    ManagedRunCancelRequest,
    ManagedRunPauseRequest,
    ManagedRunResumeRequest,
    ManagerCancelRequest,
    ManagerShutdownRequest>;

struct ManagerRequest final {
    std::uint32_t version{ManagerProtocolVersion};
    Domain::RequestId requestId;
    Domain::CorrelationId correlationId;
    std::int64_t deadlineUtcMilliseconds{};
    Domain::Sha256Digest nonce;
    ManagerRequestPayload payload;
};

struct ManagerAcknowledgement final {
    bool acknowledged{true};

    bool operator==(const ManagerAcknowledgement&) const = default;
};

using ManagerResult = std::variant<
    Domain::ManagerStatus,
    Domain::ManagerSettings,
    Domain::ManagerSettingsUpdateOutcome,
    Domain::ManagedRunSnapshot,
    Domain::ManagerTelemetrySnapshot,
    ManagerProjectsSnapshot,
    ManagerProjectWorkspaceSnapshot,
    ManagerLmStudioSnapshot,
    ManagerToolsSnapshot,
    ManagerToolOutcomeSnapshot,
    ManagerOperationalSnapshot,
    ManagerMaintenanceSnapshot,
    ManagerAcknowledgement>;

using ManagerResponseBody = std::variant<ManagerResult, Domain::Error>;

struct ManagerResponse final {
    std::uint32_t version{ManagerProtocolVersion};
    Domain::RequestId requestId;
    Domain::CorrelationId correlationId;
    ManagerResponseBody body;
};

class ManagerProtocolCodec final {
public:
    // The limit applies to the JSON payload represented by the little-endian
    // prefix. The complete encoded frame is four bytes larger.
    static constexpr std::size_t DefaultMaximumFrameBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumJsonNesting = 64U;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> encodeRequest(
        const ManagerRequest& request,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;

    [[nodiscard]] static Domain::Result<ManagerRequest> decodeRequest(
        std::span<const std::byte> frame,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::vector<std::byte>> encodeResponse(
        const ManagerResponse& response,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;

    [[nodiscard]] static Domain::Result<ManagerResponse> decodeResponse(
        std::span<const std::byte> frame,
        std::size_t maximumFrameBytes = DefaultMaximumFrameBytes) noexcept;
};

} // namespace ForgeConductor::Manager
