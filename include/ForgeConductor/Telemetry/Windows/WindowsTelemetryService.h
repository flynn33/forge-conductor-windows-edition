#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"

#include <memory>

namespace ForgeConductor::Telemetry::Windows {

class WindowsTelemetryService final : public Contracts::ITelemetryService {
public:
    WindowsTelemetryService(
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator,
        Domain::PathText home,
        Domain::ResourceBudgets budgets);
    ~WindowsTelemetryService() override;

    WindowsTelemetryService(const WindowsTelemetryService&) = delete;
    WindowsTelemetryService& operator=(const WindowsTelemetryService&) = delete;
    WindowsTelemetryService(WindowsTelemetryService&&) = delete;
    WindowsTelemetryService& operator=(WindowsTelemetryService&&) = delete;

    [[nodiscard]] Domain::Result<void> start(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Snapshot> sample(
        bool forceForgeComposition,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<void> setConsumer(Consumer consumer) noexcept override;
    [[nodiscard]] Snapshot latest() const noexcept override;
    [[nodiscard]] std::size_t pendingCount() const noexcept override;
    void stop() noexcept override;

private:
    class Impl;
    std::shared_ptr<Impl> implementation_;
};

[[nodiscard]] std::unique_ptr<Contracts::ITelemetryService>
createWindowsTelemetryService(
    Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuidGenerator,
    Domain::PathText home,
    Domain::ResourceBudgets budgets);

} // namespace ForgeConductor::Telemetry::Windows
