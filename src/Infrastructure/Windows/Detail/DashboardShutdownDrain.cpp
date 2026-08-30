#include "DashboardShutdownDrain.h"

#include "ForgeConductor/Domain/Error.h"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error shutdownError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error unavailableDependencyError()
{
    return shutdownError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard shutdown drain lost a process-owned dependency before exact teardown completed.");
}

[[nodiscard]] Domain::MonotonicTimePoint boundedDeadline(
    const Domain::MonotonicTimePoint now,
    const std::chrono::seconds lifetime) noexcept
{
    const auto duration = std::chrono::duration_cast<
        Domain::MonotonicTimePoint::duration>(lifetime);
    const auto maximum = (Domain::MonotonicTimePoint::max)();
    return now > maximum - duration ? maximum : now + duration;
}

void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

} // namespace

DashboardShutdownDrainHostSnapshot::DashboardShutdownDrainHostSnapshot(
    const bool dependenciesAvailable,
    const bool executorFullyDrained,
    const bool listenerRoutingDrained,
    const bool overloadFullyDrained,
    const std::size_t registeredConnectionCount,
    const std::size_t registeredAuxiliaryDeadlineTargetCount,
    const std::size_t fixedCompletionTargetCount,
    const std::uint64_t routingProgressRevision,
    const bool deadlineRoutingInProgress,
    const bool fatal) noexcept
    : dependenciesAvailable_{dependenciesAvailable},
      executorFullyDrained_{executorFullyDrained},
      listenerRoutingDrained_{listenerRoutingDrained},
      overloadFullyDrained_{overloadFullyDrained},
      registeredConnectionCount_{registeredConnectionCount},
      registeredAuxiliaryDeadlineTargetCount_{
          registeredAuxiliaryDeadlineTargetCount},
      fixedCompletionTargetCount_{fixedCompletionTargetCount},
      routingProgressRevision_{routingProgressRevision},
      deadlineRoutingInProgress_{deadlineRoutingInProgress},
      fatal_{fatal}
{
}

bool DashboardShutdownDrainHostSnapshot::dependenciesAvailable()
    const noexcept
{
    return dependenciesAvailable_;
}

bool DashboardShutdownDrainHostSnapshot::executorFullyDrained()
    const noexcept
{
    return executorFullyDrained_;
}

bool DashboardShutdownDrainHostSnapshot::listenerRoutingDrained()
    const noexcept
{
    return listenerRoutingDrained_;
}

bool DashboardShutdownDrainHostSnapshot::overloadFullyDrained()
    const noexcept
{
    return overloadFullyDrained_;
}

std::size_t DashboardShutdownDrainHostSnapshot::registeredConnectionCount()
    const noexcept
{
    return registeredConnectionCount_;
}

std::size_t DashboardShutdownDrainHostSnapshot::
registeredAuxiliaryDeadlineTargetCount() const noexcept
{
    return registeredAuxiliaryDeadlineTargetCount_;
}

std::size_t DashboardShutdownDrainHostSnapshot::fixedCompletionTargetCount()
    const noexcept
{
    return fixedCompletionTargetCount_;
}

std::uint64_t DashboardShutdownDrainHostSnapshot::routingProgressRevision()
    const noexcept
{
    return routingProgressRevision_;
}

bool DashboardShutdownDrainHostSnapshot::deadlineRoutingInProgress()
    const noexcept
{
    return deadlineRoutingInProgress_;
}

bool DashboardShutdownDrainHostSnapshot::fatal() const noexcept
{
    return fatal_;
}

void DashboardShutdownDrainProcessFailFast::failFast() noexcept
{
    std::terminate();
}

DashboardShutdownDrainHost::DashboardShutdownDrainHost(
    DashboardConnectionRuntimeServices& runtimeServices,
    WindowsDashboardHandlerExecutor& handlerExecutor,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardIocpWorkerKernel& kernel,
    std::shared_ptr<DashboardDeadlineIocpBridge> deadlineBridge,
    std::shared_ptr<DashboardListenerGenerationCoordinator>
        listenerCoordinator,
    std::shared_ptr<DashboardOverloadResponderSet> overloadResponders,
    std::shared_ptr<DashboardConnectionRegistry> connectionRegistry,
    std::shared_ptr<DashboardIocpCompletionRouter> completionRouter)
    noexcept
    : runtimeServices_{std::addressof(runtimeServices)},
      handlerExecutor_{std::addressof(handlerExecutor)},
      deadlineScheduler_{std::addressof(deadlineScheduler)},
      kernel_{std::addressof(kernel)},
      deadlineBridge_{std::move(deadlineBridge)},
      listenerCoordinator_{std::move(listenerCoordinator)},
      overloadResponders_{std::move(overloadResponders)},
      connectionRegistry_{std::move(connectionRegistry)},
      completionRouter_{std::move(completionRouter)}
{
}

Domain::Result<void>
DashboardShutdownDrainHost::bindBridgeFailureObserver(
    std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver> observer)
    noexcept
{
    const auto bridge = deadlineBridge_.lock();
    return bridge != nullptr
        ? bridge->bindFailureObserver(std::move(observer))
        : Domain::Result<void>::failure(unavailableDependencyError());
}

Domain::Result<void>
DashboardShutdownDrainHost::bindDeadlineSchedulerFailureObserver(
    std::weak_ptr<IWindowsDashboardDeadlineSchedulerFailureObserver>
        observer) noexcept
{
    return deadlineScheduler_->bindFailureObserver(std::move(observer));
}

Domain::Result<void>
DashboardShutdownDrainHost::bindHandlerDrainObserver(
    std::weak_ptr<IDashboardHandlerExecutorDrainObserver> observer)
    noexcept
{
    return handlerExecutor_->bindShutdownDrainObserver(
        std::move(observer));
}

