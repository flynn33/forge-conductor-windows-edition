#include "WindowsDashboardRuntime.h"

#include "DashboardAdmissionController.h"
#include "DashboardConnectionRegistry.h"
#include "DashboardConnectionResponseCatalog.h"
#include "DashboardConnectionRuntimeServices.h"
#include "DashboardDeadlineIocpBridge.h"
#include "DashboardFixedIocpKeyAuthority.h"
#include "DashboardIoCompletionPort.h"
#include "DashboardIocpCompletionRouter.h"
#include "DashboardIocpWorkerKernel.h"
#include "DashboardListenerCompletionKeyLease.h"
#include "DashboardListenerGenerationCoordinator.h"
#include "DashboardOverloadResponderSet.h"
#include "DashboardShutdownDrain.h"
#include "DashboardWinsockRuntime.h"
#include "ManagerDashboardOperationalState.h"
#include "WindowsDashboardListenerGenerationFactory.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error runtimeError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidRuntimeDependenciesError()
{
    return runtimeError(
        Domain::ErrorCodes::InvalidRequest,
        "The Windows dashboard runtime requires a clock, UUID generator, and mutable operational-state owner.");
}

[[nodiscard]] Domain::Error runtimeAlreadyStartedError()
{
    return runtimeError(
        Domain::ErrorCodes::Conflict,
        "The Windows dashboard runtime initial listener is already published.");
}

[[nodiscard]] Domain::Error runtimeNotListeningError()
{
    return runtimeError(
        Domain::ErrorCodes::Conflict,
        "The Windows dashboard runtime can rebind only while listening.");
}

[[nodiscard]] Domain::Error sameEndpointTransitionConflictError()
{
    return runtimeError(
        Domain::ErrorCodes::Conflict,
        "A same-endpoint dashboard rebind requires a deferred post-delivery cutover.");
}

[[nodiscard]] Domain::Error runtimeClosedError()
{
    return runtimeError(
        Domain::ErrorCodes::TransportClosed,
        "The Windows dashboard runtime is shutting down.");
}

[[nodiscard]] Domain::Error waitBeforeShutdownError()
{
    return runtimeError(
        Domain::ErrorCodes::Conflict,
        "The Windows dashboard runtime cannot wait before shutdown is requested.");
}

[[nodiscard]] Domain::Error bindingRollbackError()
{
    return runtimeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The Windows dashboard runtime could not restore the active listener binding after a failed rebind.");
}

[[nodiscard]] Domain::Error bindingObservationError()
{
    return runtimeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The Windows dashboard runtime could not observe its exact published listener binding.");
}

[[nodiscard]] Domain::Error internalRuntimeError()
{
    return runtimeError(
        Domain::ErrorCodes::InternalFailure,
        "The Windows dashboard runtime failed safely.");
}

} // namespace

WindowsDashboardRuntimeSnapshot::WindowsDashboardRuntimeSnapshot(
    const WindowsDashboardRuntimeLifecycle lifecycle,
    const bool operationalServiceActive,
    std::optional<Domain::DashboardConfig> configuration,
    const void* const applicationIdentity,
    const std::uint64_t bindingPublicationSequence,
    std::optional<std::uint64_t> activeRegistrationId,
    std::optional<std::uint64_t> retiringRegistrationId,
    const std::uint64_t listenerPublicationCount,
    const std::uint64_t listenerRetirementCount,
    const std::size_t registeredConnectionCount,
    const std::size_t registeredAuxiliaryDeadlineTargetCount,
    const std::size_t fixedCompletionTargetCount,
    const std::size_t startedWorkerCount,
    const std::size_t exitedWorkerCount,
    const bool shutdownDrainInstalled,
    const bool shutdownDrainFatal) noexcept
    : lifecycle_{lifecycle},
      operationalServiceActive_{operationalServiceActive},
      configuration_{std::move(configuration)},
      applicationIdentity_{applicationIdentity},
      bindingPublicationSequence_{bindingPublicationSequence},
      activeRegistrationId_{std::move(activeRegistrationId)},
      retiringRegistrationId_{std::move(retiringRegistrationId)},
      listenerPublicationCount_{listenerPublicationCount},
      listenerRetirementCount_{listenerRetirementCount},
      registeredConnectionCount_{registeredConnectionCount},
      registeredAuxiliaryDeadlineTargetCount_{
          registeredAuxiliaryDeadlineTargetCount},
      fixedCompletionTargetCount_{fixedCompletionTargetCount},
      startedWorkerCount_{startedWorkerCount},
      exitedWorkerCount_{exitedWorkerCount},
      shutdownDrainInstalled_{shutdownDrainInstalled},
      shutdownDrainFatal_{shutdownDrainFatal}
{
}

