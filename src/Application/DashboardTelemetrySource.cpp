#include "ForgeConductor/Application/DashboardTelemetrySource.h"

#include "ForgeConductor/Dashboard/DashboardSseFrameEncoder.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/TelemetryModels.h"

#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::Error cancelledError()
{
    return Domain::makeError(
        Domain::ErrorCodes::Cancelled,
        "The dashboard telemetry source is shut down.");
}

[[nodiscard]] Domain::Error contextCancelledError()
{
    return Domain::makeError(
        Domain::ErrorCodes::Cancelled,
        "The dashboard telemetry operation was cancelled.");
}

[[nodiscard]] Domain::Error deadlineError()
{
    return Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "The dashboard telemetry operation deadline expired.",
        true);
}

[[nodiscard]] Domain::Error internalError()
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard telemetry source failed safely.");
}

[[nodiscard]] Domain::Result<void> validateConfiguration(
    const DashboardTelemetrySourceConfiguration& configuration) noexcept
{
    const auto& budgets = configuration.resourceBudgets;
    if (!std::isfinite(budgets.telemetrySampleHz) ||
        (budgets.telemetrySampleHz != 1.0 &&
         budgets.telemetrySampleHz != 2.0) ||
        budgets.telemetryPendingSnapshotsMaximum !=
            Domain::TelemetryPendingSnapshotsMaximum ||
        budgets.historyPointsHardMaximum == 0U ||
        budgets.historyPointsDefault > budgets.historyPointsHardMaximum ||
        budgets.historyPointsHardMaximum >
            Dashboard::DashboardTelemetryJsonCodec::MaximumHistoryPoints ||
        configuration.maximumSubscriptions == 0U ||
        configuration.maximumSubscriptions >
            Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions ||
        configuration.maximumEncodedBytes == 0U ||
        configuration.maximumEncodedBytes >
            Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry source configuration violates a fixed "
            "resource or response bound."));
    }
    return Domain::Result<void>::success();
}

class PublicationLease final {
public:
    explicit PublicationLease(std::atomic_flag& flag) noexcept
        : flag_{flag},
          acquired_{!flag_.test_and_set(std::memory_order_acquire)}
    {
    }

    PublicationLease(const PublicationLease&) = delete;
    PublicationLease& operator=(const PublicationLease&) = delete;