Domain::Result<void>
DashboardShutdownDrainHost::bindListenerDrainObserver(
    std::weak_ptr<IDashboardListenerGenerationCoordinatorDrainObserver>
        observer) noexcept
{
    const auto coordinator = listenerCoordinator_.lock();
    return coordinator != nullptr
        ? coordinator->bindShutdownDrainObserver(std::move(observer))
        : Domain::Result<void>::failure(unavailableDependencyError());
}

Domain::Result<void>
DashboardShutdownDrainHost::bindRegistryConnectionDrainObserver(
    std::weak_ptr<IDashboardConnectionRegistryDrainObserver> observer)
    noexcept
{
    const auto registry = connectionRegistry_.lock();
    return registry != nullptr
        ? registry->bindShutdownDrainObserver(std::move(observer))
        : Domain::Result<void>::failure(unavailableDependencyError());
}

Domain::Result<void>
DashboardShutdownDrainHost::bindRegistryRoutingProgressObserver(
    std::weak_ptr<IDashboardConnectionRegistryRoutingProgressObserver>
        observer) noexcept
{
    const auto registry = connectionRegistry_.lock();
    return registry != nullptr
        ? registry->bindRoutingProgressObserver(std::move(observer))
        : Domain::Result<void>::failure(unavailableDependencyError());
}

Domain::Result<void>
DashboardShutdownDrainHost::bindOverloadDrainObserver(
    std::weak_ptr<IDashboardOverloadResponderSetDrainObserver> observer)
    noexcept
{
    const auto overload = overloadResponders_.lock();
    return overload != nullptr
        ? overload->bindProcessDrainObserver(std::move(observer))
        : Domain::Result<void>::failure(unavailableDependencyError());
}

Domain::Result<void>
DashboardShutdownDrainHost::registerShutdownDeadlineTarget(
    std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> target) noexcept
{
    const auto registry = connectionRegistry_.lock();
    return registry != nullptr
        ? registry->registerAuxiliaryDeadlineTarget(std::move(target))
        : Domain::Result<void>::failure(unavailableDependencyError());
}

Domain::MonotonicTimePoint DashboardShutdownDrainHost::monotonicNow()
    const noexcept
{
    return runtimeServices_->monotonicNow();
}

Domain::Result<WindowsDashboardDeadline>
DashboardShutdownDrainHost::scheduleShutdownDeadline(
    WindowsDashboardDeadlineRequest request) noexcept
{
    return deadlineScheduler_->schedule(std::move(request));
}

bool DashboardShutdownDrainHost::cancelShutdownDeadline(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence) noexcept
{
    return deadlineScheduler_->cancel(registrationId, armSequence);
}

void DashboardShutdownDrainHost::closeRuntimeAdmission() noexcept
{
    runtimeServices_->beginShutdown();
}

void DashboardShutdownDrainHost::closeHandlerAdmission() noexcept
{
    handlerExecutor_->beginShutdown();
}

void DashboardShutdownDrainHost::beginGracefulListenerShutdown() noexcept
{
    if (const auto coordinator = listenerCoordinator_.lock()) {
        coordinator->beginGracefulShutdown();
    }
}

void DashboardShutdownDrainHost::beginOverloadShutdown() noexcept
{
    if (const auto overload = overloadResponders_.lock()) {
        overload->beginShutdown();
    }
}

void DashboardShutdownDrainHost::beginGracefulRegistryShutdown() noexcept
{
    if (const auto registry = connectionRegistry_.lock()) {
        registry->beginGracefulShutdown();
    }
}

void DashboardShutdownDrainHost::beginHardListenerShutdown() noexcept
{
    if (const auto coordinator = listenerCoordinator_.lock()) {
        coordinator->beginShutdown();
    }
}

void DashboardShutdownDrainHost::beginHardRegistryShutdown() noexcept
{
    if (const auto registry = connectionRegistry_.lock()) {
        registry->beginShutdown();
    }
}

void DashboardShutdownDrainHost::beginCompletionRouterShutdown() noexcept
{
    if (const auto router = completionRouter_.lock()) {
        router->beginShutdown();
    }
}

void DashboardShutdownDrainHost::joinHandlerExecutor() noexcept
{
    handlerExecutor_->shutdown();
}

DashboardShutdownDrainHostSnapshot DashboardShutdownDrainHost::snapshot()
    const noexcept
{
    const auto bridge = deadlineBridge_.lock();
    const auto coordinator = listenerCoordinator_.lock();
    const auto overload = overloadResponders_.lock();
    const auto registry = connectionRegistry_.lock();
    const auto router = completionRouter_.lock();
    if (bridge == nullptr || coordinator == nullptr || overload == nullptr ||
        registry == nullptr || router == nullptr) {
        return DashboardShutdownDrainHostSnapshot{
            false, false, false, false, 0U, 0U, 0U, 0U, false, true};
    }

    const auto listenerSnapshot = coordinator->snapshot();
    const auto overloadSnapshot = overload->snapshot();
    const auto registrySnapshot = registry->snapshot();
    const auto routerSnapshot = router->snapshot();
    const auto bridgeSnapshot = bridge->snapshot();
    const auto schedulerSnapshot = deadlineScheduler_->snapshot();
    const bool listenersDrained =
        listenerSnapshot.shutdownRequested() &&
        !listenerSnapshot.activeRegistrationId().has_value() &&
        !listenerSnapshot.retiringRegistrationId().has_value() &&
        !listenerSnapshot.preparationInProgress() &&
        !listenerSnapshot.collectionInProgress();
    const bool overloadFatal =
        overloadSnapshot.lifecycleFailure() != nullptr ||
        overloadSnapshot.managedSourcePublicationFailed() ||
        overloadSnapshot.processDrainPublicationFailed();
    const bool fatal = listenerSnapshot.fatal() ||
        listenerSnapshot.hasFailure() || overloadFatal ||
        registrySnapshot.isFatal() || bridgeSnapshot.isFatal() ||
        schedulerSnapshot.failure() != nullptr ||
        routerSnapshot.fatalNativeError().has_value();
    return DashboardShutdownDrainHostSnapshot{
        true,
        handlerExecutor_->fullyDrained(),
        listenersDrained,
        overloadSnapshot.fullyDrained(),
        registrySnapshot.registeredConnectionCount(),
        registrySnapshot.registeredAuxiliaryDeadlineTargetCount(),
        routerSnapshot.fixedTargetCount(),
        registrySnapshot.routingProgressRevision(),
        registrySnapshot.deadlineRoutingInProgress(),
        fatal};
}

