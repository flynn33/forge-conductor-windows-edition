#pragma once

#include "ForgeConductor/Domain/TelemetryModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumDiagnosticEventBytes = 256U;
inline constexpr std::size_t MaximumDiagnosticRoleBytes = 64U;
inline constexpr std::size_t MaximumDiagnosticFieldCount = 64U;
inline constexpr std::size_t MaximumDiagnosticFieldNameBytes = 128U;
inline constexpr std::size_t MaximumDiagnosticFieldValueBytes = 4U * 1024U;
inline constexpr std::size_t MaximumDiagnosticFlattenedFieldBytes = 512U;
inline constexpr std::size_t MaximumDiagnosticRingRecords = 4'000U;

enum class DiagnosticSeverity { Info, Warn, Error, Critical };
enum class DiagnosticCategory {
    General, Bootstrap, Telemetry, Mcp, LmStudio, Manager, Tools, Agent, Diagnostics, Ui
};

struct DiagnosticField final { std::string name; std::string value; };

struct DiagnosticEnvelope final {
    UtcTimePoint timestamp;
    std::string event;
    DiagnosticSeverity severity{DiagnosticSeverity::Info};
    std::string role{"primary"};
    std::uint32_t processId{};
    DiagnosticCategory category{DiagnosticCategory::General};
    std::vector<DiagnosticField> fields;
};

struct DiagnosticExportRequest final {
    std::optional<PathText> directory;
    std::optional<std::string> basename;
};

struct DiagnosticExportResult final {
    PathText artifact;
    std::size_t recordCount{};
    std::size_t encodedBytes{};
    Sha256Digest checksum;
    std::optional<PathText> markdownArtifact;
    std::optional<Sha256Digest> markdownChecksum;
};

enum class AuditOutcome { Success, OperationalError, PolicyDenied, MaintenanceWarning, Other };

struct AuditOutcomeCounts final {
    std::size_t errorCount{};
    std::size_t deniedCount{};
    std::size_t warningCount{};
    std::size_t otherCount{};
};

struct AuditEvent final {
    UtcTimePoint timestamp;
    std::optional<ClientId> clientId;
    std::string tool;
    std::optional<Sha256Digest> argumentsDigest;
    std::string status;
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::string> error;
};

struct DoctorCheck final {
    std::string name;
    bool ok{};
    std::string detail;
    bool hard{true};
};

struct DoctorReport final {
    bool ok{};
    std::string version;
    PathText home;
    std::vector<DoctorCheck> checks;
    TelemetryHealthReport telemetry;
    bool binaryInstalled{};
    PathText binaryPath;
};

struct RuntimeDiagnosticSnapshot final {
    UtcTimePoint timestamp;
    std::size_t ownedOperations{};
    std::size_t pendingCallbacks{};
    std::size_t backgroundThreads{};
    std::size_t openRepositories{};
    std::size_t telemetryPendingSnapshots{};
    ResourcePressureLevel pressure{ResourcePressureLevel::Nominal};
    std::size_t activeTimers{};
    std::size_t childProcesses{};
    std::size_t processReaders{};
    std::size_t openDatabases{};
};

[[nodiscard]] AuditOutcome auditOutcomeFor(std::string_view status) noexcept;
[[nodiscard]] AuditOutcomeCounts summarizeAuditOutcomes(
    const std::vector<std::string>& statuses) noexcept;
[[nodiscard]] Result<void> validateDiagnosticEnvelope(
    const DiagnosticEnvelope& envelope);

} // namespace ForgeConductor::Domain
