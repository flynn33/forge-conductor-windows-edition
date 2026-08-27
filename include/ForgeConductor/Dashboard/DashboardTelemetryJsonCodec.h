#pragma once

#include "ForgeConductor/Domain/TelemetryModels.h"

#include <cstddef>
#include <optional>
#include <string>

namespace ForgeConductor::Dashboard {

// Pure telemetry-to-dashboard projection. The codec owns no collector,
// transport, route, or operating-system resource.
class DashboardTelemetryJsonCodec final {
public:
    static constexpr std::size_t DefaultMaximumEncodedBytes =
        2U * 1024U * 1024U;
    static constexpr std::size_t MaximumLogicalProcessorEntries = 2'048U;
    static constexpr std::size_t MaximumDiskVolumes = 256U;
    static constexpr std::size_t MaximumGpuDevices = 64U;
    static constexpr std::size_t MaximumProcesses = 4'096U;
    static constexpr std::size_t MaximumTools = 4'096U;
    static constexpr std::size_t MaximumAgentSessions =
        Domain::AgentSessionLimits::MaximumSessionQueryRows;
    static constexpr std::size_t MaximumHistoryPoints = 7'200U;
    static constexpr std::size_t MaximumCompactProcesses = 8U;
    static constexpr std::size_t MaximumCompactHistoryPoints = 20U;

    [[nodiscard]] static Domain::Result<std::string> encodeHealth(
        const Domain::TelemetryHealthReport& report,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    // /api/live and /api/frame are equivalent current-frame views.
    [[nodiscard]] static Domain::Result<std::string> encodeLiveFrame(
        const Domain::TelemetrySnapshot& snapshot,
        std::optional<double> measuredSampleHz = std::nullopt,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeFrame(
        const Domain::TelemetrySnapshot& snapshot,
        std::optional<double> measuredSampleHz = std::nullopt,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    // /api/snapshot remains a compatibility alias for the current live frame.
    [[nodiscard]] static Domain::Result<std::string> encodeSnapshot(
        const Domain::TelemetrySnapshot& snapshot,
        std::optional<double> measuredSampleHz = std::nullopt,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeSystem(
        const Domain::SystemMetrics& system,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeForge(
        const Domain::ForgeSnapshot& forge,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    // One complete SSE telemetry event. The framing bytes count toward the
    // same bound as the JSON payload.
    [[nodiscard]] static Domain::Result<std::string> encodeServerSentEvent(
        const Domain::TelemetrySnapshot& snapshot,
        std::optional<double> measuredSampleHz = std::nullopt,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;

    // Compact SSE frames preserve the realtime system surface while omitting
    // Forge detail, power, all but the first eight process rows, and all but
    // the newest twenty history points. The caller still supplies one complete
    // immutable snapshot so compact and full representations describe the
    // same producer event.
    [[nodiscard]] static Domain::Result<std::string>
    encodeCompactServerSentEvent(
        const Domain::TelemetrySnapshot& snapshot,
        std::optional<double> measuredSampleHz = std::nullopt,
        std::size_t maximumEncodedBytes = DefaultMaximumEncodedBytes) noexcept;
};

} // namespace ForgeConductor::Dashboard
