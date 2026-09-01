#pragma once

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t TelemetryPendingSnapshotsMaximum = 1;
inline constexpr std::size_t TelemetryMetricSourceBytesMaximum = 128U;
inline constexpr std::size_t TelemetryMetricReasonBytesMaximum = 256U;
inline constexpr std::size_t CpuBrandBytesMaximum = 256U;

enum class TelemetryHealth { Ok, Warn, Error, Down, Config };
enum class TelemetryStatusTone { Healthy, Caution, Failure, Informational, Unavailable };
enum class TelemetryMetricAvailability {
    Available,
    WarmingUp,
    Unsupported,
    TemporarilyUnavailable,
    AccessDenied
};

template <typename T>
struct TelemetryMetric final {
    std::optional<T> value;
    TelemetryMetricAvailability availability{TelemetryMetricAvailability::WarmingUp};
    bool stale{};
    std::optional<UtcTimePoint> capturedAt;
    std::optional<UtcTimePoint> observedAt;
    std::string source{"not_collected"};
    std::optional<std::string> unavailableReason{
        std::string{"No sample has been collected."}};
};

template <typename T>
[[nodiscard]] TelemetryMetric<T> makeAvailableTelemetryMetric(
    T value,
    const UtcTimePoint capturedAt,
    std::string source)
{
    return TelemetryMetric<T>{
        std::move(value),
        TelemetryMetricAvailability::Available,
        false,
        capturedAt,
        capturedAt,
        std::move(source),
        std::nullopt};
}

template <typename T>
[[nodiscard]] TelemetryMetric<T> makeUnavailableTelemetryMetric(
    const TelemetryMetricAvailability availability,
    const UtcTimePoint observedAt,
    std::string source,
    std::string reason)
{
    return TelemetryMetric<T>{
        std::nullopt,
        availability,
        false,
        std::nullopt,
        observedAt,
        std::move(source),
        std::move(reason)};
}

template <typename T>
[[nodiscard]] TelemetryMetric<T> makeStaleTelemetryMetric(
    const TelemetryMetric<T>& previous,
    const TelemetryMetricAvailability availability,
    const UtcTimePoint observedAt,
    std::string reason)
{
    return TelemetryMetric<T>{
        previous.value,
        availability,
        true,
        previous.capturedAt,
        observedAt,
        previous.source,
        std::move(reason)};
}

struct LoadAverage final { double oneMinute{}; double fiveMinutes{}; double fifteenMinutes{}; };

struct CpuMetrics final {
    TelemetryMetric<double> percent;
    TelemetryMetric<std::vector<double>> perLogicalProcessor;
    TelemetryMetric<std::uint32_t> logicalProcessorCount;
    TelemetryMetric<std::uint32_t> physicalCoreCount;
    TelemetryMetric<std::uint32_t> frequencyMhz;
    TelemetryMetric<std::vector<std::uint32_t>> perCoreFrequencyMhz;
    TelemetryMetric<LoadAverage> loadAverage;
    TelemetryMetric<std::string> brand;
    TelemetryMetric<double> userPercent;
    TelemetryMetric<double> systemPercent;
    TelemetryMetric<double> idlePercent;
};

using CPUMetrics = CpuMetrics;

struct RamMetrics final {
    TelemetryMetric<std::uint64_t> totalBytes;
    TelemetryMetric<std::uint64_t> usedBytes;
    TelemetryMetric<std::uint64_t> availableBytes;
    TelemetryMetric<double> percent;
    TelemetryMetric<double> pressurePercent;
    TelemetryMetric<std::uint64_t> activeBytes;
    TelemetryMetric<std::uint64_t> wiredBytes;
    TelemetryMetric<std::uint64_t> compressedBytes;
    TelemetryMetric<std::uint64_t> swapTotalBytes;
    TelemetryMetric<std::uint64_t> swapUsedBytes;
    TelemetryMetric<double> swapPercent;
    TelemetryMetric<std::uint64_t> committedBytes;
    TelemetryMetric<std::uint64_t> pagedPoolBytes;
};

using RAMMetrics = RamMetrics;

struct DiskVolume final {
    std::string device;
    PathText mount;
    std::string fileSystem;
    std::uint64_t totalBytes{};
    std::uint64_t usedBytes{};
    std::uint64_t availableBytes{};
    double percent{};
};

