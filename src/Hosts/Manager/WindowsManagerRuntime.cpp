#include "WindowsManagerRuntime.h"

#include "Infrastructure/Windows/Detail/ManagerDashboardOperationalState.h"
#include "Infrastructure/Windows/Detail/WindowsDashboardRuntime.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

using DashboardRuntime =
    Infrastructure::Windows::Detail::WindowsDashboardRuntime;
using DashboardRuntimeBinding =
    Infrastructure::Windows::Detail::WindowsDashboardRuntimeBinding;
using DashboardRuntimeLifecycle =
    Infrastructure::Windows::Detail::WindowsDashboardRuntimeLifecycle;
using DashboardOperationalState =
    Infrastructure::Windows::Detail::ManagerDashboardOperationalState;

enum class TransitionKind : std::uint8_t {
    Start,
    ExplicitRebind,
    Reconcile,
};

[[nodiscard]] Domain::Error managerRuntimeError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidDependenciesError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::InvalidRequest,
        "The Windows manager runtime requires a clock, UUID generator, and dashboard application factory.");
}

[[nodiscard]] Domain::Error runtimeClosedError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::TransportClosed,
        "The Windows manager runtime is shutting down.");
}

[[nodiscard]] Domain::Error listenerUnavailableError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::Conflict,
        "The Windows manager runtime has no listening dashboard to pause.");
}

[[nodiscard]] Domain::Error nonbindingSettingsChangedEndpointError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::Conflict,
        "A nonbinding manager settings update cannot change the dashboard endpoint.");
}

[[nodiscard]] Domain::Error nullApplicationError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard application factory returned a null application.");
}

[[nodiscard]] Domain::Error invalidLowerSnapshotError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The Windows dashboard runtime reported an incoherent listener snapshot.");
}

[[nodiscard]] Domain::Error restartCountExhaustedError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::Conflict,
        "The Windows manager runtime restart counter is exhausted.");
}

[[nodiscard]] Domain::Error internalRuntimeError()
{
    return managerRuntimeError(
        Domain::ErrorCodes::InternalFailure,
        "The Windows manager runtime failed safely.");
}

[[nodiscard]] Domain::Error incompleteCandidateCleanupError(
    const Domain::Error& transition,
    const Domain::Error& cleanup)
{
    return managerRuntimeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard transition failed (" + transition.message +
            ") and its candidate cleanup also failed (" + cleanup.message +
            ").");
}

[[nodiscard]] bool sameEndpoint(
    const Domain::DashboardConfig& left,
    const Domain::DashboardConfig& right) noexcept
{
    return left.host == right.host && left.port == right.port;
}

} // namespace