bool DashboardShutdownDrainHost::unregisterOverloadDeadlineTarget()
    noexcept
{
    const auto overload = overloadResponders_.lock();
    const auto registry = connectionRegistry_.lock();
    return overload != nullptr && registry != nullptr &&
        registry->unregisterAuxiliaryDeadlineTarget(overload);
}

bool DashboardShutdownDrainHost::unregisterOverloadCompletionTarget()
    noexcept
{
    const auto overload = overloadResponders_.lock();
    const auto router = completionRouter_.lock();
    return overload != nullptr && router != nullptr &&
        router->unregisterFixedTarget(overload);
}

bool DashboardShutdownDrainHost::unregisterShutdownDeadlineTarget(
    const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
    noexcept
{
    const auto registry = connectionRegistry_.lock();
    return registry != nullptr &&
        registry->unregisterAuxiliaryDeadlineTarget(target);
}

void DashboardShutdownDrainHost::shutdownDeadlineScheduler() noexcept
{
    deadlineScheduler_->shutdown();
}

Domain::Result<DashboardDeadlineRoutingFinalizeDisposition>
DashboardShutdownDrainHost::finalizeDeadlineRouting() noexcept
{
    const auto registry = connectionRegistry_.lock();
    return registry != nullptr
        ? registry->finalizeDeadlineRouting()
        : Domain::Result<DashboardDeadlineRoutingFinalizeDisposition>::
              failure(unavailableDependencyError());
}

void DashboardShutdownDrainHost::shutdownIocpKernel() noexcept
{
    kernel_->shutdown();
}

DashboardShutdownDrainSnapshot::DashboardShutdownDrainSnapshot(
    const DashboardShutdownDrainLifecycle lifecycle,
    const bool installed,
    const bool gracefulShutdownRequested,
    const bool hardShutdownRequested,
    const bool handlerDrained,
    const bool listenersDrained,
    const bool overloadDrained,
    const bool registryConnectionsDrained,
    const bool executorFinalizerReturned,
    const std::uint64_t routingProgressRevision,
    const std::uint64_t staleDeadlineCount,
    const std::uint64_t hardEscalationCount,
    const std::uint64_t failFastCount,
    std::optional<WindowsDashboardDeadline> currentDeadline,
    std::optional<DashboardDeadlineIocpFailure> bridgeFailure,
    std::optional<WindowsDashboardDeadlineSchedulerFailure>
        deadlineSchedulerFailure) noexcept
    : lifecycle_{lifecycle},
      installed_{installed},
      gracefulShutdownRequested_{gracefulShutdownRequested},
      hardShutdownRequested_{hardShutdownRequested},
      handlerDrained_{handlerDrained},
      listenersDrained_{listenersDrained},
      overloadDrained_{overloadDrained},
      registryConnectionsDrained_{registryConnectionsDrained},
      executorFinalizerReturned_{executorFinalizerReturned},
      routingProgressRevision_{routingProgressRevision},
      staleDeadlineCount_{staleDeadlineCount},
      hardEscalationCount_{hardEscalationCount},
      failFastCount_{failFastCount},
      currentDeadline_{std::move(currentDeadline)},
      bridgeFailure_{std::move(bridgeFailure)},
      deadlineSchedulerFailure_{std::move(deadlineSchedulerFailure)}
{
}

DashboardShutdownDrainLifecycle DashboardShutdownDrainSnapshot::lifecycle()
    const noexcept
{
    return lifecycle_;
}

bool DashboardShutdownDrainSnapshot::installed() const noexcept
{
    return installed_;
}

bool DashboardShutdownDrainSnapshot::gracefulShutdownRequested()
    const noexcept
{
    return gracefulShutdownRequested_;
}

bool DashboardShutdownDrainSnapshot::hardShutdownRequested()
    const noexcept
{
    return hardShutdownRequested_;
}

bool DashboardShutdownDrainSnapshot::handlerDrained() const noexcept
{
    return handlerDrained_;
}

bool DashboardShutdownDrainSnapshot::listenersDrained() const noexcept
{
    return listenersDrained_;
}

bool DashboardShutdownDrainSnapshot::overloadDrained() const noexcept
{
    return overloadDrained_;
}

bool DashboardShutdownDrainSnapshot::registryConnectionsDrained()
    const noexcept
{
    return registryConnectionsDrained_;
}

bool DashboardShutdownDrainSnapshot::executorFinalizerReturned()
    const noexcept
{
    return executorFinalizerReturned_;
}

std::uint64_t DashboardShutdownDrainSnapshot::routingProgressRevision()
    const noexcept
{
    return routingProgressRevision_;
}

std::uint64_t DashboardShutdownDrainSnapshot::staleDeadlineCount()
    const noexcept
{
    return staleDeadlineCount_;
}

std::uint64_t DashboardShutdownDrainSnapshot::hardEscalationCount()
    const noexcept
{
    return hardEscalationCount_;
}

std::uint64_t DashboardShutdownDrainSnapshot::failFastCount()
    const noexcept
{
    return failFastCount_;
}

const WindowsDashboardDeadline*
DashboardShutdownDrainSnapshot::currentDeadline() const noexcept
{
    return currentDeadline_.has_value()
        ? std::addressof(*currentDeadline_)
        : nullptr;
}

const DashboardDeadlineIocpFailure*
DashboardShutdownDrainSnapshot::bridgeFailure() const noexcept
{
    return bridgeFailure_.has_value()
        ? std::addressof(*bridgeFailure_)
        : nullptr;
}

const WindowsDashboardDeadlineSchedulerFailure*
DashboardShutdownDrainSnapshot::deadlineSchedulerFailure() const noexcept
{
    return deadlineSchedulerFailure_.has_value()
        ? std::addressof(*deadlineSchedulerFailure_)
        : nullptr;
}

DashboardShutdownDrain::DashboardShutdownDrain(
    const std::uint64_t registrationId,
    std::shared_ptr<IDashboardShutdownDrainHost> host,
    std::shared_ptr<IDashboardShutdownDrainFailFast> failFast) noexcept
    : registrationId_{registrationId},
      host_{std::move(host)},
      failFast_{std::move(failFast)}
{
}

Domain::Result<std::shared_ptr<DashboardShutdownDrain>>
DashboardShutdownDrain::create(
    const std::uint64_t registrationId,
    std::shared_ptr<IDashboardShutdownDrainHost> host,
    std::shared_ptr<IDashboardShutdownDrainFailFast> failFast) noexcept
{
    using CreateResult =
        Domain::Result<std::shared_ptr<DashboardShutdownDrain>>;
    if (registrationId == 0U || host == nullptr || failFast == nullptr) {
        return CreateResult::failure(shutdownError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard shutdown drain requires a nonzero identity, host, and fail-fast boundary."));
    }

    try {
        auto owner = std::shared_ptr<DashboardShutdownDrain>{
            new DashboardShutdownDrain{
                registrationId, std::move(host), std::move(failFast)}};
        owner->driver_ = std::thread{
            [instance = owner.get()]() noexcept { instance->driverMain(); }};
        {
            std::unique_lock lock{owner->mutex_};
            const bool ready = owner->stateChanged_.wait_for(
                lock,
                DriverStartupTimeout,
                [&owner] { return owner->driverReady_; });
            if (!ready) {
                owner->exitRequested_ = true;
                incrementSaturating(owner->wakeRevision_);
                lock.unlock();
                owner->stateChanged_.notify_all();
                if (owner->driver_.joinable()) {
                    owner->driver_.join();
                }
                return CreateResult::failure(shutdownError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The dashboard shutdown drain driver did not reach its bounded ready barrier."));
            }
        }
        return CreateResult::success(std::move(owner));
    } catch (...) {
        return CreateResult::failure(shutdownError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard shutdown drain driver could not be created safely."));
    }
}

DashboardShutdownDrain::~DashboardShutdownDrain() noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!driverExited_) {
                if (installed_) {
                    hardShutdownRequested_ = true;
                } else {
                    exitRequested_ = true;
                }
                incrementSaturating(wakeRevision_);
            }
        }
        stateChanged_.notify_all();
        if (driver_.joinable()) {
            if (driver_.get_id() == std::this_thread::get_id()) {
                std::terminate();
            }
            driver_.join();
        }
        if (executorFinalizer_.joinable()) {
            executorFinalizer_.join();
        }
    } catch (...) {
        std::terminate();
    }
}