WindowsDashboardRuntimeLifecycle
WindowsDashboardRuntimeSnapshot::lifecycle() const noexcept
{
    return lifecycle_;
}

bool WindowsDashboardRuntimeSnapshot::operationalServiceActive()
    const noexcept
{
    return operationalServiceActive_;
}

const std::optional<Domain::DashboardConfig>&
WindowsDashboardRuntimeSnapshot::configuration() const noexcept
{
    return configuration_;
}

const void* WindowsDashboardRuntimeSnapshot::applicationIdentity()
    const noexcept
{
    return applicationIdentity_;
}

std::uint64_t
WindowsDashboardRuntimeSnapshot::bindingPublicationSequence() const noexcept
{
    return bindingPublicationSequence_;
}

std::optional<std::uint64_t>
WindowsDashboardRuntimeSnapshot::activeRegistrationId() const noexcept
{
    return activeRegistrationId_;
}

std::optional<std::uint64_t>
WindowsDashboardRuntimeSnapshot::retiringRegistrationId() const noexcept
{
    return retiringRegistrationId_;
}

std::uint64_t
WindowsDashboardRuntimeSnapshot::listenerPublicationCount() const noexcept
{
    return listenerPublicationCount_;
}

std::uint64_t
WindowsDashboardRuntimeSnapshot::listenerRetirementCount() const noexcept
{
    return listenerRetirementCount_;
}

std::size_t
WindowsDashboardRuntimeSnapshot::registeredConnectionCount() const noexcept
{
    return registeredConnectionCount_;
}

std::size_t WindowsDashboardRuntimeSnapshot::
registeredAuxiliaryDeadlineTargetCount() const noexcept
{
    return registeredAuxiliaryDeadlineTargetCount_;
}

std::size_t
WindowsDashboardRuntimeSnapshot::fixedCompletionTargetCount() const noexcept
{
    return fixedCompletionTargetCount_;
}

std::size_t
WindowsDashboardRuntimeSnapshot::startedWorkerCount() const noexcept
{
    return startedWorkerCount_;
}

std::size_t
WindowsDashboardRuntimeSnapshot::exitedWorkerCount() const noexcept
{
    return exitedWorkerCount_;
}

bool WindowsDashboardRuntimeSnapshot::shutdownDrainInstalled() const noexcept
{
    return shutdownDrainInstalled_;
}

bool WindowsDashboardRuntimeSnapshot::shutdownDrainFatal() const noexcept
{
    return shutdownDrainFatal_;
}

class WindowsDashboardRuntime::Impl final {
public:
    struct SnapshotState final {
        WindowsDashboardRuntimeLifecycle lifecycle{
            WindowsDashboardRuntimeLifecycle::Ready};
        bool operationalServiceActive{};
        std::optional<Domain::DashboardConfig> configuration;
        const void* applicationIdentity{};
        std::uint64_t bindingPublicationSequence{};
        std::optional<std::uint64_t> activeRegistrationId;
        std::optional<std::uint64_t> retiringRegistrationId;
        std::uint64_t listenerPublicationCount{};
        std::uint64_t listenerRetirementCount{};
        std::size_t registeredConnectionCount{};
        std::size_t registeredAuxiliaryDeadlineTargetCount{};
        std::size_t fixedCompletionTargetCount{};
        std::size_t startedWorkerCount{};
        std::size_t exitedWorkerCount{};
        bool shutdownDrainInstalled{};
        bool shutdownDrainFatal{};
    };

