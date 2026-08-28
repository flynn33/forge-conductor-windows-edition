#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"
#include "ForgeConductor/Dashboard/DashboardApplicationJsonCodec.h"
#include "ForgeConductor/Dashboard/DashboardSseBroadcaster.h"
#include "ForgeConductor/Dashboard/IDashboardTelemetrySource.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ForgeConductor::Application {

struct DashboardTelemetrySourceConfiguration final {
    Domain::ResourceBudgets resourceBudgets;
    std::size_t maximumSubscriptions{
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions};
    std::size_t maximumEncodedBytes{
        Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes};
    bool exportPresent{};
    bool staticPresent{};
    bool nodeAvailable{};
};

// Application-owned projection and fan-out adapter. The manager composition
// root forwards its already-owned telemetry consumer to publish(); this class
// never starts, stops, or replaces the consumer on the injected service.
class DashboardTelemetrySource final
    : public Dashboard::IDashboardTelemetrySource {
public:
    DashboardTelemetrySource(const DashboardTelemetrySource&) = delete;
    DashboardTelemetrySource& operator=(const DashboardTelemetrySource&) =
        delete;
    DashboardTelemetrySource(DashboardTelemetrySource&&) = delete;
    DashboardTelemetrySource& operator=(DashboardTelemetrySource&&) = delete;
    ~DashboardTelemetrySource() noexcept override;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardTelemetrySource>>
    create(
        Contracts::ITelemetryService& telemetryService,
        Contracts::IRuntimeDiagnostics& runtimeDiagnostics,
        const Contracts::IClock& clock,
        DashboardTelemetrySourceConfiguration configuration) noexcept;

    // Synchronous producer ingress. A successful call validates and encodes
    // the immutable observation once, then publishes that one shared frame
    // pair to every capacity-one subscription.
    [[nodiscard]] Domain::Result<void> publish(
        Dashboard::DashboardTelemetryObservation observation) noexcept;

    [[nodiscard]] Domain::Result<Dashboard::DashboardTelemetryHealth> health(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Dashboard::DashboardTelemetryObservation>
    latest(const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<
        std::unique_ptr<Dashboard::IDashboardSseSubscription>>
    subscribe(
        const Dashboard::DashboardStreamRateSelection& rate,
        const Domain::OperationContext& context) noexcept override;

    // Bounded close transition for adapter-owned frames and subscriptions.
    // Composition disables telemetry fan-out and handler admission first,
    // retains this object until those callers drain, then destroys it before
    // the telemetry service, runtime diagnostics, and clock.
    void shutdown() noexcept override;

private:
    DashboardTelemetrySource(
        Contracts::ITelemetryService& telemetryService,
        Contracts::IRuntimeDiagnostics& runtimeDiagnostics,
        const Contracts::IClock& clock,
        DashboardTelemetrySourceConfiguration configuration,
        Dashboard::DashboardSseBroadcaster broadcaster) noexcept;

    [[nodiscard]] Domain::Result<void> publishInternal(
        Dashboard::DashboardTelemetryObservation observation,
        bool marksProducerRunning) noexcept;

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept;

    Contracts::ITelemetryService& telemetryService_;
    Contracts::IRuntimeDiagnostics& runtimeDiagnostics_;
    const Contracts::IClock& clock_;
    const DashboardTelemetrySourceConfiguration configuration_;
    Dashboard::DashboardSseBroadcaster broadcaster_;

    // Only one producer callback is expected. Contending ingress is rejected
    // instead of queued so callback work can never form a backlog.
    std::atomic_flag publicationActive_ = ATOMIC_FLAG_INIT;
    mutable std::mutex stateMutex_;
    Dashboard::DashboardTelemetryObservation latestObservation_;
    Dashboard::DashboardSseFramePair::ImmutableFrame latestFrame_;
    std::uint64_t nextSourceSequence_{1U};
    bool sequenceExhausted_{};
    bool streamRunning_{};
    double measuredSampleHz_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Application