    ~PublicationLease() noexcept
    {
        if (acquired_) {
            flag_.clear(std::memory_order_release);
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    std::atomic_flag& flag_;
    const bool acquired_{};
};

} // namespace

DashboardTelemetrySource::DashboardTelemetrySource(
    Contracts::ITelemetryService& telemetryService,
    Contracts::IRuntimeDiagnostics& runtimeDiagnostics,
    const Contracts::IClock& clock,
    DashboardTelemetrySourceConfiguration configuration,
    Dashboard::DashboardSseBroadcaster broadcaster) noexcept
    : telemetryService_{telemetryService},
      runtimeDiagnostics_{runtimeDiagnostics},
      clock_{clock},
      configuration_{std::move(configuration)},
      broadcaster_{std::move(broadcaster)}
{
}

DashboardTelemetrySource::~DashboardTelemetrySource() noexcept
{
    shutdown();
}

Domain::Result<std::unique_ptr<DashboardTelemetrySource>>
DashboardTelemetrySource::create(
    Contracts::ITelemetryService& telemetryService,
    Contracts::IRuntimeDiagnostics& runtimeDiagnostics,
    const Contracts::IClock& clock,
    DashboardTelemetrySourceConfiguration configuration) noexcept
{
    try {
        const auto valid = validateConfiguration(configuration);
        if (!valid) {
            return Domain::Result<
                std::unique_ptr<DashboardTelemetrySource>>::failure(
                    valid.error());
        }

        auto broadcaster = Dashboard::DashboardSseBroadcaster::create(
            configuration.maximumSubscriptions);
        if (!broadcaster) {
            return Domain::Result<
                std::unique_ptr<DashboardTelemetrySource>>::failure(
                    std::move(broadcaster).error());
        }

        std::unique_ptr<DashboardTelemetrySource> source{
            new DashboardTelemetrySource{
                telemetryService,
                runtimeDiagnostics,
                clock,
                std::move(configuration),
                std::move(broadcaster).value()}};

        const auto initial = telemetryService.latest();
        if (initial != nullptr) {
            auto seeded = source->publishInternal(
                Dashboard::DashboardTelemetryObservation{
                    initial, std::nullopt},
                false);
            if (!seeded) {
                return Domain::Result<
                    std::unique_ptr<DashboardTelemetrySource>>::failure(
                        std::move(seeded).error());
            }
        }

        return Domain::Result<
            std::unique_ptr<DashboardTelemetrySource>>::success(
                std::move(source));
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardTelemetrySource>>::failure(
                internalError());
    }
}

Domain::Result<void> DashboardTelemetrySource::publish(
    Dashboard::DashboardTelemetryObservation observation) noexcept
{
    return publishInternal(std::move(observation), true);
}

Domain::Result<void> DashboardTelemetrySource::publishInternal(
    Dashboard::DashboardTelemetryObservation observation,
    const bool marksProducerRunning) noexcept
{
    PublicationLease publication{publicationActive_};
    if (!publication.acquired()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::RateLimited,
            "A dashboard telemetry publication is already in progress.",
            true));
    }

    try {
        if (observation.snapshot == nullptr) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The telemetry producer supplied a null dashboard "
                "observation."));
        }

        std::uint64_t sequence{};
        {
            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return Domain::Result<void>::failure(cancelledError());
            }
            if (sequenceExhausted_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The dashboard telemetry source sequence is "
                    "exhausted."));
            }
            sequence = nextSourceSequence_;
        }

        const auto domainValid = Domain::validateTelemetrySnapshot(
            *observation.snapshot, configuration_.resourceBudgets);
        if (!domainValid) {
            return Domain::Result<void>::failure(domainValid.error());
        }

        auto encoded = Dashboard::DashboardSseFrameEncoder::encode(
            sequence,
            *observation.snapshot,
            observation.measuredSampleHz,
            configuration_.maximumEncodedBytes);
        if (!encoded) {
            return Domain::Result<void>::failure(std::move(encoded).error());
        }
        auto frame = std::move(encoded).value();

        {
            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return Domain::Result<void>::failure(cancelledError());
            }
            latestObservation_ = std::move(observation);
            latestFrame_ = frame;
            if (marksProducerRunning) {
                streamRunning_ = true;
            }
            measuredSampleHz_ =
                latestObservation_.measuredSampleHz.value_or(0.0);
            if (sequence == (std::numeric_limits<std::uint64_t>::max)()) {
                sequenceExhausted_ = true;
            } else {
                nextSourceSequence_ = sequence + 1U;
            }
        }

        return broadcaster_.publish(std::move(frame));
    } catch (...) {
        return Domain::Result<void>::failure(internalError());
    }
}

Domain::Result<void> DashboardTelemetrySource::validateContext(
    const Domain::OperationContext& context) const noexcept
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(contextCancelledError());
    }
    if (context.isExpired(clock_.monotonicNow())) {
        return Domain::Result<void>::failure(deadlineError());
    }
    return Domain::Result<void>::success();
}

Domain::Result<Dashboard::DashboardTelemetryHealth>
DashboardTelemetrySource::health(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = validateContext(context);
        if (!validContext) {
            return Domain::Result<
                Dashboard::DashboardTelemetryHealth>::failure(
                    std::move(validContext).error());
        }
        {
            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return Domain::Result<
                    Dashboard::DashboardTelemetryHealth>::failure(
                        cancelledError());
            }
        }

        auto report = telemetryService_.health(context);
        if (!report) {
            return Domain::Result<
                Dashboard::DashboardTelemetryHealth>::failure(
                    std::move(report).error());
        }

        validContext = validateContext(context);
        if (!validContext) {
            return Domain::Result<
                Dashboard::DashboardTelemetryHealth>::failure(
                    std::move(validContext).error());
        }
        auto diagnostics = runtimeDiagnostics_.snapshot(context);
        if (!diagnostics) {
            return Domain::Result<
                Dashboard::DashboardTelemetryHealth>::failure(
                    std::move(diagnostics).error());
        }

        double measuredSampleHz{};
        bool streamRunning{};
        {
            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return Domain::Result<
                    Dashboard::DashboardTelemetryHealth>::failure(
                        cancelledError());
            }
            measuredSampleHz = measuredSampleHz_;
            streamRunning = streamRunning_;
        }

        Dashboard::DashboardTelemetryHealth value{
            std::move(report).value(),
            configuration_.resourceBudgets.telemetrySampleHz,
            measuredSampleHz,
            streamRunning,
            std::move(diagnostics).value(),
            configuration_.exportPresent,
            configuration_.staticPresent,
            configuration_.nodeAvailable};

        auto bounded = Dashboard::DashboardApplicationJsonCodec::encodeHealth(
            value, configuration_.maximumEncodedBytes);
        if (!bounded) {
            return Domain::Result<
                Dashboard::DashboardTelemetryHealth>::failure(
                    std::move(bounded).error());
        }
        return Domain::Result<Dashboard::DashboardTelemetryHealth>::success(
            std::move(value));
    } catch (...) {
        return Domain::Result<
            Dashboard::DashboardTelemetryHealth>::failure(internalError());
    }
}