Domain::Result<void> DashboardShutdownDrain::install() noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (installed_ || installing_) {
                return Domain::Result<void>::failure(shutdownError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard shutdown drain installation is one-shot."));
            }
            if (lifecycle_ != DashboardShutdownDrainLifecycle::Registered ||
                driverExited_) {
                return Domain::Result<void>::failure(shutdownError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard shutdown drain is closed to installation."));
            }
            installing_ = true;
        }

        const auto owner = shared_from_this();
        auto bound = host_->bindBridgeFailureObserver(
            std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->bindDeadlineSchedulerFailureObserver(
            std::weak_ptr<
                IWindowsDashboardDeadlineSchedulerFailureObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->bindHandlerDrainObserver(
            std::weak_ptr<IDashboardHandlerExecutorDrainObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->bindListenerDrainObserver(std::weak_ptr<
            IDashboardListenerGenerationCoordinatorDrainObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->bindRegistryConnectionDrainObserver(std::weak_ptr<
            IDashboardConnectionRegistryDrainObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->bindRegistryRoutingProgressObserver(std::weak_ptr<
            IDashboardConnectionRegistryRoutingProgressObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->bindOverloadDrainObserver(
            std::weak_ptr<IDashboardOverloadResponderSetDrainObserver>{owner});
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        bound = host_->registerShutdownDeadlineTarget(
            std::static_pointer_cast<IDashboardAuxiliaryDeadlineTarget>(owner));
        if (!bound) {
            return failInstallation(std::move(bound).error());
        }
        {
            const std::scoped_lock lock{mutex_};
            shutdownDeadlineTargetRegistered_ = true;
        }

        bool failedDuringInstall{};
        {
            const std::scoped_lock lock{mutex_};
            if (lifecycle_ == DashboardShutdownDrainLifecycle::Fatal) {
                installing_ = false;
                failedDuringInstall = true;
            } else {
                installing_ = false;
                installed_ = true;
            }
        }
        if (failedDuringInstall) {
            return failInstallation(shutdownError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard shutdown drain failed while its managed observers were being installed."));
        }
        stateChanged_.notify_all();
        return Domain::Result<void>::success();
    } catch (...) {
        return failInstallation(shutdownError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard shutdown drain could not install its exact process observers safely."));
    }
}

Domain::Result<void> DashboardShutdownDrain::failInstallation(
    Domain::Error error) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        if (!firstFailure_.has_value()) {
            firstFailure_.emplace(error);
        }
        lifecycle_ = DashboardShutdownDrainLifecycle::Fatal;
        hardShutdownRequested_ = true;
        installing_ = false;
        incrementSaturating(wakeRevision_);
        lock.unlock();
        stateChanged_.notify_all();
        lock.lock();
        stateChanged_.wait(lock, [this] {
            return driverExited_;
        });
    } catch (...) {
        std::terminate();
    }
    return Domain::Result<void>::failure(std::move(error));
}

void DashboardShutdownDrain::requestGracefulShutdown() noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!installed_ || driverExited_ ||
                lifecycle_ == DashboardShutdownDrainLifecycle::Drained ||
                lifecycle_ == DashboardShutdownDrainLifecycle::Fatal) {
                return;
            }
            if (!gracefulShutdownRequested_) {
                gracefulShutdownRequested_ = true;
                if (!hardShutdownRequested_) {
                    lifecycle_ = DashboardShutdownDrainLifecycle::Arming;
                }
                incrementSaturating(wakeRevision_);
            }
        }
        stateChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::beginShutdown() noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (driverExited_ ||
                lifecycle_ == DashboardShutdownDrainLifecycle::Drained ||
                lifecycle_ ==
                    DashboardShutdownDrainLifecycle::RoutingReleasePending) {
                return;
            }
            hardShutdownRequested_ = true;
            incrementSaturating(wakeRevision_);
        }
        stateChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::dispatchDeadline(
    const WindowsDashboardDeadline deadline) noexcept
{
    try {
        bool wake{};
        {
            const std::scoped_lock lock{mutex_};
            if (deadline.registrationId != registrationId_ ||
                deadline.kind != WindowsDashboardDeadlineKind::ShutdownDrain) {
                incrementSaturating(staleDeadlineCount_);
                return;
            }
            if (currentDeadline_.has_value() &&
                deadline == *currentDeadline_) {
                currentDeadline_.reset();
                hardShutdownRequested_ = true;
                incrementSaturating(wakeRevision_);
                wake = true;
            } else if (lifecycle_ ==
                           DashboardShutdownDrainLifecycle::Arming &&
                       !earlyDeadline_.has_value()) {
                earlyDeadline_.emplace(deadline);
                incrementSaturating(wakeRevision_);
                wake = true;
            } else {
                incrementSaturating(staleDeadlineCount_);
            }
        }
        if (wake) {
            stateChanged_.notify_all();
        }
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::latchSimpleDrainEdge(bool& edge) noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!edge) {
                edge = true;
                incrementSaturating(wakeRevision_);
            }
        }
        stateChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::handlerExecutorMayHaveDrained() noexcept
{
    latchSimpleDrainEdge(handlerDrained_);
}

void DashboardShutdownDrain::listenerGenerationsMayHaveDrained() noexcept
{
    latchSimpleDrainEdge(listenersDrained_);
}

void DashboardShutdownDrain::overloadRespondersMayHaveDrained() noexcept
{
    latchSimpleDrainEdge(overloadDrained_);
}

void DashboardShutdownDrain::registryConnectionsMayHaveDrained() noexcept
{
    latchSimpleDrainEdge(registryConnectionsDrained_);
}

void DashboardShutdownDrain::registryRoutingMayHaveProgressed(
    const std::uint64_t revision) noexcept
{
    try {
        bool wake{};
        {
            const std::scoped_lock lock{mutex_};
            if (revision > routingProgressRevision_) {
                routingProgressRevision_ = revision;
                incrementSaturating(routingWakeRevision_);
                incrementSaturating(wakeRevision_);
                wake = true;
            }
        }
        if (wake) {
            stateChanged_.notify_all();
        }
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::dashboardDeadlineIocpBridgeFailed(
    const DashboardDeadlineIocpFailure failure) noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!bridgeFailure_.has_value()) {
                bridgeFailure_.emplace(failure);
            }
            lifecycle_ = DashboardShutdownDrainLifecycle::Fatal;
            hardShutdownRequested_ = true;
            incrementSaturating(wakeRevision_);
        }
        stateChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::dashboardDeadlineSchedulerFailed(
    const WindowsDashboardDeadlineSchedulerFailure failure) noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!deadlineSchedulerFailure_.has_value()) {
                deadlineSchedulerFailure_.emplace(failure);
            }
            lifecycle_ = DashboardShutdownDrainLifecycle::Fatal;
            hardShutdownRequested_ = true;
            incrementSaturating(wakeRevision_);
        }
        stateChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::retainFatal(
    Domain::Error error,
    const bool requestHard) noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!firstFailure_.has_value()) {
                firstFailure_.emplace(std::move(error));
            }
            lifecycle_ = DashboardShutdownDrainLifecycle::Fatal;
            hardShutdownRequested_ = hardShutdownRequested_ || requestHard;
            incrementSaturating(wakeRevision_);
        }
        stateChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardShutdownDrain::retainFixedFatal(
    const bool requestHard) noexcept
{
    try {
        retainFatal(shutdownError(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard shutdown drain observed a structural process-lifecycle failure."),
            requestHard);
    } catch (...) {
        {
            const std::scoped_lock lock{mutex_};
            lifecycle_ = DashboardShutdownDrainLifecycle::Fatal;
            hardShutdownRequested_ = hardShutdownRequested_ || requestHard;
            incrementSaturating(wakeRevision_);
        }
        stateChanged_.notify_all();
    }
}

bool DashboardShutdownDrain::runGracefulCutoff() noexcept
{
    auto scheduled = host_->scheduleShutdownDeadline(
        WindowsDashboardDeadlineRequest{
            registrationId_,
            WindowsDashboardDeadlineKind::ShutdownDrain,
            boundedDeadline(host_->monotonicNow(), GraceLifetime)});
    if (!scheduled) {
        retainFatal(std::move(scheduled).error(), true);
        return false;
    }

    const auto armed = std::move(scheduled).value();
    bool armMismatch{};
    {
        const std::scoped_lock lock{mutex_};
        if (earlyDeadline_.has_value()) {
            if (*earlyDeadline_ == armed) {
                hardShutdownRequested_ = true;
            } else {
                armMismatch = true;
            }
            earlyDeadline_.reset();
        } else {
            currentDeadline_.emplace(armed);
        }
    }
    if (armMismatch) {
        retainFixedFatal(true);
        return false;
    }

    host_->closeRuntimeAdmission();
    host_->closeHandlerAdmission();
    host_->beginGracefulListenerShutdown();
    host_->beginOverloadShutdown();
    host_->beginGracefulRegistryShutdown();
    {
        const std::scoped_lock lock{mutex_};
        gracefulCutoffCompleted_ = true;
        if (lifecycle_ != DashboardShutdownDrainLifecycle::Fatal) {
            lifecycle_ = hardShutdownRequested_
                ? DashboardShutdownDrainLifecycle::HardEscalated
                : DashboardShutdownDrainLifecycle::Draining;
        }
    }
    if (!startExecutorFinalizer()) {
        retainFixedFatal(true);
        return false;
    }
    return true;
}

void DashboardShutdownDrain::runHardFanout() noexcept
{
    {
        const std::scoped_lock lock{mutex_};
        if (hardFanoutCompleted_ || hardFanoutClaimed_) {
            return;
        }
        hardFanoutClaimed_ = true;
        hardShutdownRequested_ = true;
        if (lifecycle_ != DashboardShutdownDrainLifecycle::Fatal) {
            lifecycle_ = DashboardShutdownDrainLifecycle::HardEscalated;
        }
        incrementSaturating(hardEscalationCount_);
    }

    host_->closeRuntimeAdmission();
    host_->closeHandlerAdmission();
    host_->beginHardListenerShutdown();
    host_->beginOverloadShutdown();
    host_->beginHardRegistryShutdown();
    host_->beginCompletionRouterShutdown();

    {
        const std::scoped_lock lock{mutex_};
        completionRouterShutdownStarted_ = true;
        hardFanoutCompleted_ = true;
        incrementSaturating(wakeRevision_);
    }
    stateChanged_.notify_all();
}

bool DashboardShutdownDrain::startExecutorFinalizer() noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (executorFinalizerStarted_) {
                return true;
            }
            executorFinalizerStarted_ = true;
        }
        executorFinalizer_ = std::thread{[this]() noexcept {
            host_->joinHandlerExecutor();
            {
                const std::scoped_lock lock{mutex_};
                executorFinalizerReturned_ = true;
                incrementSaturating(wakeRevision_);
            }
            stateChanged_.notify_all();
        }};
        return true;
    } catch (...) {
        const std::scoped_lock lock{mutex_};
        executorFinalizerStarted_ = false;
        return false;
    }
}

