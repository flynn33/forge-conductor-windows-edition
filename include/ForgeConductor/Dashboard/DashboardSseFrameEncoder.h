#pragma once

#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Dashboard/DashboardTelemetryJsonCodec.h"
#include "ForgeConductor/Domain/TelemetryModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ForgeConductor::Dashboard {

// Produces one immutable compact/full SSE pair from one telemetry snapshot.
// The broadcaster and every connection share the resulting byte buffers;
// neither transport nor mailbox code reserializes telemetry business data.
class DashboardSseFrameEncoder final {
public:
    [[nodiscard]] static Domain::Result<
        DashboardSseFramePair::ImmutableFrame>
    encode(
        std::uint64_t sourceSequence,
        const Domain::TelemetrySnapshot& snapshot,
        std::optional<double> measuredSampleHz = std::nullopt,
        std::size_t maximumEncodedBytes =
            DashboardTelemetryJsonCodec::DefaultMaximumEncodedBytes) noexcept;
};

} // namespace ForgeConductor::Dashboard
