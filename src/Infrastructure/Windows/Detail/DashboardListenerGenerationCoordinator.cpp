#include "DashboardListenerGenerationCoordinator.h"

#include "DashboardOverloadResponderSet.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error coordinatorError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidCoordinatorError()
{
    return coordinatorError(
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard listener coordinator requires complete registration and preparation owners.");
}

[[nodiscard]] Domain::Error internalCoordinatorError()
{
    return coordinatorError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard listener coordinator failed safely.");
}

[[nodiscard]] Domain::Error retryableTransitionConflict(
    std::string message)
{
    return coordinatorError(
        Domain::ErrorCodes::Conflict, std::move(message), true);
}

void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

} // namespace

DashboardListenerGenerationOverloadDrainSource::
DashboardListenerGenerationOverloadDrainSource(
    DashboardOverloadResponderSet& overloadResponders) noexcept
    : overloadResponders_{std::addressof(overloadResponders)}
{
}

Domain::Result<void> DashboardListenerGenerationOverloadDrainSource::
bindOverloadDrainObserver(
    std::weak_ptr<IDashboardAdmissionOverloadDrainObserver> observer)
    noexcept
{
    return overloadResponders_->bindDrainObserver(std::move(observer));
}

DashboardListenerGenerationRegistrationHost::
DashboardListenerGenerationRegistrationHost(
    DashboardIocpCompletionRouter& completionRouter,
    DashboardConnectionRegistry& connectionRegistry) noexcept
    : completionRouter_{std::addressof(completionRouter)},
      connectionRegistry_{std::addressof(connectionRegistry)}
{
}

Domain::Result<void> DashboardListenerGenerationRegistrationHost::
bindConnectionDrainObserver(
    std::weak_ptr<IDashboardConnectionGenerationDrainObserver> observer)
    noexcept
{
    return connectionRegistry_->bindGenerationDrainObserver(
        std::move(observer));
}

Domain::Result<void> DashboardListenerGenerationRegistrationHost::
registerCompletionTarget(
    std::shared_ptr<IDashboardListenerGeneration> generation) noexcept
{
    return completionRouter_->registerFixedTarget(
        std::static_pointer_cast<IDashboardFixedIocpCompletionTarget>(
            std::move(generation)));
}

Domain::Result<void> DashboardListenerGenerationRegistrationHost::
registerDeadlineTarget(
    std::shared_ptr<IDashboardListenerGeneration> generation) noexcept
{
    return connectionRegistry_->registerAuxiliaryDeadlineTarget(
        std::static_pointer_cast<IDashboardAuxiliaryDeadlineTarget>(
            std::move(generation)));
}

bool DashboardListenerGenerationRegistrationHost::unregisterDeadlineTarget(
    const std::shared_ptr<IDashboardListenerGeneration>& generation) noexcept
{
    return connectionRegistry_->unregisterAuxiliaryDeadlineTarget(
        std::static_pointer_cast<IDashboardAuxiliaryDeadlineTarget>(
            generation));
}

bool DashboardListenerGenerationRegistrationHost::
unregisterCompletionTarget(
    const std::shared_ptr<IDashboardListenerGeneration>& generation) noexcept
{
    return completionRouter_->unregisterFixedTarget(
        std::static_pointer_cast<IDashboardFixedIocpCompletionTarget>(
            generation));
}

DashboardListenerGenerationCoordinatorSnapshot::
DashboardListenerGenerationCoordinatorSnapshot(
    std::optional<std::uint64_t> activeRegistrationId,
    std::optional<std::uint64_t> retiringRegistrationId,
    const bool preparationInProgress,
    const bool collectionInProgress,
    const bool shutdownRequested,
    const bool gracefulShutdownRequested,
    const bool hardShutdownRequested,
    const bool fatal,
    const bool hasFailure,
    const std::uint64_t publicationCount,
    const std::uint64_t retirementCount) noexcept
    : activeRegistrationId_{std::move(activeRegistrationId)},
      retiringRegistrationId_{std::move(retiringRegistrationId)},
      preparationInProgress_{preparationInProgress},
      collectionInProgress_{collectionInProgress},
      shutdownRequested_{shutdownRequested},
      gracefulShutdownRequested_{gracefulShutdownRequested},
      hardShutdownRequested_{hardShutdownRequested},
      fatal_{fatal},
      hasFailure_{hasFailure},
      publicationCount_{publicationCount},
      retirementCount_{retirementCount}
{
}