void DashboardShutdownDrain::joinExecutorFinalizer() noexcept
{
    try {
        if (executorFinalizer_.joinable()) {
            if (executorFinalizer_.get_id() == std::this_thread::get_id()) {
                std::terminate();
            }
            executorFinalizer_.join();
        }
        const std::scoped_lock lock{mutex_};
        if (executorFinalizerStarted_) {
            executorFinalizerJoined_ = true;
        }
    } catch (...) {
        std::terminate();
    }
}

bool DashboardShutdownDrain::logicalDrainReady(
    const DashboardShutdownDrainHostSnapshot& hostSnapshot) const noexcept
{
    return handlerDrained_ && listenersDrained_ && overloadDrained_ &&
        registryConnectionsDrained_ && executorFinalizerReturned_ &&
        hostSnapshot.executorFullyDrained() &&
        hostSnapshot.listenerRoutingDrained() &&
        hostSnapshot.overloadFullyDrained() &&
        hostSnapshot.registeredConnectionCount() == 0U;
}

void DashboardShutdownDrain::invokeFailFastOnce() noexcept
{
    bool invoke{};
    {
        const std::scoped_lock lock{mutex_};
        if (!failFastInvoked_) {
            failFastInvoked_ = true;
            incrementSaturating(failFastCount_);
            invoke = true;
        }
    }
    if (invoke) {
        failFast_->failFast();
    }
}

