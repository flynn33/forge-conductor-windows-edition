#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"

#include <memory>

namespace ForgeConductor::Telemetry::Windows {
namespace Detail {
class ICpuMetricsPlatform;
struct WindowsCpuMetricsCollectorTestAccess;
} // namespace Detail

class WindowsCpuMetricsCollector final
    : public Contracts::ICpuMetricsCollector {
public:
    // The composition root owns the clock and must keep it alive until this
    // synchronous collector has been shut down and destroyed.
    explicit WindowsCpuMetricsCollector(Contracts::IClock& clock);
    ~WindowsCpuMetricsCollector() override;

    WindowsCpuMetricsCollector(const WindowsCpuMetricsCollector&) = delete;
    WindowsCpuMetricsCollector& operator=(
        const WindowsCpuMetricsCollector&) = delete;
    WindowsCpuMetricsCollector(WindowsCpuMetricsCollector&&) = delete;
    WindowsCpuMetricsCollector& operator=(
        WindowsCpuMetricsCollector&&) = delete;

    [[nodiscard]] Domain::Result<Domain::CpuMetrics> collect(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    friend struct Detail::WindowsCpuMetricsCollectorTestAccess;

    WindowsCpuMetricsCollector(
        Contracts::IClock& clock,
        std::shared_ptr<Detail::ICpuMetricsPlatform> platform);

    class Impl;
    std::shared_ptr<Impl> implementation_;
};

[[nodiscard]] std::unique_ptr<Contracts::ICpuMetricsCollector>
createWindowsCpuMetricsCollector(Contracts::IClock& clock);

} // namespace ForgeConductor::Telemetry::Windows