struct DiskIoMetrics final {
    double readBytesPerSecond{};
    double writeBytesPerSecond{};
    double readOperationsPerSecond{};
    double writeOperationsPerSecond{};
};

using DiskIOMetrics = DiskIoMetrics;

struct GpuMetrics final {
    std::string vendor;
    std::string name;
    std::optional<double> utilizationPercent;
    std::optional<std::uint64_t> dedicatedBytesUsed;
    std::optional<std::uint64_t> dedicatedBytesTotal;
    std::optional<std::uint64_t> sharedBytesUsed;
    bool direct3dAvailable{};
};

using GPUMetrics = GpuMetrics;

struct ProcessMetrics final {
    std::uint32_t processId{};
    std::string name;
    double cpuPercent{};
    std::uint64_t workingSetBytes{};
    std::uint64_t privateBytes{};
    std::uint32_t threadCount{};
    std::uint32_t handleCount{};
    std::string source;
};

struct PowerMetrics final {
    bool onAcPower{true};
    std::string state{"unknown"};
    std::optional<double> batteryPercent;
    std::optional<bool> charging;
    std::optional<std::chrono::minutes> timeRemaining;
};

struct SystemMetrics final {
    UtcTimePoint timestamp;
    std::string host;
    std::string platform;
    std::string architecture;
    CpuMetrics cpu;
    RamMetrics ram;
    std::vector<DiskVolume> disks;
    DiskIoMetrics diskIo;
    std::vector<GpuMetrics> gpus;
    std::vector<ProcessMetrics> processes;
    PowerMetrics power;
};

struct ForgeSnapshot final {
    UtcTimePoint timestamp;
    PathText home;
    std::string runtime;
    std::size_t presenceCount{};
    std::size_t mcpServerCount{};
    std::vector<ToolDescriptor> tools;
    std::vector<AgentSession> agentSessions;
    std::size_t auditEventCount{};
    TelemetryHealth orchestrationHealth{TelemetryHealth::Config};
};

struct HistoryPoint final {
    UtcTimePoint timestamp;
    double cpuPercent{};
    double ramPercent{};
    std::optional<double> gpuPercent;
    double diskBytesPerSecond{};
    std::size_t mcpEvents{};
    TelemetryHealth orchestrationHealth{TelemetryHealth::Config};
};

struct TelemetrySnapshot final {
    SystemMetrics system;
    ForgeSnapshot forge;
    UtcTimePoint updatedAt;
    std::vector<HistoryPoint> history;
    std::string runtime;
};

using LiveTelemetryFrame = TelemetrySnapshot;

struct TelemetryHealthReport final {
    bool ok{};
    std::string service;
    std::string runtime;
    bool interferesWithMcp{};
    std::string mode;
    std::string collectors;
    std::string ui;
    bool nodeRequired{};
};

[[nodiscard]] TelemetryStatusTone toneFor(TelemetryHealth health) noexcept;
[[nodiscard]] TelemetryStatusTone mostSevere(
    const std::vector<TelemetryStatusTone>& tones) noexcept;
[[nodiscard]] std::string_view telemetryMetricAvailabilityName(
    TelemetryMetricAvailability availability) noexcept;
[[nodiscard]] Result<void> validateTelemetryMetricState(
    bool hasValue,
    const std::optional<UtcTimePoint>& capturedAt,
    const std::optional<UtcTimePoint>& observedAt,
    TelemetryMetricAvailability availability,
    bool stale,
    std::string_view source,
    const std::optional<std::string>& unavailableReason);

template <typename T>
[[nodiscard]] Result<void> validateTelemetryMetric(
    const TelemetryMetric<T>& metric)
{
    return validateTelemetryMetricState(
        metric.value.has_value(),
        metric.capturedAt,
        metric.observedAt,
        metric.availability,
        metric.stale,
        metric.source,
        metric.unavailableReason);
}

[[nodiscard]] Result<void> validateRamMetrics(const RamMetrics& metrics);
[[nodiscard]] Result<void> validateCpuMetrics(const CpuMetrics& metrics);
[[nodiscard]] Result<void> validateTelemetrySnapshot(
    const TelemetrySnapshot& snapshot,
    const ResourceBudgets& budgets);

} // namespace ForgeConductor::Domain
