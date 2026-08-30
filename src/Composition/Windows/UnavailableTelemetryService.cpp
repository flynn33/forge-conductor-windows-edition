#include "UnavailableTelemetryService.h"

#include "ForgeConductor/Domain/Error.h"

#include <utility>

namespace ForgeConductor::Composition::Windows {
namespace {

[[nodiscard]] Domain::Error unavailableError()
{
    return Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "Windows native telemetry collectors are unavailable until the "
        "telemetry phase is active.",
        true);
}

[[nodiscard]] Domain::Error internalError()
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The unavailable telemetry capability failed safely.");
}

} // namespace

UnavailableTelemetryService::UnavailableTelemetryService(
    const Contracts::IClock& clock) noexcept
    : clock_{clock}
{
}

Domain::Result<void> UnavailableTelemetryService::validateContext(
    const Domain::OperationContext& context) const noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The unavailable telemetry operation was cancelled."));
        }
        if (context.isExpired(clock_.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The unavailable telemetry operation deadline expired.",
                true));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalError());
    }
}

Domain::Result<void> UnavailableTelemetryService::start(
    const Domain::OperationContext& context) noexcept
{
    return validateContext(context);
}

Domain::Result<Contracts::ITelemetryService::Snapshot>
UnavailableTelemetryService::sample(
    const bool forceForgeComposition,
    const Domain::OperationContext& context) noexcept
{
    static_cast<void>(forceForgeComposition);
    try {
        auto valid = validateContext(context);
        if (!valid) {
            return Domain::Result<Snapshot>::failure(
                std::move(valid).error());
        }
        return Domain::Result<Snapshot>::failure(unavailableError());
    } catch (...) {
        return Domain::Result<Snapshot>::failure(internalError());
    }
}

Domain::Result<Domain::TelemetryHealthReport>
UnavailableTelemetryService::health(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto valid = validateContext(context);
        if (!valid) {
            return Domain::Result<Domain::TelemetryHealthReport>::failure(
                std::move(valid).error());
        }
        return Domain::Result<Domain::TelemetryHealthReport>::success(
            Domain::TelemetryHealthReport{
                false,
                "forge-telemetry",
                "windows-native",
                false,
                "unavailable",
                "Windows native telemetry collectors unavailable",
                "WinUI 3 + SSE",
                false});
    } catch (...) {
        return Domain::Result<Domain::TelemetryHealthReport>::failure(
            internalError());
    }
}

Domain::Result<void> UnavailableTelemetryService::setConsumer(
    Consumer consumer) noexcept
{
    // Destruction at return deliberately releases every capture. Retaining a
    // consumer while no producer exists would create hidden callback ownership.
    static_cast<void>(consumer);
    return Domain::Result<void>::success();
}

Contracts::ITelemetryService::Snapshot
UnavailableTelemetryService::latest() const noexcept
{
    return {};
}

std::size_t UnavailableTelemetryService::pendingCount() const noexcept
{
    return 0U;
}

void UnavailableTelemetryService::stop() noexcept
{
}

} // namespace ForgeConductor::Composition::Windows