class WindowsManagerRuntime::Impl final {
public:
    Impl(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Dashboard::IDashboardConnectionApplicationFactory>
            applicationFactory) noexcept
        : clock_{std::move(clock)},
          uuidGenerator_{std::move(uuidGenerator)},
          applicationFactory_{std::move(applicationFactory)}
    {
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> start(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (auto admitted = validateMutationLocked(
                    config, context, "start"); !admitted) {
                return snapshotFailure(std::move(admitted).error());
            }

            auto transitioned = transitionLocked(
                config, true, TransitionKind::Start);
            if (!transitioned) {
                return runtimeFailureLocked(
                    std::move(transitioned).error());
            }
            if (std::move(transitioned).value() || !startedAt_.has_value()) {
                startedAt_ = clock_->utcNow();
            }
            lastError_.reset();
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> pause(
        const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (auto open = validateOpenContextLocked(context, "pause");
                !open) {
                return snapshotFailure(std::move(open).error());
            }

            auto observed = observeDashboardLocked();
            if (!observed) {
                return runtimeFailureLocked(std::move(observed).error());
            }
            if (!observed.value().listenerListening) {
                return snapshotFailure(listenerUnavailableError());
            }

            dashboardRuntime_->pauseOperationalService();
            lastError_.reset();
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> rebind(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (restartCount_ ==
                (std::numeric_limits<std::uint32_t>::max)()) {
                return runtimeFailureLocked(restartCountExhaustedError());
            }
            ++restartCount_;

            if (auto admitted = validateMutationLocked(
                    config, context, "rebind"); !admitted) {
                return snapshotFailure(std::move(admitted).error());
            }

            auto transitioned = transitionLocked(
                config,
                operationalServiceDesired,
                TransitionKind::ExplicitRebind);
            if (!transitioned) {
                return runtimeFailureLocked(
                    std::move(transitioned).error());
            }

            // A successful explicit restart begins a fresh runtime uptime
            // epoch even when the lower A/B path retained one process graph.
            startedAt_ = clock_->utcNow();
            lastError_.reset();
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> reconcile(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (auto admitted = validateMutationLocked(
                    config, context, "reconcile"); !admitted) {
                return snapshotFailure(std::move(admitted).error());
            }

            auto transitioned = transitionLocked(
                config,
                operationalServiceDesired,
                TransitionKind::Reconcile);
            if (!transitioned) {
                return runtimeFailureLocked(
                    std::move(transitioned).error());
            }
            if (std::move(transitioned).value() || !startedAt_.has_value()) {
                startedAt_ = clock_->utcNow();
            }
            lastError_.reset();
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> applySettings(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (auto admitted = validateMutationLocked(
                    config, context, "apply settings"); !admitted) {
                return snapshotFailure(std::move(admitted).error());
            }

            auto observed = observeDashboardLocked();
            if (!observed) {
                return runtimeFailureLocked(std::move(observed).error());
            }
            if (!observed.value().listenerListening ||
                !observed.value().configuration.has_value()) {
                return snapshotFailure(listenerUnavailableError());
            }
            if (!sameEndpoint(
                    *observed.value().configuration, config.dashboard)) {
                return snapshotFailure(
                    nonbindingSettingsChangedEndpointError());
            }

            // Refresh interval and other nonbinding policy are owned above the
            // immutable lower listener snapshot and apply to future requests
            // through their injected application services.
            lastError_.reset();
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (auto current = validateContextLocked(context, "snapshot");
                !current) {
                return snapshotFailure(std::move(current).error());
            }
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot>
    requestShutdown(const Domain::OperationContext& context) noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            if (auto current = validateContextLocked(
                    context, "request shutdown"); !current) {
                return snapshotFailure(std::move(current).error());
            }
            if (closed_) {
                return snapshotFailure(runtimeClosedError());
            }

            shutdownRequested_ = true;
            lastError_.reset();
            // The response-delivery owner publishes the process-stop edge only
            // after its acknowledgement is safe. This runtime only latches
            // intent and never drains a client transport from its callback.
            return snapshotLocked();
        } catch (...) {
            return exceptionFailure();
        }
    }

    void shutdown() noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            closed_ = true;
            shutdownRequested_ = true;
            if (dashboardRuntime_ == nullptr) {
                return;
            }

            dashboardRuntime_->pauseOperationalService();
            dashboardRuntime_->requestGracefulShutdown();
            auto waited = dashboardRuntime_->wait();
            if (!waited) {
                lastError_ = waited.error().message;
                return;
            }
            dashboardRuntime_.reset();
        } catch (...) {
            try {
                lastError_ = internalRuntimeError().message;
            } catch (...) {
            }
        }
    }

private:
    struct DashboardObservation final {
        bool listenerListening{};
        bool operationalServiceActive{};
        std::optional<Domain::DashboardConfig> configuration;
    };

    [[nodiscard]] Domain::Result<void> validateContextLocked(
        const Domain::OperationContext& context,
        const std::string_view action) const
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(managerRuntimeError(
                Domain::ErrorCodes::Cancelled,
                "The Windows manager runtime could not " +
                    std::string{action} + " because the operation was cancelled."));
        }
        if (context.isExpired(clock_->monotonicNow())) {
            return Domain::Result<void>::failure(managerRuntimeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The Windows manager runtime could not " +
                    std::string{action} + " because the deadline expired."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> validateOpenContextLocked(
        const Domain::OperationContext& context,
        const std::string_view action) const
    {
        if (closed_ || shutdownRequested_) {
            return Domain::Result<void>::failure(runtimeClosedError());
        }
        return validateContextLocked(context, action);
    }

    [[nodiscard]] Domain::Result<void> validateMutationLocked(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context,
        const std::string_view action) const
    {
        auto open = validateOpenContextLocked(context, action);
        if (!open) {
            return open;
        }
        return Domain::validateAppConfig(config);
    }

    [[nodiscard]] Domain::Result<DashboardObservation>
    observeDashboardLocked()
    {
        if (dashboardRuntime_ == nullptr) {
            return Domain::Result<DashboardObservation>::success({});
        }

        auto lower = dashboardRuntime_->snapshot();
        if (!lower) {
            return Domain::Result<DashboardObservation>::failure(
                std::move(lower).error());
        }
        auto observed = std::move(lower).value();
        const bool listening = observed.lifecycle() ==
            DashboardRuntimeLifecycle::Listening;
        if ((listening &&
             (!observed.configuration().has_value() ||
              observed.applicationIdentity() == nullptr)) ||
            (observed.operationalServiceActive() && !listening)) {
            return Domain::Result<DashboardObservation>::failure(
                invalidLowerSnapshotError());
        }

        DashboardObservation result;
        result.listenerListening = listening;
        result.operationalServiceActive =
            listening && observed.operationalServiceActive();
        if (observed.configuration().has_value()) {
            result.configuration = *observed.configuration();
        }
        return Domain::Result<DashboardObservation>::success(
            std::move(result));
    }

    [[nodiscard]] Domain::Result<
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>>
    createApplicationLocked(const Domain::DashboardConfig& configuration)
    {
        auto application = applicationFactory_->create(configuration);
        if (!application) {
            return application;
        }
        auto applicationOwner = std::move(application).value();
        if (applicationOwner == nullptr) {
            return Domain::Result<std::shared_ptr<
                Dashboard::IDashboardConnectionApplication>>::failure(
                nullApplicationError());
        }
        return Domain::Result<std::shared_ptr<
            Dashboard::IDashboardConnectionApplication>>::success(
            std::move(applicationOwner));
    }

    [[nodiscard]] Domain::Result<std::unique_ptr<DashboardRuntime>>
    createStartedRuntimeLocked(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application)
    {
        auto operationalState =
            std::make_shared<DashboardOperationalState>();
        auto created = DashboardRuntime::create(
            clock_, uuidGenerator_, std::move(operationalState));
        if (!created) {
            return Domain::Result<std::unique_ptr<DashboardRuntime>>::failure(
                std::move(created).error());
        }
        auto candidate = std::move(created).value();
        auto started = candidate->start(DashboardRuntimeBinding{
            config.dashboard, std::move(application)});
        if (!started) {
            auto transitionError = std::move(started).error();
            candidate->requestGracefulShutdown();
            auto cleanup = candidate->wait();
            if (!cleanup) {
                return Domain::Result<
                    std::unique_ptr<DashboardRuntime>>::failure(
                    incompleteCandidateCleanupError(
                        transitionError, cleanup.error()));
            }
            return Domain::Result<std::unique_ptr<DashboardRuntime>>::failure(
                std::move(transitionError));
        }

        setOperationalDesired(*candidate, operationalServiceDesired);
        return Domain::Result<std::unique_ptr<DashboardRuntime>>::success(
            std::move(candidate));
    }

    [[nodiscard]] Domain::Result<void> drainCurrentRuntimeLocked()
    {
        if (dashboardRuntime_ == nullptr) {
            return Domain::Result<void>::success();
        }

        dashboardRuntime_->pauseOperationalService();
        dashboardRuntime_->requestGracefulShutdown();
        auto waited = dashboardRuntime_->wait();
        if (!waited) {
            return waited;
        }
        dashboardRuntime_.reset();
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<bool> replaceSeriallyLocked(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired)
    {
        // Application construction is transport neutral. Validate it while
        // the healthy old listener still owns the endpoint so a factory
        // failure remains fully rollback-free.
        auto application = createApplicationLocked(config.dashboard);
        if (!application) {
            return Domain::Result<bool>::failure(
                std::move(application).error());
        }

        auto drained = drainCurrentRuntimeLocked();
        if (!drained) {
            return Domain::Result<bool>::failure(
                std::move(drained).error());
        }

        // The exclusive same endpoint is not offered to the successor until
        // the old graph has joined and its owner has been destroyed.
        auto candidate = createStartedRuntimeLocked(
            config,
            operationalServiceDesired,
            std::move(application).value());
        if (!candidate) {
            return Domain::Result<bool>::failure(
                std::move(candidate).error());
        }
        dashboardRuntime_ = std::move(candidate).value();
        return Domain::Result<bool>::success(true);
    }

    [[nodiscard]] Domain::Result<bool> rebindDistinctLocked(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired)
    {
        auto application = createApplicationLocked(config.dashboard);
        if (!application) {
            return Domain::Result<bool>::failure(
                std::move(application).error());
        }
        auto applicationOwner = std::move(application).value();

        dashboardRuntime_->pauseOperationalService();
        auto rebound = dashboardRuntime_->rebind(DashboardRuntimeBinding{
            config.dashboard, std::move(applicationOwner)});
        if (!rebound) {
            setOperationalDesired(
                *dashboardRuntime_, operationalServiceDesired);
            return Domain::Result<bool>::failure(
                std::move(rebound).error());
        }

        setOperationalDesired(*dashboardRuntime_, operationalServiceDesired);
        return Domain::Result<bool>::success(true);
    }

    [[nodiscard]] Domain::Result<bool> transitionLocked(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired,
        const TransitionKind kind)
    {
        auto observed = observeDashboardLocked();
        if (!observed) {
            return Domain::Result<bool>::failure(
                std::move(observed).error());
        }

        if (observed.value().listenerListening &&
            observed.value().configuration.has_value()) {
            const bool endpointMatches = sameEndpoint(
                *observed.value().configuration, config.dashboard);
            if (endpointMatches && kind != TransitionKind::ExplicitRebind) {
                setOperationalDesired(
                    *dashboardRuntime_, operationalServiceDesired);
                return Domain::Result<bool>::success(false);
            }
            if (!endpointMatches) {
                return rebindDistinctLocked(
                    config, operationalServiceDesired);
            }
        }

        return replaceSeriallyLocked(config, operationalServiceDesired);
    }

    static void setOperationalDesired(
        DashboardRuntime& runtime,
        const bool desired) noexcept
    {
        if (desired) {
            runtime.resumeOperationalService();
        } else {
            runtime.pauseOperationalService();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot>
    snapshotLocked()
    {
        auto observed = observeDashboardLocked();
        if (!observed) {
            return runtimeFailureLocked(std::move(observed).error());
        }
        return Domain::Result<Domain::ManagerRuntimeSnapshot>::success({
            observed.value().listenerListening,
            observed.value().operationalServiceActive,
            startedAt_,
            restartCount_,
            lastError_,
            shutdownRequested_});
    }

    [[nodiscard]] static Domain::Result<Domain::ManagerRuntimeSnapshot>
    snapshotFailure(Domain::Error error)
    {
        return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
            std::move(error));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot>
    runtimeFailureLocked(Domain::Error error)
    {
        lastError_ = error.message;
        return snapshotFailure(std::move(error));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot>
    exceptionFailure() noexcept
    {
        try {
            const std::lock_guard lock{operationMutex_};
            return runtimeFailureLocked(internalRuntimeError());
        } catch (...) {
            return snapshotFailure(internalRuntimeError());
        }
    }

    // One mutex is the sole operation/transition owner. It is deliberately
    // held through lower-runtime wait so no caller can observe or install a
    // successor between exclusive endpoint destruction and publication.
    mutable std::mutex operationMutex_;
    const std::shared_ptr<Contracts::IClock> clock_;
    const std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    const std::shared_ptr<
        Dashboard::IDashboardConnectionApplicationFactory>
        applicationFactory_;
    std::unique_ptr<DashboardRuntime> dashboardRuntime_;
    std::optional<Domain::UtcTimePoint> startedAt_;
    std::uint32_t restartCount_{};
    std::optional<std::string> lastError_;
    bool shutdownRequested_{};
    bool closed_{};
};

Domain::Result<std::unique_ptr<WindowsManagerRuntime>>
WindowsManagerRuntime::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Dashboard::IDashboardConnectionApplicationFactory>
        applicationFactory) noexcept
{
    using CreateResult =
        Domain::Result<std::unique_ptr<WindowsManagerRuntime>>;
    if (clock == nullptr || uuidGenerator == nullptr ||
        applicationFactory == nullptr) {
        return CreateResult::failure(invalidDependenciesError());
    }

    try {
        auto implementation = std::make_unique<Impl>(
            std::move(clock),
            std::move(uuidGenerator),
            std::move(applicationFactory));
        return CreateResult::success(
            std::unique_ptr<WindowsManagerRuntime>{
                new WindowsManagerRuntime{std::move(implementation)}});
    } catch (...) {
        return CreateResult::failure(internalRuntimeError());
    }
}

WindowsManagerRuntime::WindowsManagerRuntime(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsManagerRuntime::~WindowsManagerRuntime() noexcept
{
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

Domain::Result<Domain::ManagerRuntimeSnapshot> WindowsManagerRuntime::start(
    const Domain::AppConfig& config,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->start(config, context);
}

Domain::Result<Domain::ManagerRuntimeSnapshot> WindowsManagerRuntime::pause(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->pause(context);
}

Domain::Result<Domain::ManagerRuntimeSnapshot> WindowsManagerRuntime::rebind(
    const Domain::AppConfig& config,
    const bool operationalServiceDesired,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->rebind(
        config, operationalServiceDesired, context);
}

Domain::Result<Domain::ManagerRuntimeSnapshot>
WindowsManagerRuntime::reconcile(
    const Domain::AppConfig& config,
    const bool operationalServiceDesired,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->reconcile(
        config, operationalServiceDesired, context);
}

Domain::Result<Domain::ManagerRuntimeSnapshot>
WindowsManagerRuntime::applySettings(
    const Domain::AppConfig& config,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->applySettings(config, context);
}

Domain::Result<Domain::ManagerRuntimeSnapshot> WindowsManagerRuntime::snapshot(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->snapshot(context);
}

Domain::Result<Domain::ManagerRuntimeSnapshot>
WindowsManagerRuntime::requestShutdown(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->requestShutdown(context);
}

void WindowsManagerRuntime::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Hosts::Manager
