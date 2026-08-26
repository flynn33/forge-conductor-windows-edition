#pragma once

#include <cstddef>
#include <cstdint>

namespace ForgeConductor::Domain {

enum class ResourceProfile {
    Constrained8GiB,
    Standard16GiB,
    Expanded32GiBPlus
};

enum class ResourcePressureLevel {
    Nominal,
    Warning,
    Critical
};

struct ResourceBudgets final {
    double telemetrySampleHz{};
    double visibleUiRefreshHzMaximum{};
    double hiddenUiRefreshHzMaximum{};
    std::size_t telemetryPendingSnapshotsMaximum{};
    std::size_t historyPointsDefault{};
    std::size_t historyPointsHardMaximum{};
    std::size_t openProjectRepositoriesMaximum{};
    std::size_t diagnosticLogFilesMaximum{};
    std::size_t diagnosticLogFileBytesMaximum{};
    std::size_t toolStdoutBytesMaximum{};
    std::size_t toolStderrBytesMaximum{};
    std::uint32_t shellTimeoutSecondsMaximum{};
    std::size_t mcpInputLineBytesMaximum{};
    std::size_t namedPipeFrameBytesMaximum{};
    std::size_t guiPrivateBytesSteadyMiBMaximum{};
    std::size_t managerPrivateBytesSteadyMiBMaximum{};
    std::size_t mcpPrivateBytesSteadyMiBMaximum{};
    double combinedGuiManagerIdleCpuPercentMaximum{};
    double hiddenGuiGpuPercentMaximum{};
    std::size_t oneHourPrivateBytesGrowthMiBMaximum{};
    double oneHourHandleGrowthPercentMaximum{};
    std::size_t guiThreadsMaximum{};
    std::size_t managerThreadsMaximum{};

    bool operator==(const ResourceBudgets&) const = default;
};

[[nodiscard]] ResourceBudgets budgetsForProfile(ResourceProfile profile) noexcept;

// The Windows expanded profile is deliberately reserved for machines with at
// least 32 GiB. Machines between the standard and expanded thresholds retain
// the lower standard ceilings.
[[nodiscard]] ResourceProfile selectResourceProfile(
    std::uint64_t physicalMemoryBytes) noexcept;

} // namespace ForgeConductor::Domain