void DashboardShutdownDrain::driverMain() noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            driverThreadId_ = std::this_thread::get_id();
            driverReady_ = true;
        }
        stateChanged_.notify_all();

        {
            std::unique_lock lock{mutex_};
            stateChanged_.wait(lock, [this] {
                return exitRequested_ ||
                    (!installing_ &&
                     (gracefulShutdownRequested_ ||
                      hardShutdownRequested_ ||
                      lifecycle_ == DashboardShutdownDrainLifecycle::Fatal));
            });
            if (exitRequested_) {
                lock.unlock();
                finishDriver();
                return;
            }
        }

        bool fatal{};
        bool graceful{};
        bool hard{};
        {
            const std::scoped_lock lock{mutex_};
            fatal = lifecycle_ == DashboardShutdownDrainLifecycle::Fatal;
            graceful = gracefulShutdownRequested_;
            hard = hardShutdownRequested_;
        }
        if (fatal) {
            finishFatalTerminal();
            return;
        }

        if (hard) {
            runHardFanout();
            if (!startExecutorFinalizer()) {
                retainFixedFatal(true);
                finishFatalTerminal();
                return;
            }
        } else if (graceful) {
            if (!runGracefulCutoff()) {
                finishFatalTerminal();
                return;
            }
        } else {
            retainFixedFatal(true);
            finishFatalTerminal();
            return;
        }

        for (;;) {
            bool runHard{};
            std::uint64_t baseline{};
            {
                const std::scoped_lock lock{mutex_};
                if (lifecycle_ == DashboardShutdownDrainLifecycle::Fatal) {
                    runHard = !hardFanoutCompleted_;
                } else {
                    runHard = hardShutdownRequested_ &&
                        !hardFanoutCompleted_;
                }
                baseline = wakeRevision_;
            }
            if (runHard) {
                runHardFanout();
            }

            const auto hostSnapshot = host_->snapshot();
            if (!hostSnapshot.dependenciesAvailable() ||
                hostSnapshot.fatal()) {
                retainFixedFatal(true);
            }

            bool ready{};
            bool nowFatal{};
            {
                const std::scoped_lock lock{mutex_};
                nowFatal = lifecycle_ ==
                    DashboardShutdownDrainLifecycle::Fatal;
                const bool hardMustWin = hardShutdownRequested_ &&
                    !hardFanoutCompleted_;
                ready = !nowFatal && !hardMustWin &&
                    logicalDrainReady(hostSnapshot);
                if (ready) {
                    if (currentDeadline_.has_value()) {
                        finalizationCancellation_.emplace(
                            *currentDeadline_);
                        currentDeadline_.reset();
                    }
                    lifecycle_ =
                        DashboardShutdownDrainLifecycle::RoutingReleasePending;
                }
            }
            if (nowFatal) {
                finishFatalTerminal();
                return;
            }
            if (ready) {
                if (!runFinalTeardown()) {
                    finishFatalTerminal();
                    return;
                }
                finishDriver();
                return;
            }

            std::unique_lock lock{mutex_};
            stateChanged_.wait(lock, [this, baseline] {
                return wakeRevision_ != baseline || exitRequested_;
            });
            if (exitRequested_) {
                lock.unlock();
                finishDriver();
                return;
            }
        }
    } catch (...) {
        retainFixedFatal(true);
        finishFatalTerminal();
    }
}

