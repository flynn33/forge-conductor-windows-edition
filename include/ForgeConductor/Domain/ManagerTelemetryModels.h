#pragma once

#include "ForgeConductor/Domain/DiagnosticsModels.h"
#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/TelemetryModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumManagerTelemetryProjects = 256U;
inline constexpr std::size_t MaximumManagerTelemetryTools = 4'096U;
inline constexpr std::size_t MaximumManagerTelemetryEvents = 80U;

// Native-client resource projection of the Manager-owned telemetry sample.
// Availability metadata is retained for every primary gauge so a GUI can
// distinguish a genuine zero from warmup, stale, and unsupported states.
struct ManagerResourceSnapshot final {
    UtcTimePoint capturedAt;
    std::string host;
    std::string platform;
    std::string architecture;
    TelemetryMetric<double> cpuPercent;
    TelemetryMetric<double> ramPercent;
    TelemetryMetric<std::uint64_t> ramUsedBytes;
    TelemetryMetric<std::uint64_t> ramTotalBytes;
    TelemetryMetric<std::uint64_t> ramAvailableBytes;
    std::vector<GpuMetrics> gpus;
    std::vector<ProcessMetrics> processes;
    std::vector<HistoryPoint> history;
    TelemetryMetric<std::vector<double>> cpuPerLogicalProcessor;
    TelemetryMetric<std::uint32_t> cpuFrequencyMhz;
    TelemetryMetric<std::vector<std::uint32_t>> cpuPerLogicalFrequencyMhz;
    std::vector<DiskVolume> disks;
    TelemetryMetric<DiskIoMetrics> diskIo;
    std::uint32_t targetSampleIntervalMilliseconds{250U};
    std::optional<double> measuredSampleIntervalMilliseconds;
    std::string samplingPolicy{"realtime_cpu_ram; gpu_disk_1s; process_volume_5s"};
};

struct ManagerProviderSnapshot final {
    std::string host;
    std::uint16_t port{};
    bool secure{};
    std::optional<std::string> model;
    std::optional<ProviderSessionId> responseId;
};

// All derived values are composed inside the Manager from its settings and
// the selected ManagedRunSnapshot. Native views consume these values as-is.
struct ManagerContextSnapshot final {
    std::uint64_t capacityTokens{};
    std::uint64_t nextResponseReserveTokens{};
    std::uint64_t handoffReserveTokens{};
    std::uint64_t estimationSafetyMarginTokens{};
    std::optional<std::uint64_t> inputTokens;
    std::optional<std::uint64_t> outputTokens;
    std::optional<std::uint64_t> retainedTokens;
    std::optional<std::uint64_t> headroomTokens;
    bool authoritative{};
};

struct ManagerContinuitySnapshot final {
    bool contextOnly{true};
    bool managerOwned{};
    std::optional<SessionId> runId;
    std::optional<ProjectId> projectId;
    std::optional<ManagedRunState> runState;
    std::optional<ProviderSessionId> canonicalResponseId;
};

// One bounded, timestamped operational read model for native clients. It
// joins the resource collector with existing Manager, run, project, tool,
// event, runtime-diagnostic, and store observations without transferring
// ownership of any producer to the GUI process.
struct ManagerTelemetrySnapshot final {
    UtcTimePoint capturedAt;
    ManagerResourceSnapshot resources;
    ManagerStatus manager;
    std::optional<RuntimeDiagnosticSnapshot> runtimeDiagnostics;
    ManagerProviderSnapshot provider;
    ManagerContextSnapshot context;
    ManagerContinuitySnapshot continuity;
    std::optional<ManagedRunSnapshot> selectedRun;
    std::vector<ProjectId> projects;
    std::vector<std::string> tools;
    std::size_t openSessionCount{};
    std::size_t recentSessionCount{};
    std::size_t presenceCount{};
    std::vector<AuditEvent> recentEvents;
    TelemetryMetric<bool> storeHealthy;
    std::string runtime;
};

} // namespace ForgeConductor::Domain