Domain::Result<std::shared_ptr<DashboardListenerGenerationCoordinator>>
DashboardListenerGenerationCoordinator::create(
    std::shared_ptr<IDashboardListenerGenerationRegistrationHost>
        registrationHost,
    std::shared_ptr<IDashboardListenerGenerationOverloadDrainSource>
        overloadDrainSource,
    std::shared_ptr<IDashboardListenerGenerationFactory> factory) noexcept
{
    using CreateResult = Domain::Result<std::shared_ptr<
        DashboardListenerGenerationCoordinator>>;
    if (registrationHost == nullptr || overloadDrainSource == nullptr ||
        factory == nullptr) {
        return CreateResult::failure(invalidCoordinatorError());
    }

    try {
        auto transitionGate = std::make_shared<
            DashboardListenerGenerationTransitionGate>();
        auto coordinator = std::shared_ptr<
            DashboardListenerGenerationCoordinator>{
                new DashboardListenerGenerationCoordinator{
                    std::move(registrationHost),
                    std::move(overloadDrainSource),
                    std::move(factory),
                    std::move(transitionGate)}};
        auto bound = coordinator->registrationHost_->
            bindConnectionDrainObserver(
                std::weak_ptr<IDashboardConnectionGenerationDrainObserver>{
                    coordinator});
        if (!bound) {
            return CreateResult::failure(std::move(bound).error());
        }
        auto overloadBound = coordinator->overloadDrainSource_->
            bindOverloadDrainObserver(
                std::weak_ptr<IDashboardAdmissionOverloadDrainObserver>{
                    coordinator});
        if (!overloadBound) {
            return CreateResult::failure(
                std::move(overloadBound).error());
        }
        return CreateResult::success(std::move(coordinator));
    } catch (...) {
        return CreateResult::failure(internalCoordinatorError());
    }
}

DashboardListenerGenerationCoordinator::
DashboardListenerGenerationCoordinator(
    std::shared_ptr<IDashboardListenerGenerationRegistrationHost>
        registrationHost,
    std::shared_ptr<IDashboardListenerGenerationOverloadDrainSource>
        overloadDrainSource,
    std::shared_ptr<IDashboardListenerGenerationFactory> factory,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate) noexcept
    : registrationHost_{std::move(registrationHost)},
      overloadDrainSource_{std::move(overloadDrainSource)},
      factory_{std::move(factory)},
      transitionGate_{std::move(transitionGate)}
{
}

DashboardListenerGenerationCoordinator::~
DashboardListenerGenerationCoordinator() noexcept
{
    beginShutdown();
    collectDrained();
}

Domain::Result<void>
DashboardListenerGenerationCoordinator::bindShutdownDrainObserver(
    std::weak_ptr<IDashboardListenerGenerationCoordinatorDrainObserver>
        observer) noexcept
{
    try {
        const auto pinned = observer.lock();
        if (pinned == nullptr) {
            return Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard listener coordinator requires a live process drain observer."));
        }
        const std::scoped_lock lock{mutex_};
        if (shutdownRequested_) {
            return Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::TransportClosed,
                "The dashboard listener coordinator is closed to process drain observer binding."));
        }
        if (shutdownDrainObserverEverBound_) {
            return Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::Conflict,
                "The dashboard listener coordinator process drain observer is one-shot."));
        }
        shutdownDrainObserver_ = std::move(observer);
        shutdownDrainObserverEverBound_ = true;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalCoordinatorError());
    }
}

Domain::Result<void>
DashboardListenerGenerationCoordinator::claimPreparation(
    const bool requireActive) noexcept
{
    const std::scoped_lock lock{mutex_};
    if (fatal_) {
        if (firstFailure_.has_value()) {
            return Domain::Result<void>::failure(
                Domain::Error{*firstFailure_});
        }
        return Domain::Result<void>::failure(internalCoordinatorError());
    }
    if (shutdownRequested_) {
        return Domain::Result<void>::failure(coordinatorError(
            Domain::ErrorCodes::TransportClosed,
            "The dashboard listener coordinator is shutting down."));
    }
    if (terminalPendingGenerationCount_ != 0U) {
        return Domain::Result<void>::failure(coordinatorError(
            Domain::ErrorCodes::IntegrityFailure,
            "A terminal dashboard overload edge is pending coordinated delivery."));
    }
    if (completionPendingGenerationCount_ != 0U) {
        return Domain::Result<void>::failure(retryableTransitionConflict(
            "A dashboard overload ownership completion is still settling."));
    }
    if (preparationInProgress_) {
        return Domain::Result<void>::failure(retryableTransitionConflict(
            "Another dashboard listener preparation is already in progress."));
    }
    if (collectionInProgress_) {
        return Domain::Result<void>::failure(retryableTransitionConflict(
            "A drained dashboard listener generation is still unregistering."));
    }
    if (retiring_.has_value()) {
        return Domain::Result<void>::failure(retryableTransitionConflict(
            "A dashboard listener generation is still retiring."));
    }
    if (requireActive && !active_.has_value()) {
        return Domain::Result<void>::failure(coordinatorError(
            Domain::ErrorCodes::Conflict,
            "The dashboard listener cannot rebind before initial publication."));
    }
    if (!requireActive && active_.has_value()) {
        return Domain::Result<void>::failure(coordinatorError(
            Domain::ErrorCodes::Conflict,
            "The initial dashboard listener is already published."));
    }
    preparationInProgress_ = true;
    return Domain::Result<void>::success();
}