void DashboardShutdownDrain::finishFatalTerminal() noexcept
{
    // A fatal edge can race the post-kernel health check. Never fan process
    // owners back out after the exact platform teardown already stopped IOCP.
    if (!iocpKernelShutdownCompleted_) {
        runHardFanout();
    }
    const bool finalizerStarted = startExecutorFinalizer();
    invokeFailFastOnce();

    // Production fail-fast never returns. A returning verification seam must
    // still synchronously own executor shutdown when thread creation failed.
    if (!finalizerStarted) {
        host_->joinHandlerExecutor();
        {
            const std::scoped_lock lock{mutex_};
            executorFinalizerStarted_ = true;
            executorFinalizerReturned_ = true;
            executorFinalizerJoined_ = true;
            incrementSaturating(wakeRevision_);
        }
        stateChanged_.notify_all();
    } else {
        joinExecutorFinalizer();
    }

    // A partially installed observer graph is process-terminal. Returning
    // seams may complete only when authoritative host state proves the hard
    // fanout and executor join released every live runtime owner.
    for (;;) {
        const auto hostSnapshot = host_->snapshot();
        bool executorReturned{};
        std::uint64_t baseline{};
        {
            const std::scoped_lock lock{mutex_};
            executorReturned = executorFinalizerReturned_;
            baseline = wakeRevision_;
        }
        if (hostSnapshot.dependenciesAvailable() && executorReturned &&
            hostSnapshot.executorFullyDrained() &&
            hostSnapshot.listenerRoutingDrained() &&
            hostSnapshot.overloadFullyDrained() &&
            hostSnapshot.registeredConnectionCount() == 0U) {
            break;
        }
        std::unique_lock lock{mutex_};
        stateChanged_.wait(lock, [this, baseline] {
            return wakeRevision_ != baseline;
        });
    }

    if (!runFatalTerminalTeardown()) {
        // Once fail-fast returned, releasing a borrowed dependency or a live
        // routing owner would be unsafe. This is an intentional process-
        // terminal barrier; production has already terminated at failFast().
        std::unique_lock lock{mutex_};
        stateChanged_.wait(lock, [] { return false; });
    }
    finishDriver();
}

bool DashboardShutdownDrain::runFinalTeardown() noexcept
{
    return runTerminalTeardown(false);
}

bool DashboardShutdownDrain::runFatalTerminalTeardown() noexcept
{
    return runTerminalTeardown(true);
}

bool DashboardShutdownDrain::runTerminalTeardown(
    const bool preserveFatal) noexcept
{
    try {
        joinExecutorFinalizer();

        std::optional<WindowsDashboardDeadline> cancelled;
        {
            const std::scoped_lock lock{mutex_};
            if (finalizationCancellation_.has_value()) {
                cancelled.emplace(*finalizationCancellation_);
                finalizationCancellation_.reset();
            } else if (currentDeadline_.has_value()) {
                cancelled.emplace(*currentDeadline_);
                currentDeadline_.reset();
            }
        }
        if (cancelled.has_value()) {
            static_cast<void>(host_->cancelShutdownDeadline(
                cancelled->registrationId, cancelled->armSequence));
        }

        if (!overloadDeadlineTargetRemoved_) {
            if (!host_->unregisterOverloadDeadlineTarget()) {
                retainFixedFatal(true);
                return false;
            }
            overloadDeadlineTargetRemoved_ = true;
        }
        if (!overloadCompletionTargetRemoved_) {
            if (!host_->unregisterOverloadCompletionTarget()) {
                retainFixedFatal(true);
                return false;
            }
            overloadCompletionTargetRemoved_ = true;
        }
        bool selfRegistered{};
        {
            const std::scoped_lock lock{mutex_};
            selfRegistered = shutdownDeadlineTargetRegistered_;
        }
        if (selfRegistered) {
            const auto self = std::static_pointer_cast<
                IDashboardAuxiliaryDeadlineTarget>(shared_from_this());
            if (!host_->unregisterShutdownDeadlineTarget(self)) {
                retainFixedFatal(true);
                return false;
            }
            const std::scoped_lock lock{mutex_};
            shutdownDeadlineTargetRegistered_ = false;
        }
        if (!completionRouterShutdownStarted_) {
            host_->beginCompletionRouterShutdown();
            completionRouterShutdownStarted_ = true;
        }

        const auto routesRemoved = host_->snapshot();
        if (!routesRemoved.dependenciesAvailable() ||
            (!preserveFatal && routesRemoved.fatal()) ||
            routesRemoved.registeredConnectionCount() != 0U ||
            routesRemoved.registeredAuxiliaryDeadlineTargetCount() != 0U ||
            routesRemoved.fixedCompletionTargetCount() != 0U) {
            retainFixedFatal(true);
            return false;
        }

        if (!deadlineSchedulerShutdownCompleted_) {
            host_->shutdownDeadlineScheduler();
            deadlineSchedulerShutdownCompleted_ = true;
        }
        if (!deadlineRoutingFinalized_) {
            if (!finalizeDeadlineRouting(preserveFatal)) {
                return false;
            }
            deadlineRoutingFinalized_ = true;
        }
        if (!iocpKernelShutdownCompleted_) {
            host_->shutdownIocpKernel();
            iocpKernelShutdownCompleted_ = true;
        }
        const auto afterKernel = host_->snapshot();
        if (!afterKernel.dependenciesAvailable() ||
            (!preserveFatal && afterKernel.fatal())) {
            retainFixedFatal(true);
            return false;
        }
        bool fatalAfterKernel{};
        {
            const std::scoped_lock lock{mutex_};
            fatalAfterKernel =
                lifecycle_ == DashboardShutdownDrainLifecycle::Fatal;
            if (!preserveFatal && !fatalAfterKernel) {
                lifecycle_ = DashboardShutdownDrainLifecycle::Drained;
                incrementSaturating(wakeRevision_);
            }
            terminalTeardownCompleted_ = true;
        }
        if (!preserveFatal && fatalAfterKernel) {
            return false;
        }
        stateChanged_.notify_all();
        return true;
    } catch (...) {
        retainFixedFatal(true);
        return false;
    }
}

