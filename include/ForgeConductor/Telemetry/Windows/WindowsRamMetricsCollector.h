#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"

#include <memory>

namespace ForgeConductor::Telemetry::Windows {
namespace Detail {
class IRamMetricsPlatform;
struct WindowsRamMetricsCollectorTestAccess;
} // namespace Detail

class WindowsRamMetricsCollector final
    : public Contracts::IRamMetricsCollector {
public:
    // The composition root owns the clock and must keep it alive until this
    // synchronous collector has been shut down and destroyed.
    explicit WindowsRamMetricsCollector(Contracts::IClock& clock);
    ~WindowsRamMetricsCollector() override;

    WindowsRamMetricsCollector(const WindowsRamMetricsCollector&) = delete;
    WindowsRamMetricsCollector& operator=(
        const WindowsRamMetricsCollector&) = delete;
    WindowsRamMetricsCollector(WindowsRamMetricsCollector&&) = delete;
    WindowsRamMetricsCollector& operator=(
        WindowsRamMetricsCollector&&) = delete;

    [[nodiscard]] Domain::Result<Domain::RamMetrics> collect(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    friend struct Detail::WindowsRamMetricsCollectorTestAccess;

    WindowsRamMetricsCollector(
        Contracts::IClock& clock,
        std::shared_ptr<Detail::IRamMetricsPlatform> platform);

    class Impl;
    std::shared_ptr<Impl> implementation_;
};

[[nodiscard]] std::unique_ptr<Contracts::IRamMetricsCollector>
createWindowsRamMetricsCollector(Contracts::IClock& clock);

} // namespace ForgeConductor::Telemetry::Windows