Domain::Result<void>
DashboardListenerGenerationCoordinator::prepareAndRegister(
    GenerationEntry& entry) noexcept
{
    auto prepared = factory_->prepareGeneration(transitionGate_);
    if (!prepared) {
        return Domain::Result<void>::failure(std::move(prepared).error());
    }
    entry.generation = std::move(prepared).value();
    auto& generation = entry.generation;
    if (generation == nullptr || generation->registrationId() == 0U ||
        generation->completionKey().value() == 0U) {
        return Domain::Result<void>::failure(internalCoordinatorError());
    }

    auto observer = std::weak_ptr<IDashboardListenerGenerationDrainObserver>{
        shared_from_this()};
    auto bound = generation->bindDrainObserver(std::move(observer));
    if (!bound) {
        return Domain::Result<void>::failure(std::move(bound).error());
    }

    auto completionRegistered =
        registrationHost_->registerCompletionTarget(generation);
    if (!completionRegistered) {
        return Domain::Result<void>::failure(
            std::move(completionRegistered).error());
    }
    entry.completionRegistered = true;

    auto deadlineRegistered =
        registrationHost_->registerDeadlineTarget(generation);
    if (!deadlineRegistered) {
        return Domain::Result<void>::failure(
            std::move(deadlineRegistered).error());
    }
    entry.deadlineRegistered = true;
    return Domain::Result<void>::success();
}

void DashboardListenerGenerationCoordinator::finishPreparation() noexcept
{
    bool collect{};
    {
        const std::scoped_lock lock{mutex_};
        preparationInProgress_ = false;
        collect = drainCheckPending_;
        drainCheckPending_ = false;
    }
    if (collect) {
        collectDrained();
    } else {
        notifyShutdownDrainObserverIfReady();
    }
}

Domain::Result<void>
DashboardListenerGenerationCoordinator::startInitial() noexcept
{
    auto claimed = claimPreparation(false);
    if (!claimed) {
        return claimed;
    }

    GenerationEntry entry;
    auto prepared = prepareAndRegister(entry);
    if (!prepared) {
        if (entry.generation != nullptr) {
            rollbackPrepared(std::move(entry));
        }
        finishPreparation();
        return Domain::Result<void>::failure(std::move(prepared).error());
    }

    Domain::Result<void> started = [&]() noexcept {
        const std::scoped_lock lock{mutex_};
        if (shutdownRequested_) {
            return Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::TransportClosed,
                "Dashboard shutdown overtook initial listener preparation."));
        }
        return Domain::Result<void>::success();
    }();
    bool published{};
    if (started) {
        auto transition = transitionGate_->enter();
        started = [&]() noexcept {
            const std::scoped_lock lock{mutex_};
            if (shutdownRequested_ ||
                terminalPendingGenerationCount_ != 0U) {
                return Domain::Result<void>::failure(coordinatorError(
                    Domain::ErrorCodes::TransportClosed,
                    "Dashboard shutdown overtook initial listener start."));
            }
            if (completionPendingGenerationCount_ != 0U) {
                return Domain::Result<void>::failure(
                    retryableTransitionConflict(
                        "Dashboard overload ownership was still settling before initial listener start."));
            }
            return Domain::Result<void>::success();
        }();
        if (started) {
            started = entry.generation->startAdmission(transition);
        }
        if (started && entry.generation->fullyDrained()) {
            started = Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::TransportClosed,
                "The initial dashboard listener drained before publication."));
        }
        if (started) {
            const std::scoped_lock lock{mutex_};
            if (shutdownRequested_) {
                started = Domain::Result<void>::failure(coordinatorError(
                    Domain::ErrorCodes::TransportClosed,
                    "Dashboard shutdown overtook initial listener publication."));
            } else {
                active_.emplace(std::move(entry));
                incrementSaturating(publicationCount_);
                published = true;
            }
        }
    }
    if (!started || !published) {
        auto error = std::move(started).error();
        rollbackPrepared(std::move(entry));
        finishPreparation();
        return Domain::Result<void>::failure(std::move(error));
    }
    finishPreparation();
    collectDrained();
    return Domain::Result<void>::success();
}