bool DashboardShutdownDrain::finalizeDeadlineRouting(
    const bool allowFatal) noexcept
{
    for (;;) {
        std::uint64_t baselineRoutingWake{};
        {
            const std::scoped_lock lock{mutex_};
            baselineRoutingWake = routingWakeRevision_;
            if (!allowFatal &&
                lifecycle_ == DashboardShutdownDrainLifecycle::Fatal) {
                return false;
            }
        }

        const auto hostSnapshot = host_->snapshot();
        if (!hostSnapshot.dependenciesAvailable() ||
            (!allowFatal && hostSnapshot.fatal())) {
            retainFixedFatal(true);
            return false;
        }
        auto finalized = host_->finalizeDeadlineRouting();
        if (!finalized) {
            retainFatal(std::move(finalized).error(), true);
            return false;
        }
        if (std::move(finalized).value() ==
            DashboardDeadlineRoutingFinalizeDisposition::Finalized) {
            return true;
        }

        std::unique_lock lock{mutex_};
        if (routingWakeRevision_ != baselineRoutingWake ||
            routingProgressRevision_ >
                hostSnapshot.routingProgressRevision()) {
            continue;
        }
        stateChanged_.wait(lock, [this, baselineRoutingWake, allowFatal] {
            return routingWakeRevision_ != baselineRoutingWake ||
                (!allowFatal &&
                 lifecycle_ == DashboardShutdownDrainLifecycle::Fatal);
        });
        if (!allowFatal &&
            lifecycle_ == DashboardShutdownDrainLifecycle::Fatal) {
            return false;
        }
    }
}

void DashboardShutdownDrain::finishDriver() noexcept
{
    joinExecutorFinalizer();
    {
        const std::scoped_lock lock{mutex_};
        driverExited_ = true;
        incrementSaturating(wakeRevision_);
    }
    stateChanged_.notify_all();
}

Domain::Result<void> DashboardShutdownDrain::wait() noexcept
{
    try {
        if (driver_.joinable()) {
            if (driver_.get_id() == std::this_thread::get_id()) {
                return Domain::Result<void>::failure(shutdownError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard shutdown drain driver cannot join itself."));
            }
            driver_.join();
        }
        joinExecutorFinalizer();
        const std::scoped_lock lock{mutex_};
        if (lifecycle_ == DashboardShutdownDrainLifecycle::Drained) {
            return Domain::Result<void>::success();
        }
        if (firstFailure_.has_value()) {
            return Domain::Result<void>::failure(*firstFailure_);
        }
        return Domain::Result<void>::failure(shutdownError(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard shutdown drain stopped before exact teardown completed."));
    } catch (...) {
        return Domain::Result<void>::failure(shutdownError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard shutdown drain could not join its process driver safely."));
    }
}

std::uint64_t DashboardShutdownDrain::registrationId() const noexcept
{
    return registrationId_;
}

DashboardShutdownDrainSnapshot DashboardShutdownDrain::snapshotLocked()
    const noexcept
{
    return DashboardShutdownDrainSnapshot{
        lifecycle_,
        installed_,
        gracefulShutdownRequested_,
        hardShutdownRequested_,
        handlerDrained_,
        listenersDrained_,
        overloadDrained_,
        registryConnectionsDrained_,
        executorFinalizerReturned_,
        routingProgressRevision_,
        staleDeadlineCount_,
        hardEscalationCount_,
        failFastCount_,
        currentDeadline_,
        bridgeFailure_,
        deadlineSchedulerFailure_};
}

DashboardShutdownDrainSnapshot DashboardShutdownDrain::snapshot()
    const noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        return snapshotLocked();
    } catch (...) {
        return DashboardShutdownDrainSnapshot{
            DashboardShutdownDrainLifecycle::Fatal,
            false,
            false,
            true,
            false,
            false,
            false,
            false,
            false,
            0U,
            0U,
            0U,
            0U,
            std::nullopt,
            DashboardDeadlineIocpFailure{
                DashboardDeadlineIocpFailureKind::InternalFailure,
                false},
            WindowsDashboardDeadlineSchedulerFailure{
                WindowsDashboardDeadlineSchedulerFailureKind::
                    WorkerFailure}};
    }
}

std::optional<Domain::Error> DashboardShutdownDrain::fullFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
