#include "ForgeConductor/Domain/TelemetryModels.h"

#include <algorithm>
#include <cmath>

namespace ForgeConductor::Domain {

TelemetryStatusTone toneFor(const TelemetryHealth health) noexcept
{
    switch (health) {
    case TelemetryHealth::Ok: return TelemetryStatusTone::Healthy;
    case TelemetryHealth::Warn: return TelemetryStatusTone::Caution;
    case TelemetryHealth::Error:
    case TelemetryHealth::Down: return TelemetryStatusTone::Failure;
    case TelemetryHealth::Config: return TelemetryStatusTone::Informational;
    }
    return TelemetryStatusTone::Unavailable;
}

TelemetryStatusTone mostSevere(const std::vector<TelemetryStatusTone>& tones) noexcept
{
    const auto severity = [](const TelemetryStatusTone tone) {
        switch (tone) {
        case TelemetryStatusTone::Unavailable: return 0;
        case TelemetryStatusTone::Informational: return 1;
        case TelemetryStatusTone::Healthy: return 2;
        case TelemetryStatusTone::Caution: return 3;
        case TelemetryStatusTone::Failure: return 4;
        }
        return 0;
    };
    if (tones.empty()) {
        return TelemetryStatusTone::Unavailable;
    }
    return *std::max_element(tones.begin(), tones.end(), [&](const auto left, const auto right) {
        return severity(left) < severity(right);
    });
}

Result<void> validateTelemetrySnapshot(
    const TelemetrySnapshot& snapshot,
    const ResourceBudgets& budgets)
{
    if (snapshot.history.size() > budgets.historyPointsHardMaximum) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Telemetry history exceeds the active hard limit."));
    }
    const auto percentInvalid = [](const double value) {
        return !std::isfinite(value) || value < 0.0 || value > 100.0;
    };
    if (percentInvalid(snapshot.system.cpu.percent) ||
        percentInvalid(snapshot.system.ram.percent) ||
        std::any_of(snapshot.system.cpu.perLogicalProcessor.begin(),
                    snapshot.system.cpu.perLogicalProcessor.end(), percentInvalid)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry utilization percentages must be finite values within 0...100."));
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