Domain::Result<void> DashboardListenerGenerationCoordinator::rebind() noexcept
{
    auto claimed = claimPreparation(true);
    if (!claimed) {
        return claimed;
    }

    GenerationEntry replacement;
    auto prepared = prepareAndRegister(replacement);
    if (!prepared) {
        if (replacement.generation != nullptr) {
            rollbackPrepared(std::move(replacement));
        }
        finishPreparation();
        return Domain::Result<void>::failure(std::move(prepared).error());
    }

    std::shared_ptr<IDashboardListenerGeneration> previous;
    bool shutdownBeforeTransition{};
    {
        const std::scoped_lock lock{mutex_};
        shutdownBeforeTransition = shutdownRequested_;
        if (active_.has_value()) {
            previous = active_->generation;
        }
    }
    if (shutdownBeforeTransition || previous == nullptr) {
        rollbackPrepared(std::move(replacement));
        finishPreparation();
        return Domain::Result<void>::failure(coordinatorError(
            Domain::ErrorCodes::TransportClosed,
            "Dashboard shutdown overtook listener rebind preparation."));
    }

    Domain::Result<void> started = Domain::Result<void>::success();
    Domain::Result<void> retired = Domain::Result<void>::success();
    std::shared_ptr<IDashboardListenerGeneration> replacementForShutdown;
    bool published{};
    {
        auto transition = transitionGate_->enter();
        started = [&]() noexcept {
            const std::scoped_lock lock{mutex_};
            if (shutdownRequested_ ||
                terminalPendingGenerationCount_ != 0U) {
                return Domain::Result<void>::failure(coordinatorError(
                    Domain::ErrorCodes::TransportClosed,
                    "Dashboard shutdown overtook listener rebind start."));
            }
            if (completionPendingGenerationCount_ != 0U) {
                return Domain::Result<void>::failure(
                    retryableTransitionConflict(
                        "Dashboard overload ownership was still settling before listener rebind start."));
            }
            return Domain::Result<void>::success();
        }();
        if (started) {
            started = replacement.generation->startAdmission(transition);
        }
        if (started && replacement.generation->fullyDrained()) {
            started = Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::TransportClosed,
                "The replacement dashboard listener drained before publication."));
        }
        if (started) {
            retired = previous->beginRetirement(transition);
        }
        if (started && retired) {
            const std::scoped_lock lock{mutex_};
            if (!active_.has_value() ||
                active_->generation.get() != previous.get()) {
                retired = Domain::Result<void>::failure(coordinatorError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The active dashboard listener changed during serialized publication."));
            } else {
                GenerationEntry old = std::move(*active_);
                active_.reset();
                active_.emplace(std::move(replacement));
                retiring_.emplace(std::move(old));
                incrementSaturating(publicationCount_);
                incrementSaturating(retirementCount_);
                if (shutdownRequested_) {
                    replacementForShutdown = active_->generation;
                }
                published = true;
            }
        }
    }

    if (!started) {
        auto error = std::move(started).error();
        rollbackPrepared(std::move(replacement));
        finishPreparation();
        return Domain::Result<void>::failure(std::move(error));
    }
    if (!retired || !published) {
        auto error = std::move(retired).error();
        rollbackPrepared(std::move(replacement));
        finishPreparation();
        return Domain::Result<void>::failure(std::move(error));
    }
    finishPreparation();
    if (replacementForShutdown != nullptr) {
        replacementForShutdown->beginShutdown();
    }
    collectDrained();
    return Domain::Result<void>::success();
}

void DashboardListenerGenerationCoordinator::rollbackPrepared(
    GenerationEntry entry) noexcept
{
    std::shared_ptr<IDashboardListenerGeneration> generation;
    {
        const std::scoped_lock lock{mutex_};
        if (!retiring_.has_value()) {
            retiring_.emplace(std::move(entry));
            generation = retiring_->generation;
            drainCheckPending_ = true;
        }
    }
    if (generation != nullptr) {
        // Publish rollback ownership before shutdown. A synchronous last-
        // connection or overload edge can now find the exact generation and
        // re-drive its accept+connection drain state instead of being lost.
        generation->beginShutdown();
        generation->ownershipMayHaveDrained();
        collectDrained();
        return;
    }
    // This path is structurally unreachable because preparation is rejected
    // while any retiring generation exists. Retain ownership in the routing
    // hosts and fail closed rather than destroy native obligations.
}