Domain::Result<Dashboard::DashboardTelemetryObservation>
DashboardTelemetrySource::latest(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = validateContext(context);
        if (!validContext) {
            return Domain::Result<
                Dashboard::DashboardTelemetryObservation>::failure(
                    std::move(validContext).error());
        }

        std::lock_guard lock{stateMutex_};
        if (shutdown_) {
            return Domain::Result<
                Dashboard::DashboardTelemetryObservation>::failure(
                    cancelledError());
        }
        if (latestObservation_.snapshot == nullptr) {
            return Domain::Result<
                Dashboard::DashboardTelemetryObservation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::HostCapabilityUnavailable,
                        "No dashboard telemetry observation is available.",
                        true));
        }
        return Domain::Result<
            Dashboard::DashboardTelemetryObservation>::success(
                latestObservation_);
    } catch (...) {
        return Domain::Result<
            Dashboard::DashboardTelemetryObservation>::failure(
                internalError());
    }
}

Domain::Result<std::unique_ptr<Dashboard::IDashboardSseSubscription>>
DashboardTelemetrySource::subscribe(
    const Dashboard::DashboardStreamRateSelection& rate,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = validateContext(context);
        if (!validContext) {
            return Domain::Result<std::unique_ptr<
                Dashboard::IDashboardSseSubscription>>::failure(
                    std::move(validContext).error());
        }
        if (!std::isfinite(rate.deliveryHz) ||
            rate.deliveryHz <
                Dashboard::IDashboardSseSubscription::MinimumDeliveryHz ||
            rate.deliveryHz >
                Dashboard::IDashboardSseSubscription::MaximumDeliveryHz ||
            rate.deliveryHz >
                configuration_.resourceBudgets.telemetrySampleHz) {
            return Domain::Result<std::unique_ptr<
                Dashboard::IDashboardSseSubscription>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "Dashboard telemetry delivery frequency violates the "
                        "active 1 to 2 Hz resource bound."));
        }

        Dashboard::DashboardSseFramePair::ImmutableFrame initialFrame;
        {
            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return Domain::Result<std::unique_ptr<
                    Dashboard::IDashboardSseSubscription>>::failure(
                        cancelledError());
            }
            initialFrame = latestFrame_;
        }
        if (initialFrame == nullptr) {
            return Domain::Result<std::unique_ptr<
                Dashboard::IDashboardSseSubscription>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::HostCapabilityUnavailable,
                        "No dashboard telemetry frame is available for a "
                        "stream subscription.",
                        true));
        }

        auto subscribed = broadcaster_.subscribe(
            std::move(initialFrame), rate.deliveryHz);
        if (!subscribed) {
            return Domain::Result<std::unique_ptr<
                Dashboard::IDashboardSseSubscription>>::failure(
                    std::move(subscribed).error());
        }
        std::unique_ptr<Dashboard::IDashboardSseSubscription> erased{
            std::move(subscribed).value()};
        return Domain::Result<std::unique_ptr<
            Dashboard::IDashboardSseSubscription>>::success(
                std::move(erased));
    } catch (...) {
        return Domain::Result<std::unique_ptr<
            Dashboard::IDashboardSseSubscription>>::failure(
                internalError());
    }
}

void DashboardTelemetrySource::shutdown() noexcept
{
    try {
        {
            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return;
            }
            shutdown_ = true;
            streamRunning_ = false;
            measuredSampleHz_ = 0.0;
            latestObservation_ = {};
            latestFrame_.reset();
        }
        broadcaster_.shutdown();
    } catch (...) {
        broadcaster_.shutdown();
    }
}

} // namespace ForgeConductor::Application