    [[nodiscard]] static Domain::Result<std::unique_ptr<Impl>> create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<ManagerDashboardOperationalState>
            operationalState) noexcept
    {
        using CreateResult = Domain::Result<std::unique_ptr<Impl>>;
        if (clock == nullptr || uuidGenerator == nullptr ||
            operationalState == nullptr) {
            return CreateResult::failure(
                invalidRuntimeDependenciesError());
        }

        try {
            auto owner = std::unique_ptr<Impl>{new Impl{
                std::move(clock),
                std::move(uuidGenerator),
                std::move(operationalState)}};

            auto authority = DashboardFixedIocpKeyAuthority::create();
            if (!authority) {
                return CreateResult::failure(
                    std::move(authority).error());
            }
            owner->fixedKeyAuthority_ = std::make_unique<
                DashboardFixedIocpKeyAuthority>(
                    std::move(authority).value());

            auto listenerKeyLeases =
                DashboardListenerCompletionKeyLeasePool::create(
                    *owner->fixedKeyAuthority_);
            if (!listenerKeyLeases) {
                return CreateResult::failure(
                    std::move(listenerKeyLeases).error());
            }
            owner->listenerKeyLeases_ =
                std::move(listenerKeyLeases).value();

            auto winsock = DashboardWinsockRuntime::create();
            if (!winsock) {
                return CreateResult::failure(
                    std::move(winsock).error());
            }
            owner->winsock_ = std::move(winsock).value();

            auto registry = DashboardConnectionRegistry::create(
                owner->fixedKeyAuthority_->deadline());
            if (!registry) {
                return CreateResult::failure(
                    std::move(registry).error());
            }
            owner->connectionRegistry_ = std::move(registry).value();

            auto router = DashboardIocpCompletionRouter::create(
                owner->fixedKeyAuthority_->deadline(),
                owner->connectionRegistry_);
            if (!router) {
                return CreateResult::failure(
                    std::move(router).error());
            }
            owner->completionRouter_ = std::move(router).value();

            auto completionPort = DashboardIoCompletionPort::create();
            if (!completionPort) {
                return CreateResult::failure(
                    std::move(completionPort).error());
            }
            auto kernel = DashboardIocpWorkerKernel::create(
                std::move(completionPort).value(),
                owner->completionRouter_);
            if (!kernel) {
                return CreateResult::failure(
                    std::move(kernel).error());
            }
            owner->kernel_ = std::move(kernel).value();

            auto bridge = DashboardDeadlineIocpBridge::create(
                *owner->kernel_, owner->fixedKeyAuthority_->deadline());
            if (!bridge) {
                return CreateResult::failure(
                    std::move(bridge).error());
            }
            owner->deadlineBridge_ = std::move(bridge).value();

            auto bridgeBound = owner->connectionRegistry_->
                bindDeadlineBridge(owner->deadlineBridge_);
            if (!bridgeBound) {
                return CreateResult::failure(
                    std::move(bridgeBound).error());
            }

            auto scheduler = WindowsDashboardDeadlineScheduler::create(
                owner->clock_, owner->deadlineBridge_);
            if (!scheduler) {
                return CreateResult::failure(
                    std::move(scheduler).error());
            }
            owner->deadlineScheduler_ = std::move(scheduler).value();

            auto runtimeServices =
                DashboardConnectionRuntimeServices::create(
                    owner->clock_,
                    owner->uuidGenerator_,
                    owner->operationalState_,
                    *owner->fixedKeyAuthority_);
            if (!runtimeServices) {
                return CreateResult::failure(
                    std::move(runtimeServices).error());
            }
            owner->runtimeServices_ =
                std::move(runtimeServices).value();

            auto handlerExecutor =
                WindowsDashboardHandlerExecutor::create();
            if (!handlerExecutor) {
                return CreateResult::failure(
                    std::move(handlerExecutor).error());
            }
            owner->handlerExecutor_ =
                std::move(handlerExecutor).value();

            auto responseCatalog =
                DashboardConnectionResponseCatalog::create();
            if (!responseCatalog) {
                return CreateResult::failure(
                    std::move(responseCatalog).error());
            }
            owner->responseCatalog_ =
                std::move(responseCatalog).value();

            auto admissionController =
                DashboardAdmissionController::create();
            if (!admissionController) {
                return CreateResult::failure(
                    std::move(admissionController).error());
            }
            owner->admissionController_ =
                std::move(admissionController).value();

            auto overloadRegistrationId = owner->runtimeServices_->
                allocateAuxiliaryRegistrationId();
            if (!overloadRegistrationId) {
                return CreateResult::failure(
                    std::move(overloadRegistrationId).error());
            }
            auto overload = DashboardOverloadResponderSet::create(
                *owner->kernel_,
                owner->fixedKeyAuthority_->overload(),
                std::move(overloadRegistrationId).value(),
                *owner->deadlineScheduler_,
                *owner->runtimeServices_,
                *owner->responseCatalog_);
            if (!overload) {
                return CreateResult::failure(
                    std::move(overload).error());
            }
            owner->overloadResponders_ = std::move(overload).value();

            owner->registrationHost_ = std::make_shared<
                DashboardListenerGenerationRegistrationHost>(
                    *owner->completionRouter_,
                    *owner->connectionRegistry_);
            owner->overloadDrainSource_ = std::make_shared<
                DashboardListenerGenerationOverloadDrainSource>(
                    *owner->overloadResponders_);

            auto factory =
                WindowsDashboardListenerGenerationFactory::create(
                    *owner->winsock_,
                    *owner->kernel_,
                    *owner->deadlineScheduler_,
                    *owner->handlerExecutor_,
                    *owner->runtimeServices_,
                    *owner->admissionController_,
                    *owner->listenerKeyLeases_,
                    owner->connectionRegistry_,
                    std::static_pointer_cast<
                        IDashboardAdmissionOverloadResponder>(
                            owner->overloadResponders_),
                    *owner->responseCatalog_);
            if (!factory) {
                return CreateResult::failure(
                    std::move(factory).error());
            }
            owner->listenerFactory_ = std::move(factory).value();

            auto coordinator =
                DashboardListenerGenerationCoordinator::create(
                    owner->registrationHost_,
                    owner->overloadDrainSource_,
                    owner->listenerFactory_);
            if (!coordinator) {
                return CreateResult::failure(
                    std::move(coordinator).error());
            }
            owner->listenerCoordinator_ =
                std::move(coordinator).value();

            owner->shutdownHost_ =
                std::make_shared<DashboardShutdownDrainHost>(
                    *owner->runtimeServices_,
                    *owner->handlerExecutor_,
                    *owner->deadlineScheduler_,
                    *owner->kernel_,
                    owner->deadlineBridge_,
                    owner->listenerCoordinator_,
                    owner->overloadResponders_,
                    owner->connectionRegistry_,
                    owner->completionRouter_);
            owner->shutdownFailFast_ = std::make_shared<
                DashboardShutdownDrainProcessFailFast>();

            auto shutdownRegistrationId = owner->runtimeServices_->
                allocateAuxiliaryRegistrationId();
            if (!shutdownRegistrationId) {
                return CreateResult::failure(
                    std::move(shutdownRegistrationId).error());
            }
            auto shutdownDrain = DashboardShutdownDrain::create(
                std::move(shutdownRegistrationId).value(),
                owner->shutdownHost_,
                owner->shutdownFailFast_);
            if (!shutdownDrain) {
                return CreateResult::failure(
                    std::move(shutdownDrain).error());
            }
            owner->shutdownDrain_ = std::move(shutdownDrain).value();

            // Register the fixed overload owner only after every later graph
            // allocation has succeeded. From this point, any failure is the
            // drain installation transaction itself, whose fail-closed path
            // owns exact graph teardown.
            auto completionRegistered =
                owner->completionRouter_->registerFixedTarget(
                    std::static_pointer_cast<
                        IDashboardFixedIocpCompletionTarget>(
                            owner->overloadResponders_));
            if (!completionRegistered) {
                return CreateResult::failure(
                    std::move(completionRegistered).error());
            }
            owner->overloadCompletionRegistered_ = true;

            auto deadlineRegistered = owner->connectionRegistry_->
                registerAuxiliaryDeadlineTarget(
                    std::static_pointer_cast<
                        IDashboardAuxiliaryDeadlineTarget>(
                            owner->overloadResponders_));
            if (!deadlineRegistered) {
                return CreateResult::failure(
                    std::move(deadlineRegistered).error());
            }
            owner->overloadDeadlineRegistered_ = true;

            // The process drain observes every owner and holds its registered
            // deadline route before start() can publish initial admission.
            auto installed = owner->shutdownDrain_->install();
            if (!installed) {
                return CreateResult::failure(
                    std::move(installed).error());
            }
            owner->shutdownDrainInstalled_ = true;
            return CreateResult::success(std::move(owner));
        } catch (...) {
            return CreateResult::failure(internalRuntimeError());
        }
    }

    ~Impl() noexcept
    {
        try {
            if (shutdownDrainInstalled_ && shutdownDrain_ != nullptr) {
                listenerFactory_->clearBinding();
                operationalState_->setOperationalServiceActive(false);
                shutdownDrain_->requestGracefulShutdown();
                static_cast<void>(shutdownDrain_->wait());
                return;
            }
            cleanPartialGraph();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] Domain::Result<void> start(
        WindowsDashboardRuntimeBinding binding) noexcept
    {
        try {
            auto candidate = std::make_shared<
                const WindowsDashboardRuntimeBinding>(
                    std::move(binding));
            const std::scoped_lock lock{lifecycleMutex_};
            if (shutdownRequested_ ||
                lifecycle_ == WindowsDashboardRuntimeLifecycle::
                    ShuttingDown ||
                lifecycle_ == WindowsDashboardRuntimeLifecycle::Drained ||
                lifecycle_ == WindowsDashboardRuntimeLifecycle::Failed) {
                return Domain::Result<void>::failure(
                    runtimeClosedError());
            }
            if (lifecycle_ != WindowsDashboardRuntimeLifecycle::Ready ||
                activeBinding_ != nullptr) {
                return Domain::Result<void>::failure(
                    runtimeAlreadyStartedError());
            }
            if (shutdownDrain_->snapshot().lifecycle() ==
                DashboardShutdownDrainLifecycle::Fatal) {
                beginFailureShutdownLocked();
                return Domain::Result<void>::failure(
                    internalRuntimeError());
            }

            auto candidatePublished = listenerFactory_->publishBinding(
                candidate->configuration, candidate->application);
            if (!candidatePublished) {
                listenerFactory_->clearBinding();
                return Domain::Result<void>::failure(
                    std::move(candidatePublished).error());
            }
            auto published = observePublishedBinding(*candidate);
            if (!published) {
                listenerFactory_->clearBinding();
                return Domain::Result<void>::failure(
                    std::move(published).error());
            }
            const auto publicationSequence =
                std::move(published).value();

            auto started = listenerCoordinator_->startInitial();
            if (!started) {
                listenerFactory_->clearBinding();
                if (listenerCoordinator_->snapshot().fatal()) {
                    beginFailureShutdownLocked();
                }
                return Domain::Result<void>::failure(
                    std::move(started).error());
            }

            activeBinding_ = std::move(candidate);
            bindingPublicationSequence_ = publicationSequence;
            lifecycle_ = WindowsDashboardRuntimeLifecycle::Listening;
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(internalRuntimeError());
        }
    }

    [[nodiscard]] Domain::Result<void> rebind(
        WindowsDashboardRuntimeBinding binding) noexcept
    {
        try {
            auto candidate = std::make_shared<
                const WindowsDashboardRuntimeBinding>(
                    std::move(binding));
            const std::scoped_lock lock{lifecycleMutex_};
            if (shutdownRequested_ ||
                lifecycle_ == WindowsDashboardRuntimeLifecycle::
                    ShuttingDown ||
                lifecycle_ == WindowsDashboardRuntimeLifecycle::Drained ||
                lifecycle_ == WindowsDashboardRuntimeLifecycle::Failed) {
                return Domain::Result<void>::failure(
                    runtimeClosedError());
            }
            if (lifecycle_ != WindowsDashboardRuntimeLifecycle::Listening ||
                activeBinding_ == nullptr) {
                return Domain::Result<void>::failure(
                    runtimeNotListeningError());
            }

            const auto previous = activeBinding_;
            const bool sameEndpoint =
                candidate->configuration.host ==
                    previous->configuration.host &&
                candidate->configuration.port ==
                    previous->configuration.port;
            if (sameEndpoint) {
                return Domain::Result<void>::failure(
                    sameEndpointTransitionConflictError());
            }

            auto candidatePublished = listenerFactory_->publishBinding(
                candidate->configuration, candidate->application);
            if (!candidatePublished) {
                return Domain::Result<void>::failure(
                    std::move(candidatePublished).error());
            }
            auto published = observePublishedBinding(*candidate);
            if (!published) {
                auto restored = publishAndObserveBinding(*previous);
                if (!restored) {
                    beginFailureShutdownLocked();
                    return Domain::Result<void>::failure(
                        bindingRollbackError());
                }
                bindingPublicationSequence_ =
                    std::move(restored).value();
                return Domain::Result<void>::failure(
                    std::move(published).error());
            }
            const auto publicationSequence =
                std::move(published).value();

            auto rebound = listenerCoordinator_->rebind();
            if (!rebound) {
                auto restored = publishAndObserveBinding(*previous);
                if (!restored) {
                    beginFailureShutdownLocked();
                    return Domain::Result<void>::failure(
                        bindingRollbackError());
                }
                bindingPublicationSequence_ =
                    std::move(restored).value();
                if (listenerCoordinator_->snapshot().fatal()) {
                    beginFailureShutdownLocked();
                }
                return Domain::Result<void>::failure(
                    std::move(rebound).error());
            }

            activeBinding_ = std::move(candidate);
            bindingPublicationSequence_ = publicationSequence;
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(internalRuntimeError());
        }
    }

    void setOperationalServiceActive(const bool active) noexcept
    {
        try {
            const std::scoped_lock lock{lifecycleMutex_};
            if (!shutdownRequested_ &&
                lifecycle_ != WindowsDashboardRuntimeLifecycle::Drained &&
                lifecycle_ != WindowsDashboardRuntimeLifecycle::Failed) {
                operationalState_->setOperationalServiceActive(active);
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] Domain::Result<SnapshotState> snapshotState()
        const noexcept
    {
        try {
            const std::scoped_lock lock{lifecycleMutex_};
            const auto coordinator = listenerCoordinator_->snapshot();
            const auto registry = connectionRegistry_->snapshot();
            const auto router = completionRouter_->snapshot();
            const auto workers = kernel_->snapshot();
            const auto drain = shutdownDrain_->snapshot();

            SnapshotState result;
            if (drain.lifecycle() ==
                DashboardShutdownDrainLifecycle::Fatal) {
                result.lifecycle =
                    WindowsDashboardRuntimeLifecycle::Failed;
            } else {
                result.lifecycle = lifecycle_;
            }
            result.operationalServiceActive =
                operationalState_->operationalServiceActive();
            if (activeBinding_ != nullptr &&
                result.lifecycle !=
                    WindowsDashboardRuntimeLifecycle::Drained) {
                result.configuration.emplace(
                    activeBinding_->configuration);
                result.applicationIdentity =
                    activeBinding_->application.get();
                result.bindingPublicationSequence =
                    bindingPublicationSequence_;
            }
            result.activeRegistrationId =
                coordinator.activeRegistrationId();
            result.retiringRegistrationId =
                coordinator.retiringRegistrationId();
            result.listenerPublicationCount =
                coordinator.publicationCount();
            result.listenerRetirementCount =
                coordinator.retirementCount();
            result.registeredConnectionCount =
                registry.registeredConnectionCount();
            result.registeredAuxiliaryDeadlineTargetCount =
                registry.registeredAuxiliaryDeadlineTargetCount();
            result.fixedCompletionTargetCount =
                router.fixedTargetCount();
            result.startedWorkerCount = workers.startedWorkerCount();
            result.exitedWorkerCount = workers.exitedWorkerCount();
            result.shutdownDrainInstalled = drain.installed();
            result.shutdownDrainFatal = drain.lifecycle() ==
                DashboardShutdownDrainLifecycle::Fatal;
            return Domain::Result<SnapshotState>::success(
                std::move(result));
        } catch (...) {
            return Domain::Result<SnapshotState>::failure(
                internalRuntimeError());
        }
    }

    void requestGracefulShutdown() noexcept
    {
        try {
            bool request{};
            {
                const std::scoped_lock lock{lifecycleMutex_};
                if (lifecycle_ == WindowsDashboardRuntimeLifecycle::Drained ||
                    shutdownRequested_) {
                    return;
                }
                shutdownRequested_ = true;
                listenerFactory_->clearBinding();
                operationalState_->setOperationalServiceActive(false);
                if (lifecycle_ != WindowsDashboardRuntimeLifecycle::Failed) {
                    lifecycle_ =
                        WindowsDashboardRuntimeLifecycle::ShuttingDown;
                }
                request = true;
            }
            if (request) {
                shutdownDrain_->requestGracefulShutdown();
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] Domain::Result<void> wait() noexcept
    {
        try {
            const std::scoped_lock joinLock{waitMutex_};
            {
                const std::scoped_lock lock{lifecycleMutex_};
                if (!shutdownRequested_ &&
                    lifecycle_ != WindowsDashboardRuntimeLifecycle::Drained &&
                    lifecycle_ != WindowsDashboardRuntimeLifecycle::Failed) {
                    return Domain::Result<void>::failure(
                        waitBeforeShutdownError());
                }
            }

            auto waited = shutdownDrain_->wait();
            {
                const std::scoped_lock lock{lifecycleMutex_};
                if (waited) {
                    activeBinding_.reset();
                    bindingPublicationSequence_ = 0U;
                    lifecycle_ = WindowsDashboardRuntimeLifecycle::Drained;
                } else {
                    lifecycle_ = WindowsDashboardRuntimeLifecycle::Failed;
                }
            }
            return waited;
        } catch (...) {
            return Domain::Result<void>::failure(internalRuntimeError());
        }
    }

private:
    Impl(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<ManagerDashboardOperationalState>
            operationalState) noexcept
        : clock_{std::move(clock)},
          uuidGenerator_{std::move(uuidGenerator)},
          operationalState_{std::move(operationalState)}
    {
    }

    [[nodiscard]] Domain::Result<std::uint64_t> observePublishedBinding(
        const WindowsDashboardRuntimeBinding& binding) noexcept
    {
        auto snapshot = listenerFactory_->bindingSnapshot();
        if (!snapshot) {
            return Domain::Result<std::uint64_t>::failure(
                std::move(snapshot).error());
        }
        auto observed = std::move(snapshot).value();
        if (!observed.has_value() ||
            observed->configuration() != binding.configuration ||
            observed->applicationIdentity() != binding.application.get() ||
            observed->publicationSequence() == 0U) {
            return Domain::Result<std::uint64_t>::failure(
                bindingObservationError());
        }
        return Domain::Result<std::uint64_t>::success(
            observed->publicationSequence());
    }

    [[nodiscard]] Domain::Result<std::uint64_t> publishAndObserveBinding(
        const WindowsDashboardRuntimeBinding& binding) noexcept
    {
        auto published = listenerFactory_->publishBinding(
            binding.configuration, binding.application);
        if (!published) {
            return Domain::Result<std::uint64_t>::failure(
                std::move(published).error());
        }
        return observePublishedBinding(binding);
    }

    void beginFailureShutdownLocked() noexcept
    {
        lifecycle_ = WindowsDashboardRuntimeLifecycle::Failed;
        shutdownRequested_ = true;
        listenerFactory_->clearBinding();
        operationalState_->setOperationalServiceActive(false);
        shutdownDrain_->requestGracefulShutdown();
    }

    void cleanPartialGraph() noexcept
    {
        if (listenerFactory_ != nullptr) {
            listenerFactory_->clearBinding();
        }
        if (runtimeServices_ != nullptr) {
            runtimeServices_->beginShutdown();
        }
        if (listenerCoordinator_ != nullptr) {
            listenerCoordinator_->beginShutdown();
        }
        if (overloadResponders_ != nullptr) {
            overloadResponders_->beginShutdown();
        }
        if (handlerExecutor_ != nullptr) {
            handlerExecutor_->shutdown();
        }
        if (connectionRegistry_ != nullptr) {
            connectionRegistry_->beginShutdown();
        }
        if (overloadDeadlineRegistered_ &&
            connectionRegistry_ != nullptr &&
            overloadResponders_ != nullptr) {
            if (!overloadResponders_->snapshot().fullyDrained() ||
                !connectionRegistry_->unregisterAuxiliaryDeadlineTarget(
                    std::static_pointer_cast<
                        IDashboardAuxiliaryDeadlineTarget>(
                            overloadResponders_))) {
                failIncompletePartialGraph();
            }
            overloadDeadlineRegistered_ = false;
        }
        if (overloadCompletionRegistered_ &&
            completionRouter_ != nullptr &&
            overloadResponders_ != nullptr) {
            if (!overloadResponders_->snapshot().fullyDrained() ||
                !completionRouter_->unregisterFixedTarget(
                    std::static_pointer_cast<
                        IDashboardFixedIocpCompletionTarget>(
                            overloadResponders_))) {
                failIncompletePartialGraph();
            }
            overloadCompletionRegistered_ = false;
        }
        if (completionRouter_ != nullptr) {
            completionRouter_->beginShutdown();
        }
        if (deadlineScheduler_ != nullptr) {
            deadlineScheduler_->shutdown();
        }
        if (connectionRegistry_ != nullptr) {
            auto finalized =
                connectionRegistry_->finalizeDeadlineRouting();
            if (!finalized || std::move(finalized).value() !=
                    DashboardDeadlineRoutingFinalizeDisposition::Finalized) {
                failIncompletePartialGraph();
            }
        } else if (deadlineBridge_ != nullptr) {
            deadlineBridge_->shutdown();
        }
        if (kernel_ != nullptr) {
            kernel_->shutdown();
        }
    }

    [[noreturn]] void failIncompletePartialGraph() noexcept
    {
        // Returning would release a retained callback target after its
        // borrowed scheduler/runtime/kernel owners. Terminate while the full
        // graph is still retained rather than manufacture a use-after-free.
        if (shutdownFailFast_ != nullptr) {
            shutdownFailFast_->failFast();
        }
        std::terminate();
    }

    // Declared from foundations to dependents so reverse member destruction
    // cannot release a borrowed owner before the graph has drained.
    mutable std::mutex lifecycleMutex_;
    std::mutex waitMutex_;
    const std::shared_ptr<Contracts::IClock> clock_;
    const std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    const std::shared_ptr<ManagerDashboardOperationalState>
        operationalState_;
    std::unique_ptr<DashboardFixedIocpKeyAuthority> fixedKeyAuthority_;
    std::unique_ptr<DashboardListenerCompletionKeyLeasePool>
        listenerKeyLeases_;
    std::unique_ptr<DashboardWinsockRuntime> winsock_;
    std::shared_ptr<DashboardConnectionRegistry> connectionRegistry_;
    std::shared_ptr<DashboardIocpCompletionRouter> completionRouter_;
    std::unique_ptr<DashboardIocpWorkerKernel> kernel_;
    std::shared_ptr<DashboardDeadlineIocpBridge> deadlineBridge_;
    std::unique_ptr<WindowsDashboardDeadlineScheduler> deadlineScheduler_;
    std::unique_ptr<DashboardConnectionRuntimeServices> runtimeServices_;
    std::unique_ptr<WindowsDashboardHandlerExecutor> handlerExecutor_;
    std::unique_ptr<DashboardConnectionResponseCatalog> responseCatalog_;
    std::unique_ptr<DashboardAdmissionController> admissionController_;
    std::shared_ptr<DashboardOverloadResponderSet> overloadResponders_;
    std::shared_ptr<DashboardListenerGenerationRegistrationHost>
        registrationHost_;
    std::shared_ptr<DashboardListenerGenerationOverloadDrainSource>
        overloadDrainSource_;
    std::shared_ptr<WindowsDashboardListenerGenerationFactory>
        listenerFactory_;
    std::shared_ptr<DashboardListenerGenerationCoordinator>
        listenerCoordinator_;
    std::shared_ptr<DashboardShutdownDrainHost> shutdownHost_;
    std::shared_ptr<DashboardShutdownDrainProcessFailFast>
        shutdownFailFast_;
    std::shared_ptr<DashboardShutdownDrain> shutdownDrain_;
    std::shared_ptr<const WindowsDashboardRuntimeBinding> activeBinding_;
    std::uint64_t bindingPublicationSequence_{};
    WindowsDashboardRuntimeLifecycle lifecycle_{
        WindowsDashboardRuntimeLifecycle::Ready};
    bool overloadCompletionRegistered_{};
    bool overloadDeadlineRegistered_{};
    bool shutdownDrainInstalled_{};
    bool shutdownRequested_{};
};

Domain::Result<std::unique_ptr<WindowsDashboardRuntime>>
WindowsDashboardRuntime::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<ManagerDashboardOperationalState> operationalState)
    noexcept
{
    using CreateResult =
        Domain::Result<std::unique_ptr<WindowsDashboardRuntime>>;
    auto implementation = Impl::create(
        std::move(clock),
        std::move(uuidGenerator),
        std::move(operationalState));
    if (!implementation) {
        return CreateResult::failure(
            std::move(implementation).error());
    }
    try {
        return CreateResult::success(
            std::unique_ptr<WindowsDashboardRuntime>{
                new WindowsDashboardRuntime{
                    std::move(implementation).value()}});
    } catch (...) {
        return CreateResult::failure(internalRuntimeError());
    }
}

WindowsDashboardRuntime::WindowsDashboardRuntime(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsDashboardRuntime::~WindowsDashboardRuntime() noexcept = default;

Domain::Result<void> WindowsDashboardRuntime::start(
    WindowsDashboardRuntimeBinding binding) noexcept
{
    return implementation_->start(std::move(binding));
}

Domain::Result<void> WindowsDashboardRuntime::rebind(
    WindowsDashboardRuntimeBinding binding) noexcept
{
    return implementation_->rebind(std::move(binding));
}

void WindowsDashboardRuntime::pauseOperationalService() noexcept
{
    implementation_->setOperationalServiceActive(false);
}

void WindowsDashboardRuntime::resumeOperationalService() noexcept
{
    implementation_->setOperationalServiceActive(true);
}

Domain::Result<WindowsDashboardRuntimeSnapshot>
WindowsDashboardRuntime::snapshot() const noexcept
{
    auto state = implementation_->snapshotState();
    if (!state) {
        return Domain::Result<WindowsDashboardRuntimeSnapshot>::failure(
            std::move(state).error());
    }
    auto observed = std::move(state).value();
    return Domain::Result<WindowsDashboardRuntimeSnapshot>::success(
        WindowsDashboardRuntimeSnapshot{
            observed.lifecycle,
            observed.operationalServiceActive,
            std::move(observed.configuration),
            observed.applicationIdentity,
            observed.bindingPublicationSequence,
            observed.activeRegistrationId,
            observed.retiringRegistrationId,
            observed.listenerPublicationCount,
            observed.listenerRetirementCount,
            observed.registeredConnectionCount,
            observed.registeredAuxiliaryDeadlineTargetCount,
            observed.fixedCompletionTargetCount,
            observed.startedWorkerCount,
            observed.exitedWorkerCount,
            observed.shutdownDrainInstalled,
            observed.shutdownDrainFatal});
}

void WindowsDashboardRuntime::requestGracefulShutdown() noexcept
{
    implementation_->requestGracefulShutdown();
}

Domain::Result<void> WindowsDashboardRuntime::wait() noexcept
{
    return implementation_->wait();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