Domain::Result<void>
DashboardListenerGenerationCoordinator::unregisterEntry(
    GenerationEntry& entry) noexcept
{
    if (!entry.generation->fullyDrained()) {
        return Domain::Result<void>::failure(coordinatorError(
            Domain::ErrorCodes::IntegrityFailure,
            "Only a fully drained dashboard listener generation may unregister."));
    }
    if (entry.deadlineRegistered) {
        if (!registrationHost_->unregisterDeadlineTarget(
                entry.generation)) {
            return Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard listener coordinator could not unregister exact deadline ownership for a fully drained generation."));
        }
        entry.deadlineRegistered = false;
    }
    if (entry.completionRegistered) {
        if (!registrationHost_->unregisterCompletionTarget(
                entry.generation)) {
            return Domain::Result<void>::failure(coordinatorError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard listener coordinator could not unregister exact completion ownership for a fully drained generation."));
        }
        entry.completionRegistered = false;
    }
    return Domain::Result<void>::success();
}

void DashboardListenerGenerationCoordinator::retainUnregisterFailure(
    GenerationEntry entry,
    const bool fromRetiring,
    Domain::Error error) noexcept
{
    {
        const std::scoped_lock lock{mutex_};
        if (fromRetiring) {
            if (!retiring_.has_value()) {
                retiring_.emplace(std::move(entry));
            }
        } else if (!active_.has_value()) {
            active_.emplace(std::move(entry));
        }
        if (!firstFailure_.has_value()) {
            firstFailure_.emplace(std::move(error));
        }
        fatal_ = true;
        shutdownRequested_ = true;
        shutdownFanoutStarted_ = true;
        unregisterFailed_ = true;
        collectionInProgress_ = false;
        drainCheckPending_ = false;
    }

    // A routing owner that refuses exact unregister is a structural process
    // failure. Keep every entry pinned in its original slot and fail closed;
    // callbacks after this point observe unregisterFailed_ and cannot spin a
    // permanently pending collection loop.
    fatal(ERROR_GEN_FAILURE);
}

void DashboardListenerGenerationCoordinator::collectDrained() noexcept
{
    bool collectionStarted{};
    {
        const std::scoped_lock lock{mutex_};
        if (unregisterFailed_) {
            drainCheckPending_ = false;
        } else if (preparationInProgress_ || collectionInProgress_ ||
                   shutdownFanoutDispatchInProgress_) {
            drainCheckPending_ = true;
        } else {
            collectionInProgress_ = true;
            collectionStarted = true;
        }
    }
    if (!collectionStarted) {
        notifyShutdownDrainObserverIfReady();
        return;
    }

    for (;;) {
        std::optional<GenerationEntry> candidate;
        bool fromRetiring{};
        {
            const std::scoped_lock lock{mutex_};
            if (retiring_.has_value() &&
                !terminalPendingLocked(
                    retiring_->generation->registrationId()) &&
                !completionPendingLocked(
                    retiring_->generation->registrationId()) &&
                retiring_->generation->fullyDrained()) {
                candidate.emplace(std::move(*retiring_));
                retiring_.reset();
                fromRetiring = true;
            } else if (active_.has_value() &&
                       !terminalPendingLocked(
                           active_->generation->registrationId()) &&
                       !completionPendingLocked(
                           active_->generation->registrationId()) &&
                       active_->generation->fullyDrained()) {
                candidate.emplace(std::move(*active_));
                active_.reset();
            } else {
                collectionInProgress_ = false;
                drainCheckPending_ = false;
            }
        }

        if (!candidate.has_value()) {
            notifyShutdownDrainObserverIfReady();
            return;
        }

        auto unregistered = unregisterEntry(*candidate);
        if (!unregistered || candidate->completionRegistered ||
            candidate->deadlineRegistered) {
            auto error = unregistered
                ? coordinatorError(
                      Domain::ErrorCodes::IntegrityFailure,
                      "A drained dashboard listener retained routing ownership after successful unregister.")
                : std::move(unregistered).error();
            retainUnregisterFailure(
                std::move(*candidate), fromRetiring, std::move(error));
            notifyShutdownDrainObserverIfReady();
            return;
        }
    }
}

std::shared_ptr<IDashboardListenerGenerationCoordinatorDrainObserver>
DashboardListenerGenerationCoordinator::takeShutdownDrainObserverLocked()
    noexcept
{
    if (shutdownDrainNotificationSent_ || !shutdownRequested_ ||
        preparationInProgress_ || collectionInProgress_ ||
        shutdownFanoutDispatchInProgress_ ||
        active_.has_value() || retiring_.has_value()) {
        return nullptr;
    }
    shutdownDrainNotificationSent_ = true;
    return shutdownDrainObserver_.lock();
}

void DashboardListenerGenerationCoordinator::
notifyShutdownDrainObserverIfReady() noexcept
{
    std::shared_ptr<IDashboardListenerGenerationCoordinatorDrainObserver>
        observer;
    bool missingBoundObserver{};
    {
        const std::scoped_lock lock{mutex_};
        const bool notificationWasSent =
            shutdownDrainNotificationSent_;
        observer = takeShutdownDrainObserverLocked();
        missingBoundObserver = !notificationWasSent &&
            shutdownDrainNotificationSent_ &&
            shutdownDrainObserverEverBound_ && observer == nullptr;
    }
    if (observer != nullptr) {
        observer->listenerGenerationsMayHaveDrained();
    } else if (missingBoundObserver) {
        // A bound process observer is a mandatory strong composition owner
        // until this edge. Losing it would otherwise turn exact shutdown into
        // an unbounded silent wait.
        std::terminate();
    }
}

bool DashboardListenerGenerationCoordinator::
claimShutdownFanoutDispatcherLocked() noexcept
{
    const bool pendingGraceful =
        gracefulShutdownFanoutStarted_ &&
        !gracefulShutdownFanoutCompleted_;
    const bool pendingFatal =
        fatalShutdownFanoutRequested_ &&
        !fatalShutdownFanoutCompleted_;
    const bool pendingHard =
        shutdownFanoutStarted_ && !hardShutdownFanoutCompleted_;
    if (shutdownFanoutDispatchInProgress_ ||
        (!pendingGraceful && !pendingFatal && !pendingHard)) {
        return false;
    }
    shutdownFanoutDispatchInProgress_ = true;
    return true;
}

void DashboardListenerGenerationCoordinator::dispatchShutdownFanouts()
    noexcept
{
    for (;;) {
        ShutdownFanoutAction action{ShutdownFanoutAction::None};
        std::shared_ptr<IDashboardListenerGeneration> active;
        std::shared_ptr<IDashboardListenerGeneration> retiring;
        DWORD fatalNativeError{};
        {
            const std::scoped_lock lock{mutex_};
            if (gracefulShutdownFanoutStarted_ &&
                !gracefulShutdownFanoutCompleted_) {
                action = ShutdownFanoutAction::Graceful;
            } else if (fatalShutdownFanoutRequested_ &&
                       !fatalShutdownFanoutCompleted_) {
                action = ShutdownFanoutAction::Fatal;
                fatalNativeError = fatalShutdownNativeError_;
            } else if (shutdownFanoutStarted_ &&
                       !hardShutdownFanoutCompleted_) {
                action = ShutdownFanoutAction::Hard;
            } else {
                shutdownFanoutDispatchInProgress_ = false;
            }
            if (action != ShutdownFanoutAction::None) {
                if (active_.has_value()) {
                    active = active_->generation;
                }
                if (retiring_.has_value()) {
                    retiring = retiring_->generation;
                }
            }
        }

        if (action == ShutdownFanoutAction::None) {
            break;
        }

        if (action == ShutdownFanoutAction::Graceful) {
            // Each generation needs a fresh transition guard because each
            // may schedule one distinct after-release drain pump. The
            // coordinator dispatcher nevertheless owns the complete fanout,
            // so a queued hard request cannot overtake the second callback.
            if (active != nullptr) {
                auto transition = transitionGate_->enter();
                static_cast<void>(
                    active->beginGracefulShutdown(transition));
            }
            if (retiring != nullptr &&
                retiring.get() != active.get()) {
                auto transition = transitionGate_->enter();
                static_cast<void>(
                    retiring->beginGracefulShutdown(transition));
            }
        } else if (action == ShutdownFanoutAction::Fatal) {
            if (active != nullptr) {
                active->fatal(fatalNativeError);
            }
            if (retiring != nullptr &&
                retiring.get() != active.get()) {
                retiring->fatal(fatalNativeError);
            }
        } else {
            if (active != nullptr) {
                active->beginShutdown();
            }
            if (retiring != nullptr &&
                retiring.get() != active.get()) {
                retiring->beginShutdown();
            }
        }

        {
            const std::scoped_lock lock{mutex_};
            if (action == ShutdownFanoutAction::Graceful) {
                gracefulShutdownFanoutCompleted_ = true;
            } else if (action == ShutdownFanoutAction::Fatal) {
                fatalShutdownFanoutCompleted_ = true;
                hardShutdownFanoutCompleted_ = true;
            } else {
                hardShutdownFanoutCompleted_ = true;
            }
        }
    }

    // Generation callbacks may have synchronously published exact zero while
    // the dispatcher was active. Re-drive collection only after every queued
    // callback has completed, then expose the process-drained edge.
    collectDrained();
}

void DashboardListenerGenerationCoordinator::beginGracefulShutdown()
    noexcept
{
    bool dispatch{};

    // Rebind holds this same gate from replacement start through old-owner
    // publication. Taking it before the shutdown snapshot gives graceful
    // cutoff one exact side of that transition.
    {
        auto cutoff = transitionGate_->enter();
        const std::scoped_lock lock{mutex_};
        shutdownRequested_ = true;
        if (!shutdownFanoutStarted_ &&
            !gracefulShutdownFanoutStarted_) {
            gracefulShutdownFanoutStarted_ = true;
        }
        dispatch = claimShutdownFanoutDispatcherLocked();
    }

    if (dispatch) {
        dispatchShutdownFanouts();
    } else {
        collectDrained();
    }
}

void DashboardListenerGenerationCoordinator::beginShutdown() noexcept
{
    bool dispatch{};
    {
        // Serialize the hard latch after any in-flight generation transition.
        // A live graceful dispatcher queues this request until its complete
        // active-and-retiring snapshot has received graceful cutoff.
        auto cutoff = transitionGate_->enter();
        const std::scoped_lock lock{mutex_};
        shutdownRequested_ = true;
        if (!shutdownFanoutStarted_) {
            shutdownFanoutStarted_ = true;
        }
        dispatch = claimShutdownFanoutDispatcherLocked();
    }
    if (dispatch) {
        dispatchShutdownFanouts();
    } else {
        collectDrained();
    }
}

void DashboardListenerGenerationCoordinator::fatal(
    const DWORD nativeError) noexcept
{
    bool dispatch{};
    {
        auto cutoff = transitionGate_->enter();
        const std::scoped_lock lock{mutex_};
        fatal_ = true;
        shutdownRequested_ = true;
        shutdownFanoutStarted_ = true;
        if (!fatalShutdownFanoutRequested_) {
            fatalShutdownFanoutRequested_ = true;
            fatalShutdownNativeError_ = nativeError;
        }
        dispatch = claimShutdownFanoutDispatcherLocked();
    }
    if (dispatch) {
        dispatchShutdownFanouts();
    } else {
        collectDrained();
    }
}

void DashboardListenerGenerationCoordinator::generationMayHaveDrained(
    const std::uint64_t) noexcept
{
    collectDrained();
}

void DashboardListenerGenerationCoordinator::
generationConnectionsMayHaveDrained(
    const std::uint64_t generationId) noexcept
{
    ownershipMayHaveDrained(generationId);
}

void DashboardListenerGenerationCoordinator::
overloadOwnerBeganShutdown() noexcept
{
    beginGracefulShutdown();
}

void DashboardListenerGenerationCoordinator::
overloadOwnerBecameTerminal() noexcept
{
    bool dispatch{};
    {
        auto cutoff = transitionGate_->enter();
        const std::scoped_lock lock{mutex_};
        if (!firstFailure_.has_value()) {
            try {
                firstFailure_.emplace(coordinatorError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The fixed dashboard overload owner became terminal."));
            } catch (...) {
                // Fatal lifecycle state and bounded shutdown do not depend on
                // retaining an allocation-backed diagnostic string.
            }
        }
        fatal_ = true;
        shutdownRequested_ = true;
        shutdownFanoutStarted_ = true;
        dispatch = claimShutdownFanoutDispatcherLocked();
    }
    if (dispatch) {
        dispatchShutdownFanouts();
    } else {
        collectDrained();
    }
}

void DashboardListenerGenerationCoordinator::
overloadGenerationMayHaveDrained(
    const std::uint64_t generationId) noexcept
{
    ownershipMayHaveDrained(generationId);
}

void DashboardListenerGenerationCoordinator::rememberTerminalPendingLocked(
    const std::uint64_t generationId) noexcept
{
    for (std::size_t index{}; index < terminalPendingGenerationCount_;
         ++index) {
        if (terminalPendingGenerationIds_[index] == generationId) {
            return;
        }
    }
    if (terminalPendingGenerationCount_ <
        terminalPendingGenerationIds_.size()) {
        terminalPendingGenerationIds_[terminalPendingGenerationCount_] =
            generationId;
        ++terminalPendingGenerationCount_;
        return;
    }
    fatal_ = true;
    shutdownRequested_ = true;
}

bool DashboardListenerGenerationCoordinator::terminalPendingLocked(
    const std::uint64_t generationId) const noexcept
{
    for (std::size_t index{}; index < terminalPendingGenerationCount_;
         ++index) {
        if (terminalPendingGenerationIds_[index] == generationId) {
            return true;
        }
    }
    return false;
}

bool DashboardListenerGenerationCoordinator::completionPendingLocked(
    const std::uint64_t generationId) const noexcept
{
    for (std::size_t index{}; index < completionPendingGenerationCount_;
         ++index) {
        if (completionPendingGenerationIds_[index] == generationId) {
            return true;
        }
    }
    return false;
}

void DashboardListenerGenerationCoordinator::
overloadGenerationTerminalPending(
    const std::uint64_t generationId) noexcept
{
    const std::scoped_lock lock{mutex_};
    const bool knownGeneration =
        (active_.has_value() &&
         active_->generation->registrationId() == generationId) ||
        (retiring_.has_value() &&
         retiring_->generation->registrationId() == generationId);
    if (knownGeneration) {
        rememberTerminalPendingLocked(generationId);
    }
}

void DashboardListenerGenerationCoordinator::
overloadGenerationCompletionPending(
    const std::uint64_t generationId) noexcept
{
    const std::scoped_lock lock{mutex_};
    const bool knownGeneration =
        (active_.has_value() &&
         active_->generation->registrationId() == generationId) ||
        (retiring_.has_value() &&
         retiring_->generation->registrationId() == generationId);
    if (!knownGeneration) {
        return;
    }
    if (completionPendingGenerationCount_ <
        completionPendingGenerationIds_.size()) {
        completionPendingGenerationIds_[completionPendingGenerationCount_] =
            generationId;
        ++completionPendingGenerationCount_;
        return;
    }
    fatal_ = true;
    shutdownRequested_ = true;
}

void DashboardListenerGenerationCoordinator::
overloadGenerationCompletionSettled(
    const std::uint64_t generationId) noexcept
{
    const std::scoped_lock lock{mutex_};
    for (std::size_t index{}; index < completionPendingGenerationCount_;
         ++index) {
        if (completionPendingGenerationIds_[index] != generationId) {
            continue;
        }
        --completionPendingGenerationCount_;
        completionPendingGenerationIds_[index] =
            completionPendingGenerationIds_[
                completionPendingGenerationCount_];
        completionPendingGenerationIds_[
            completionPendingGenerationCount_] = 0U;
        return;
    }
}

void DashboardListenerGenerationCoordinator::
overloadGenerationBecameTerminal(
    const std::uint64_t generationId) noexcept
{
    bool dispatch{};
    {
        auto cutoff = transitionGate_->enter();
        const std::scoped_lock lock{mutex_};
        const bool knownGeneration =
            (active_.has_value() &&
             active_->generation->registrationId() == generationId) ||
            (retiring_.has_value() &&
             retiring_->generation->registrationId() == generationId);
        if (!knownGeneration) {
            return;
        }
        if (!firstFailure_.has_value()) {
            try {
                firstFailure_.emplace(coordinatorError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The fixed dashboard overload owner became terminal while it retained exact listener-generation work."));
            } catch (...) {
                // Fatal lifecycle state and bounded shutdown do not depend on
                // retaining an allocation-backed diagnostic string.
            }
        }
        fatal_ = true;
        shutdownRequested_ = true;
        shutdownFanoutStarted_ = true;
        terminalPendingGenerationIds_.fill(0U);
        terminalPendingGenerationCount_ = 0U;
        dispatch = claimShutdownFanoutDispatcherLocked();
    }

    if (dispatch) {
        dispatchShutdownFanouts();
    } else {
        collectDrained();
    }
}

void DashboardListenerGenerationCoordinator::ownershipMayHaveDrained(
    const std::uint64_t generationId) noexcept
{
    std::shared_ptr<IDashboardListenerGeneration> generation;
    {
        const std::scoped_lock lock{mutex_};
        if (active_.has_value() &&
            active_->generation->registrationId() == generationId) {
            generation = active_->generation;
        } else if (
            retiring_.has_value() &&
            retiring_->generation->registrationId() == generationId) {
            generation = retiring_->generation;
        }
    }
    if (generation == nullptr) {
        return;
    }
    generation->ownershipMayHaveDrained();
    collectDrained();
}

DashboardListenerGenerationCoordinatorSnapshot
DashboardListenerGenerationCoordinator::snapshotLocked() const noexcept
{
    std::optional<std::uint64_t> activeRegistrationId;
    std::optional<std::uint64_t> retiringRegistrationId;
    if (active_.has_value()) {
        activeRegistrationId.emplace(
            active_->generation->registrationId());
    }
    if (retiring_.has_value()) {
        retiringRegistrationId.emplace(
            retiring_->generation->registrationId());
    }
    return DashboardListenerGenerationCoordinatorSnapshot{
        std::move(activeRegistrationId),
        std::move(retiringRegistrationId),
        preparationInProgress_,
        collectionInProgress_,
        shutdownRequested_,
        gracefulShutdownFanoutStarted_,
        shutdownFanoutStarted_,
        fatal_,
        firstFailure_.has_value(),
        publicationCount_,
        retirementCount_};
}

DashboardListenerGenerationCoordinatorSnapshot
DashboardListenerGenerationCoordinator::snapshot() const noexcept
{
    const std::scoped_lock lock{mutex_};
    return snapshotLocked();
}

std::optional<Domain::Error>
DashboardListenerGenerationCoordinator::fullFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
