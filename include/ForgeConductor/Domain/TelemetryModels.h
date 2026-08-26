#pragma once

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t TelemetryPendingSnapshotsMaximum = 1;

enum class TelemetryHealth { Ok, Warn, Error, Down, Config };
enum class TelemetryStatusTone { Healthy, Caution, Failure, Informational, Unavailable };

struct LoadAverage final { double oneMinute{}; double fiveMinutes{}; double fifteenMinutes{}; };

struct CpuMetrics final {
    double percent{};
    std::vector<double> perLogicalProcessor;
    std::uint32_t logicalProcessorCount{};
    std::uint32_t physicalCoreCount{};
    std::optional<std::uint32_t> frequencyMhz;
    std::vector<std::uint32_t> perCoreFrequencyMhz;
    LoadAverage loadAverage;
    std::string brand;
    double userPercent{};
    double systemPercent{};
    double idlePercent{};
};

using CPUMetrics = CpuMetrics;

struct RamMetrics final {
    std::uint64_t totalBytes{};
    std::uint64_t usedBytes{};
    std::uint64_t availableBytes{};
    double percent{};
    double pressurePercent{};
    std::uint64_t committedBytes{};
    std::uint64_t pagedPoolBytes{};
    std::uint64_t compressedBytes{};
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
[[nodiscard]] Result<void> validateTelemetrySnapshot(
    const TelemetrySnapshot& snapshot,
    const ResourceBudgets& budgets);

} // namespace ForgeConductor::Domain
