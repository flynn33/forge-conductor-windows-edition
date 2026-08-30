#include "ForgeConductor/Domain/TelemetryModels.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace ForgeConductor::Domain {
namespace {

constexpr std::int64_t FirstUnsupportedUtcSecond = 253'402'300'800LL;

[[nodiscard]] bool isCanonicalTelemetryTime(const UtcTimePoint value) noexcept
{
    const auto elapsed = value.time_since_epoch();
    if (elapsed < UtcTimePoint::duration::zero()) {
        return false;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() <
        FirstUnsupportedUtcSecond;
}

} // namespace

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

std::string_view telemetryMetricAvailabilityName(
    const TelemetryMetricAvailability availability) noexcept
{
    switch (availability) {
    case TelemetryMetricAvailability::Available: return "available";
    case TelemetryMetricAvailability::WarmingUp: return "warming_up";
    case TelemetryMetricAvailability::Unsupported: return "unsupported";
    case TelemetryMetricAvailability::TemporarilyUnavailable:
        return "temporarily_unavailable";
    case TelemetryMetricAvailability::AccessDenied: return "access_denied";
    }
    return "temporarily_unavailable";
}

Result<void> validateTelemetryMetricState(
    const bool hasValue,
    const std::optional<UtcTimePoint>& capturedAt,
    const std::optional<UtcTimePoint>& observedAt,
    const TelemetryMetricAvailability availability,
    const bool stale,
    const std::string_view source,
    const std::optional<std::string>& unavailableReason)
{
    switch (availability) {
    case TelemetryMetricAvailability::Available:
    case TelemetryMetricAvailability::WarmingUp:
    case TelemetryMetricAvailability::Unsupported:
    case TelemetryMetricAvailability::TemporarilyUnavailable:
    case TelemetryMetricAvailability::AccessDenied:
        break;
    default:
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry metric availability state is invalid."));
    }
    if (source.empty() ||
        source.size() > TelemetryMetricSourceBytesMaximum ||
        source.find('\0') != std::string_view::npos ||
        !isValidUtf8(source)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry metric source text is missing, oversized, or invalid UTF-8."));
    }
    if (unavailableReason &&
        (unavailableReason->empty() ||
         unavailableReason->size() > TelemetryMetricReasonBytesMaximum ||
         unavailableReason->find('\0') != std::string::npos ||
         !isValidUtf8(*unavailableReason))) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry metric unavailable reason is empty, oversized, or invalid UTF-8."));
    }

    if ((capturedAt && !isCanonicalTelemetryTime(*capturedAt)) ||
        (observedAt && !isCanonicalTelemetryTime(*observedAt))) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry metric timestamps must use the canonical UTC range."));
    }
    if (capturedAt && observedAt && *capturedAt > *observedAt) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry metric capture time cannot follow its observation time."));
    }

    if (availability == TelemetryMetricAvailability::Available) {
        if (!hasValue || !capturedAt || !observedAt || stale ||
            unavailableReason.has_value()) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "An available telemetry metric requires a captured value and no failure state."));
        }
        return Result<void>::success();
    }

    if (!unavailableReason) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "An unavailable telemetry metric requires a bounded reason."));
    }
    if (hasValue != stale) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "An unavailable telemetry metric may retain a value only when it is stale."));
    }
    if (capturedAt.has_value() != stale) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "An unavailable telemetry metric may retain a capture time only when it is stale."));
    }
    if (!observedAt &&
        (availability != TelemetryMetricAvailability::WarmingUp || stale)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "A telemetry failure state requires the time it was observed."));
    }
    return Result<void>::success();
}

Result<void> validateRamMetrics(const RamMetrics& metrics)
{
    const auto validate = [](const std::string_view field, const auto& metric)
        -> Result<void> {
        const auto result = validateTelemetryMetric(metric);
        if (!result) {
            return Result<void>::failure(makeError(
                result.error().code,
                "RAM telemetry field '" + std::string{field} +
                    "' is invalid: " + result.error().message));
        }
        return Result<void>::success();
    };

    const Result<void> stateResults[] = {
        validate("total_bytes", metrics.totalBytes),
        validate("used_bytes", metrics.usedBytes),
        validate("available_bytes", metrics.availableBytes),
        validate("percent", metrics.percent),
        validate("pressure_percent", metrics.pressurePercent),
        validate("active_bytes", metrics.activeBytes),
        validate("wired_bytes", metrics.wiredBytes),
        validate("compressed_bytes", metrics.compressedBytes),
        validate("swap_total_bytes", metrics.swapTotalBytes),
        validate("swap_used_bytes", metrics.swapUsedBytes),
        validate("swap_percent", metrics.swapPercent),
        validate("committed_bytes", metrics.committedBytes),
        validate("paged_pool_bytes", metrics.pagedPoolBytes)};
    for (const auto& result : stateResults) {
        if (!result) {
            return Result<void>::failure(result.error());
        }
    }

    const auto percentInvalid = [](const std::optional<double>& value) {
        return value &&
            (!std::isfinite(*value) || *value < 0.0 || *value > 100.0);
    };
    if (percentInvalid(metrics.percent.value) ||
        percentInvalid(metrics.pressurePercent.value) ||
        percentInvalid(metrics.swapPercent.value)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "RAM telemetry percentages must be finite values within 0...100."));
    }
    if (metrics.totalBytes.value && *metrics.totalBytes.value == 0U) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Available total RAM must be greater than zero."));
    }
    return Result<void>::success();
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
    const auto ramValid = validateRamMetrics(snapshot.system.ram);
    if (!ramValid) {
        return Result<void>::failure(ramValid.error());
    }
    if (percentInvalid(snapshot.system.cpu.percent) ||
        std::any_of(snapshot.system.cpu.perLogicalProcessor.begin(),
                    snapshot.system.cpu.perLogicalProcessor.end(), percentInvalid)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Telemetry utilization percentages must be finite values within 0...100."));
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
