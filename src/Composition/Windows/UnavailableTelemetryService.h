#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"

#include <cstddef>

namespace ForgeConductor::Composition::Windows {

// Production P16 capability marker used until P17 installs the native Windows
// collectors. Health remains queryable, but no observation or callback is ever
// fabricated. The injected clock is borrowed and remains composition-owned.
class UnavailableTelemetryService final
    : public Contracts::ITelemetryService {
public:
    explicit UnavailableTelemetryService(
        const Contracts::IClock& clock) noexcept;
    ~UnavailableTelemetryService() noexcept override = default;

    UnavailableTelemetryService(const UnavailableTelemetryService&) = delete;
    UnavailableTelemetryService& operator=(
        const UnavailableTelemetryService&) = delete;
    UnavailableTelemetryService(UnavailableTelemetryService&&) = delete;
    UnavailableTelemetryService& operator=(
        UnavailableTelemetryService&&) = delete;

    [[nodiscard]] Domain::Result<void> start(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Snapshot> sample(
        bool forceForgeComposition,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> setConsumer(
        Consumer consumer) noexcept override;

    [[nodiscard]] Snapshot latest() const noexcept override;
    [[nodiscard]] std::size_t pendingCount() const noexcept override;

    // This adapter owns no thread, callback, queue, or snapshot. stop() is an
    // idempotent no-op; replacing it with P17 telemetry supplies the lifecycle.
    void stop() noexcept override;

private:
    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept;

    const Contracts::IClock& clock_;
};

} // namespace ForgeConductor::Composition::Windows
