#include "Infrastructure/Windows/Detail/DashboardConnectionRegistry.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error registryError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard connection registry failure could not retain its diagnostic.",
            false);
    }
}

[[nodiscard]] DashboardConnectionRegistryFailureKind classifyFailureKind(
    const Domain::Error& error) noexcept
{
    if (error.code == Domain::ErrorCodes::InvalidRequest) {
        return DashboardConnectionRegistryFailureKind::InvalidRequest;
    }
    if (error.code == Domain::ErrorCodes::Conflict) {
        return DashboardConnectionRegistryFailureKind::Conflict;
    }
    if (error.code == Domain::ErrorCodes::LimitExceeded) {
        return DashboardConnectionRegistryFailureKind::LimitExceeded;
    }
    if (error.code == Domain::ErrorCodes::TransportClosed) {
        return DashboardConnectionRegistryFailureKind::TransportClosed;
    }
    if (error.code == Domain::ErrorCodes::IntegrityFailure) {
        return DashboardConnectionRegistryFailureKind::IntegrityFailure;
    }
    if (error.code == Domain::ErrorCodes::InternalFailure) {
        return DashboardConnectionRegistryFailureKind::InternalFailure;
    }
    return DashboardConnectionRegistryFailureKind::Other;
}

[[nodiscard]] std::string_view eventFailureCode(
    const DashboardConnectionEventFailureKind kind) noexcept
{
    switch (kind) {
    case DashboardConnectionEventFailureKind::InvalidRequest:
        return Domain::ErrorCodes::InvalidRequest;
    case DashboardConnectionEventFailureKind::Conflict:
        return Domain::ErrorCodes::Conflict;
    case DashboardConnectionEventFailureKind::LimitExceeded:
        return Domain::ErrorCodes::LimitExceeded;
    case DashboardConnectionEventFailureKind::TransportClosed:
        return Domain::ErrorCodes::TransportClosed;
    case DashboardConnectionEventFailureKind::IntegrityFailure:
        return Domain::ErrorCodes::IntegrityFailure;
    case DashboardConnectionEventFailureKind::InternalFailure:
        return Domain::ErrorCodes::InternalFailure;
    case DashboardConnectionEventFailureKind::Other:
        return Domain::ErrorCodes::InternalFailure;
    }
    return Domain::ErrorCodes::InternalFailure;
}

void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

class DeadlineRoutingProgressGuard final {
public:
    explicit DeadlineRoutingProgressGuard(
        std::atomic_bool& inProgress) noexcept
        : inProgress_{inProgress}
    {
        inProgress_.store(true, std::memory_order_release);
    }

    ~DeadlineRoutingProgressGuard() noexcept
    {
        inProgress_.store(false, std::memory_order_release);
    }

    DeadlineRoutingProgressGuard(const DeadlineRoutingProgressGuard&) = delete;
    DeadlineRoutingProgressGuard& operator=(
        const DeadlineRoutingProgressGuard&) = delete;

private:
    std::atomic_bool& inProgress_;
};

class DashboardConnectionRegistryProcessFailFast final
    : public IDashboardConnectionRegistryFailFast {
public:
    void failFast() noexcept override { std::terminate(); }
};

} // namespace

DashboardConnectionRegistrySnapshot::DashboardConnectionRegistrySnapshot(
    const std::size_t registeredConnectionCount,
    const std::size_t maximumConnectionCount,
    const std::size_t registeredAuxiliaryDeadlineTargetCount,
    const DashboardIoCompletionKey deadlineCompletionKey,
    const bool deadlineBridgeBound,
    const std::uint64_t connectionDispatchCount,
    const std::uint64_t deadlineDispatchCount,
    const std::uint64_t retiredDeadlineDrainCount,
    const std::uint64_t removedConnectionCount,
    const std::uint64_t fatalNotificationCount,
    const std::uint64_t routingProgressRevision,
    const bool deadlineRoutingInProgress,
    const bool shutdown,
    const bool fatal,
    std::optional<DashboardConnectionRegistryFailure> failure) noexcept
    : registeredConnectionCount_{registeredConnectionCount},
      maximumConnectionCount_{maximumConnectionCount},
      registeredAuxiliaryDeadlineTargetCount_{
          registeredAuxiliaryDeadlineTargetCount},
      deadlineCompletionKey_{deadlineCompletionKey},
      deadlineBridgeBound_{deadlineBridgeBound},
      connectionDispatchCount_{connectionDispatchCount},
      deadlineDispatchCount_{deadlineDispatchCount},
      retiredDeadlineDrainCount_{retiredDeadlineDrainCount},
      removedConnectionCount_{removedConnectionCount},
      fatalNotificationCount_{fatalNotificationCount},
      routingProgressRevision_{routingProgressRevision},
      deadlineRoutingInProgress_{deadlineRoutingInProgress},
      shutdown_{shutdown},
      fatal_{fatal},
      failure_{std::move(failure)}
{
}

DashboardConnectionRegistry::DashboardConnectionRegistry(
    const DashboardIoCompletionKey deadlineCompletionKey,
    std::shared_ptr<IDashboardConnectionRegistryFailFast> failFast) noexcept
    : deadlineCompletionKey_{deadlineCompletionKey},
      failFast_{std::move(failFast)}
{
}

Domain::Result<std::shared_ptr<DashboardConnectionRegistry>>
DashboardConnectionRegistry::create(
    const DashboardIoCompletionKey deadlineCompletionKey) noexcept
{
    try {
        return create(
            deadlineCompletionKey,
            std::make_shared<DashboardConnectionRegistryProcessFailFast>());
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<DashboardConnectionRegistry>>::failure(
            registryError(
                Domain::ErrorCodes::InternalFailure,
                "The fixed dashboard connection registry could not allocate its fail-fast owner."));
    }
}

Domain::Result<std::shared_ptr<DashboardConnectionRegistry>>
DashboardConnectionRegistry::create(
    const DashboardIoCompletionKey deadlineCompletionKey,
    std::shared_ptr<IDashboardConnectionRegistryFailFast> failFast) noexcept
{
    using RegistryResult =
        Domain::Result<std::shared_ptr<DashboardConnectionRegistry>>;
    if (deadlineCompletionKey.value() ==
        DashboardIocpWorkerKernel::ShutdownKeyValue) {
        return RegistryResult::failure(registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry cannot use the reserved IOCP shutdown key for deadlines."));
    }
    if (failFast == nullptr) {
        return RegistryResult::failure(registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry requires a live fail-fast boundary."));
    }

    try {
        return RegistryResult::success(
            std::shared_ptr<DashboardConnectionRegistry>{
                new DashboardConnectionRegistry{
                    deadlineCompletionKey, std::move(failFast)}});
    } catch (...) {
        return RegistryResult::failure(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The fixed dashboard connection registry could not be allocated."));
    }
}

DashboardConnectionRegistry::~DashboardConnectionRegistry() noexcept
{
    beginShutdown();

    std::array<
        std::shared_ptr<IDashboardConnectionDispatchTarget>,
        MaximumConnectionCount>
        targets;
    std::array<
        std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>,
        MaximumAuxiliaryDeadlineTargetCount>
        auxiliaryTargets;
    std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
    try {
        {
            const std::scoped_lock lock{mutex_};
            for (std::size_t index{}; index < entries_.size(); ++index) {
                targets[index] = std::move(entries_[index].target);
                entries_[index].key = DashboardIoCompletionKey{0U};
                entries_[index].registrationId = 0U;
                entries_[index].generationId = 0U;
                entries_[index].deadlineHandle = {};
                entries_[index].deadlineOwnerLifecycle =
                    Entry::DeadlineOwnerLifecycle::Retired;
            }
            registeredConnectionCount_ = 0U;
            for (std::size_t index{};
                 index < auxiliaryDeadlineEntries_.size();
                 ++index) {
                auxiliaryTargets[index] = std::move(
                    auxiliaryDeadlineEntries_[index].target);
                auxiliaryDeadlineEntries_[index].registrationId = 0U;
                auxiliaryDeadlineEntries_[index].deadlineHandle = {};
                auxiliaryDeadlineEntries_[index].deadlineOwnerLifecycle =
                    Entry::DeadlineOwnerLifecycle::Retired;
            }
            registeredAuxiliaryDeadlineTargetCount_ = 0U;
            bridge = std::move(deadlineBridge_);
        }

        for (const auto& target : targets) {
            if (target != nullptr && !target->isDrained()) {
                std::terminate();
            }
        }
        if (bridge != nullptr &&
            bridge->snapshot().postedOperationCount() != 0U) {
            std::terminate();
        }

        bridge.reset();
        for (auto& target : targets) {
            target.reset();
        }
        for (auto& target : auxiliaryTargets) {
            target.reset();
        }
    } catch (...) {
        std::terminate();
    }
}

DashboardConnectionRegistry::Entry*
DashboardConnectionRegistry::findByKeyLocked(
    const DashboardIoCompletionKey key) noexcept
{
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [key](const Entry& entry) noexcept {
            return entry.target != nullptr && entry.key == key;
        });
    return found == entries_.end() ? nullptr : std::addressof(*found);
}

DashboardConnectionRegistry::Entry*
DashboardConnectionRegistry::findByRegistrationIdLocked(
    const std::uint64_t registrationId) noexcept
{
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [registrationId](const Entry& entry) noexcept {
            return entry.target != nullptr &&
                entry.registrationId == registrationId;
        });
    return found == entries_.end() ? nullptr : std::addressof(*found);
}

DashboardConnectionRegistry::Entry*
DashboardConnectionRegistry::findVacantLocked() noexcept
{
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [](const Entry& entry) noexcept { return entry.target == nullptr; });
    return found == entries_.end() ? nullptr : std::addressof(*found);
}

DashboardConnectionRegistry::AuxiliaryDeadlineEntry*
DashboardConnectionRegistry::findAuxiliaryByRegistrationIdLocked(
    const std::uint64_t registrationId) noexcept
{
    const auto found = std::find_if(
        auxiliaryDeadlineEntries_.begin(),
        auxiliaryDeadlineEntries_.end(),
        [registrationId](const AuxiliaryDeadlineEntry& entry) noexcept {
            return entry.target != nullptr &&
                entry.registrationId == registrationId;
        });
    return found == auxiliaryDeadlineEntries_.end()
        ? nullptr
        : std::addressof(*found);
}

DashboardConnectionRegistry::AuxiliaryDeadlineEntry*
DashboardConnectionRegistry::findVacantAuxiliaryLocked() noexcept
{
    const auto found = std::find_if(
        auxiliaryDeadlineEntries_.begin(),
        auxiliaryDeadlineEntries_.end(),
        [](const AuxiliaryDeadlineEntry& entry) noexcept {
            return entry.target == nullptr;
        });
    return found == auxiliaryDeadlineEntries_.end()
        ? nullptr
        : std::addressof(*found);
}

bool DashboardConnectionRegistry::hasDuplicateLocked(
    const DashboardIoCompletionKey key,
    const std::uint64_t registrationId,
    const IDashboardConnectionDispatchTarget* const target) const noexcept
{
    const bool connectionDuplicate = std::any_of(
        entries_.begin(),
        entries_.end(),
        [key, registrationId, target](const Entry& entry) noexcept {
            return entry.target != nullptr &&
                (entry.key == key ||
                 entry.registrationId == registrationId ||
                 entry.target.get() == target);
        });
    return connectionDuplicate || std::any_of(
        auxiliaryDeadlineEntries_.begin(),
        auxiliaryDeadlineEntries_.end(),
        [registrationId](const AuxiliaryDeadlineEntry& entry) noexcept {
            return entry.target != nullptr &&
                entry.registrationId == registrationId;
        });
}

Domain::Result<void> DashboardConnectionRegistry::bindDeadlineBridge(
    std::shared_ptr<DashboardDeadlineIocpBridge> bridge) noexcept
{
    if (bridge == nullptr) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry requires a deadline IOCP bridge owner.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }
    if (!(bridge->completionKey() == deadlineCompletionKey_)) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry rejected a deadline bridge configured for a different completion key.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }

    std::optional<Domain::Error> failure;
    bool structuralFailure{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard connection registry is closed to deadline bridge binding."));
            } else if (deadlineBridgeEverBound_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard connection registry deadline bridge is already bound."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else {
                deadlineBridge_ = bridge;
                deadlineBridgeEverBound_ = true;
            }
        }
        if (structuralFailure) {
            beginShutdown();
        }
        if (failure.has_value()) {
            return Domain::Result<void>::failure(std::move(*failure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        auto error = registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection registry could not bind its deadline bridge safely.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }
}

Domain::Result<void>
DashboardConnectionRegistry::bindGenerationDrainObserver(
    std::weak_ptr<IDashboardConnectionGenerationDrainObserver> observer)
    noexcept
{
    auto pinned = observer.lock();
    if (pinned == nullptr) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry requires a live generation-drain observer.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }

    std::optional<Domain::Error> failure;
    bool structuralFailure{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard connection registry is closed to generation-drain observer binding."));
            } else if (generationDrainObserverEverBound_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard generation-drain observer is already bound."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else {
                generationDrainObserver_ = std::move(observer);
                generationDrainObserverEverBound_ = true;
            }
        }
        pinned.reset();
        if (structuralFailure) {
            beginShutdown();
        }
        if (failure.has_value()) {
            return Domain::Result<void>::failure(std::move(*failure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        auto error = registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard generation-drain observer could not be bound safely.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }
}

Domain::Result<void>
DashboardConnectionRegistry::bindShutdownDrainObserver(
    std::weak_ptr<IDashboardConnectionRegistryDrainObserver> observer)
    noexcept
{
    auto pinned = observer.lock();
    if (pinned == nullptr) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry requires a live process shutdown-drain observer.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }

    std::optional<Domain::Error> failure;
    bool structuralFailure{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard connection registry is closed to process shutdown-drain observer binding."));
            } else if (shutdownDrainObserverEverBound_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard process shutdown-drain observer is already bound."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else {
                shutdownDrainObserver_ = std::move(observer);
                shutdownDrainObserverEverBound_ = true;
            }
        }
        if (structuralFailure) {
            beginShutdown();
        }
        if (failure.has_value()) {
            return Domain::Result<void>::failure(std::move(*failure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        auto error = registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard process shutdown-drain observer could not be bound safely.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }
}

Domain::Result<void>
DashboardConnectionRegistry::bindRoutingProgressObserver(
    std::weak_ptr<IDashboardConnectionRegistryRoutingProgressObserver>
        observer) noexcept
{
    try {
        auto pinned = observer.lock();
        if (pinned == nullptr) {
            auto error = registryError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard connection registry requires a live routing-progress observer.");
            failRouting(Domain::Error{error});
            return Domain::Result<void>::failure(std::move(error));
        }

        std::optional<Domain::Error> failure;
        bool structuralFailure{};
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard connection registry is closed to routing-progress observer binding."));
            } else if (routingProgressObserverEverBound_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard routing-progress observer is already bound."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else {
                routingProgressObserver_ = std::move(observer);
                routingProgressObserverEverBound_ = true;
                routingProgressPendingRevision_ =
                    routingProgressRevision_;
                routingProgressDeliveredRevision_ =
                    routingProgressRevision_;
            }
        }
        pinned.reset();
        if (structuralFailure) {
            beginShutdown();
        }
        if (failure.has_value()) {
            return Domain::Result<void>::failure(std::move(*failure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        auto error = registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard routing-progress observer could not be bound safely.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }
}

Domain::Result<void> DashboardConnectionRegistry::registerConnection(
    std::shared_ptr<IDashboardConnectionDispatchTarget> target) noexcept
{
    if (target == nullptr) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry cannot register a null connection owner.");
        failRouting(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }

    const auto key = target->completionKey();
    const auto registrationId = target->registrationId();
    const auto generationId = target->generationId();
    if (key == deadlineCompletionKey_ ||
        key.value() == DashboardIocpWorkerKernel::ShutdownKeyValue ||
        registrationId == 0U || generationId == 0U) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "A dashboard connection registry entry requires distinct non-reserved identity values.");
        failRouting(Domain::Error{error});
        target->beginShutdown();
        return Domain::Result<void>::failure(std::move(error));
    }

    std::optional<Domain::Error> failure;
    std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
    bool structuralFailure{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard connection registry is closed to new connections."));
            } else if (hasDuplicateLocked(
                           key, registrationId, target.get())) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "A dashboard connection registry identity is already registered."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else if (registeredConnectionCount_ >=
                       MaximumConnectionCount) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The dashboard connection registry reached its fixed forty-connection capacity.",
                    true));
            } else {
                bridge = deadlineBridge_;
                if (bridge == nullptr) {
                    failure.emplace(registryError(
                        Domain::ErrorCodes::Conflict,
                        "A dashboard connection cannot start before the deadline bridge is bound."));
                    retainFatalFailureLocked(Domain::Error{*failure});
                    structuralFailure = true;
                }
            }
        }

        if (failure.has_value()) {
            if (structuralFailure) {
                beginShutdown();
            }
            target->beginShutdown();
            static_cast<void>(removeIfDrained(target));
            return Domain::Result<void>::failure(std::move(*failure));
        }

        std::weak_ptr<IDashboardConnectionDrainObserver> drainObserver{
            shared_from_this()};
        auto drainBound = target->bindDrainObserver(
            std::move(drainObserver));
        if (!drainBound) {
            auto error = std::move(drainBound).error();
            target->beginShutdown();
            return Domain::Result<void>::failure(std::move(error));
        }

        failure.reset();
        structuralFailure = false;
        DashboardDeadlineNotificationHandle deadlineHandle;
        auto failureNotificationDeferral =
            bridge->deferFailureNotificationDispatch();
        {
            std::unique_lock routingLock{deadlineRoutingMutex_};
            const DeadlineRoutingProgressGuard routingProgress{
                deadlineRoutingInProgress_};
            bool deadlineOwnerAcquired{};
            try {
                {
                    const std::scoped_lock lock{mutex_};
                    if (shutdown_) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::TransportClosed,
                            "The dashboard connection registry closed before deadline owner registration."));
                    } else if (hasDuplicateLocked(
                                   key, registrationId, target.get())) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::Conflict,
                            "A dashboard connection registry identity became registered concurrently."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else if (registeredConnectionCount_ >=
                               MaximumConnectionCount) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The dashboard connection registry reached its fixed forty-connection capacity.",
                            true));
                    } else {
                        bridge = deadlineBridge_;
                        if (bridge == nullptr) {
                            failure.emplace(registryError(
                                Domain::ErrorCodes::IntegrityFailure,
                                "The dashboard connection registry lost its deadline bridge before owner registration."));
                            retainFatalFailureLocked(
                                Domain::Error{*failure});
                            structuralFailure = true;
                        }
                    }
                }

                if (!failure.has_value()) {
                    auto deadlineOwner =
                        bridge->registerOwner(registrationId);
                    if (!deadlineOwner) {
                        failure.emplace(
                            std::move(deadlineOwner).error());
                        const bool structuralBridgeFailure =
                            failure->code ==
                                Domain::ErrorCodes::Conflict ||
                            failure->code ==
                                Domain::ErrorCodes::IntegrityFailure ||
                            failure->code ==
                                Domain::ErrorCodes::InternalFailure;
                        if (structuralBridgeFailure) {
                            const std::scoped_lock lock{mutex_};
                            retainFatalFailureLocked(
                                Domain::Error{*failure});
                            structuralFailure = true;
                        }
                    } else {
                        deadlineHandle =
                            std::move(deadlineOwner).value();
                        deadlineOwnerAcquired = true;
                    }
                }

                if (deadlineOwnerAcquired) {
                    const std::scoped_lock lock{mutex_};
                    if (shutdown_) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::TransportClosed,
                            "The dashboard connection registry closed during deadline owner registration."));
                    } else if (deadlineBridge_.get() != bridge.get()) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard connection registry changed its deadline bridge during owner registration."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else if (hasDuplicateLocked(
                                   key, registrationId,
                                   target.get())) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::Conflict,
                            "A dashboard connection registry identity became registered concurrently."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else if (registeredConnectionCount_ >=
                               MaximumConnectionCount) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The dashboard connection registry reached its fixed forty-connection capacity.",
                            true));
                    } else if (auto* const vacant =
                                   findVacantLocked();
                               vacant == nullptr) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard connection registry count did not match its fixed entry array."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else {
                        vacant->key = key;
                        vacant->registrationId = registrationId;
                        vacant->generationId = generationId;
                        vacant->deadlineHandle = deadlineHandle;
                        vacant->deadlineOwnerLifecycle =
                            Entry::DeadlineOwnerLifecycle::Registered;
                        vacant->target = target;
                        ++registeredConnectionCount_;
                        deadlineOwnerAcquired = false;
                    }
                }

                if (deadlineOwnerAcquired) {
                    const auto retired =
                        bridge->retireOwner(deadlineHandle);
                    if (!retired &&
                        !bridge->snapshot().isShutdown()) {
                        const std::scoped_lock lock{mutex_};
                        retainFatalFailureLocked(registryError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard connection registry could not retire an uncommitted deadline owner."));
                        structuralFailure = true;
                    }
                }
            } catch (...) {
                if (deadlineOwnerAcquired && bridge != nullptr) {
                    static_cast<void>(
                        bridge->retireOwner(deadlineHandle));
                }
                throw;
            }
        }
        failureNotificationDeferral.release();

        if (failure.has_value()) {
            if (structuralFailure) {
                beginShutdown();
            }
            target->beginShutdown();
            return Domain::Result<void>::failure(std::move(*failure));
        }

        auto started = target->start();
        if (!started) {
            auto error = std::move(started).error();
            target->beginShutdown();
            static_cast<void>(retireDeadlineOwnerIdentity(
                key, registrationId, generationId, target));
            static_cast<void>(removeIfDrained(target));
            return Domain::Result<void>::failure(std::move(error));
        }

        static_cast<void>(removeIfDrained(target));
        return Domain::Result<void>::success();
    } catch (...) {
        auto error = registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection registry could not register and start a connection safely.");
        failRouting(Domain::Error{error});
        target->beginShutdown();
        static_cast<void>(removeIfDrained(target));
        return Domain::Result<void>::failure(std::move(error));
    }
}

Domain::Result<void>
DashboardConnectionRegistry::registerAuxiliaryDeadlineTarget(
    std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> target) noexcept
{
    if (target == nullptr || target->registrationId() == 0U) {
        auto error = registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard auxiliary deadline registry requires a nonzero owner identity.");
        failRouting(Domain::Error{error});
        if (target != nullptr) {
            target->beginShutdown();
        }
        return Domain::Result<void>::failure(std::move(error));
    }

    const auto registrationId = target->registrationId();
    std::optional<Domain::Error> failure;
    std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
    bool structuralFailure{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard deadline registry is closed to auxiliary owners."));
            } else if (
                findByRegistrationIdLocked(registrationId) != nullptr ||
                findAuxiliaryByRegistrationIdLocked(registrationId) !=
                    nullptr) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard auxiliary deadline identity is already registered."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else if (registeredAuxiliaryDeadlineTargetCount_ >=
                       MaximumAuxiliaryDeadlineTargetCount) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The two listener generations, overload responder, and shutdown drain already occupy the auxiliary deadline table.",
                    true));
            } else {
                bridge = deadlineBridge_;
                if (bridge == nullptr) {
                    failure.emplace(registryError(
                        Domain::ErrorCodes::Conflict,
                        "An auxiliary dashboard deadline owner cannot register before the bridge is bound."));
                    retainFatalFailureLocked(Domain::Error{*failure});
                    structuralFailure = true;
                }
            }
        }

        if (failure.has_value()) {
            if (structuralFailure) {
                beginShutdown();
            }
            target->beginShutdown();
            return Domain::Result<void>::failure(std::move(*failure));
        }

        failure.reset();
        structuralFailure = false;
        DashboardDeadlineNotificationHandle deadlineHandle;
        auto failureNotificationDeferral =
            bridge->deferFailureNotificationDispatch();
        {
            std::unique_lock routingLock{deadlineRoutingMutex_};
            const DeadlineRoutingProgressGuard routingProgress{
                deadlineRoutingInProgress_};
            bool deadlineOwnerAcquired{};
            try {
                {
                    const std::scoped_lock lock{mutex_};
                    if (shutdown_) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::TransportClosed,
                            "The dashboard deadline registry closed before auxiliary owner registration."));
                    } else if (
                        findByRegistrationIdLocked(registrationId) !=
                            nullptr ||
                        findAuxiliaryByRegistrationIdLocked(
                            registrationId) != nullptr) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::Conflict,
                            "The dashboard auxiliary deadline identity became registered concurrently."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else if (
                        registeredAuxiliaryDeadlineTargetCount_ >=
                            MaximumAuxiliaryDeadlineTargetCount) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The dashboard auxiliary deadline table reached its fixed capacity.",
                            true));
                    } else {
                        bridge = deadlineBridge_;
                        if (bridge == nullptr) {
                            failure.emplace(registryError(
                                Domain::ErrorCodes::IntegrityFailure,
                                "The dashboard auxiliary deadline registry lost its bridge before owner registration."));
                            retainFatalFailureLocked(
                                Domain::Error{*failure});
                            structuralFailure = true;
                        }
                    }
                }

                if (!failure.has_value()) {
                    auto deadlineOwner =
                        bridge->registerOwner(registrationId);
                    if (!deadlineOwner) {
                        failure.emplace(
                            std::move(deadlineOwner).error());
                        const bool structuralBridgeFailure =
                            failure->code ==
                                Domain::ErrorCodes::Conflict ||
                            failure->code ==
                                Domain::ErrorCodes::IntegrityFailure ||
                            failure->code ==
                                Domain::ErrorCodes::InternalFailure;
                        if (structuralBridgeFailure) {
                            const std::scoped_lock lock{mutex_};
                            retainFatalFailureLocked(
                                Domain::Error{*failure});
                            structuralFailure = true;
                        }
                    } else {
                        deadlineHandle =
                            std::move(deadlineOwner).value();
                        deadlineOwnerAcquired = true;
                    }
                }

                if (deadlineOwnerAcquired) {
                    const std::scoped_lock lock{mutex_};
                    if (shutdown_) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::TransportClosed,
                            "The dashboard deadline registry closed during auxiliary owner registration."));
                    } else if (deadlineBridge_.get() != bridge.get()) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard auxiliary deadline registry changed its bridge during owner registration."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else if (
                        findByRegistrationIdLocked(registrationId) !=
                            nullptr ||
                        findAuxiliaryByRegistrationIdLocked(
                            registrationId) != nullptr) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::Conflict,
                            "The dashboard auxiliary deadline identity became registered concurrently."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else if (
                        registeredAuxiliaryDeadlineTargetCount_ >=
                            MaximumAuxiliaryDeadlineTargetCount) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The dashboard auxiliary deadline table reached its fixed capacity.",
                            true));
                    } else if (auto* const vacant =
                                   findVacantAuxiliaryLocked();
                               vacant == nullptr) {
                        failure.emplace(registryError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard auxiliary deadline count did not match its fixed entry array."));
                        retainFatalFailureLocked(
                            Domain::Error{*failure});
                        structuralFailure = true;
                    } else {
                        vacant->registrationId = registrationId;
                        vacant->deadlineHandle = deadlineHandle;
                        vacant->deadlineOwnerLifecycle =
                            Entry::DeadlineOwnerLifecycle::Registered;
                        vacant->target = target;
                        ++registeredAuxiliaryDeadlineTargetCount_;
                        deadlineOwnerAcquired = false;
                    }
                }

                if (deadlineOwnerAcquired) {
                    const auto retired =
                        bridge->retireOwner(deadlineHandle);
                    if (!retired &&
                        !bridge->snapshot().isShutdown()) {
                        const std::scoped_lock lock{mutex_};
                        retainFatalFailureLocked(registryError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard registry could not retire an uncommitted auxiliary deadline owner."));
                        structuralFailure = true;
                    }
                }
            } catch (...) {
                if (deadlineOwnerAcquired && bridge != nullptr) {
                    static_cast<void>(
                        bridge->retireOwner(deadlineHandle));
                }
                throw;
            }
        }
        failureNotificationDeferral.release();

        if (failure.has_value()) {
            if (structuralFailure) {
                beginShutdown();
            }
            target->beginShutdown();
            return Domain::Result<void>::failure(std::move(*failure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        auto error = registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard auxiliary deadline owner could not be registered safely.");
        failRouting(Domain::Error{error});
        target->beginShutdown();
        return Domain::Result<void>::failure(std::move(error));
    }
}

bool DashboardConnectionRegistry::unregisterAuxiliaryDeadlineTarget(
    const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
    noexcept
{
    if (target == nullptr) {
        return false;
    }
    const auto registrationId = target->registrationId();
    if (!retireAuxiliaryDeadlineOwnerIdentity(
            registrationId, target)) {
        return false;
    }

    std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> removed;
    bool dispatchRoutingProgress{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            auto* const entry = findAuxiliaryByRegistrationIdLocked(
                registrationId);
            if (entry == nullptr ||
                entry->target.get() != target.get() ||
                entry->deadlineOwnerLifecycle !=
                    Entry::DeadlineOwnerLifecycle::Retired) {
                return false;
            }
            removed = std::move(entry->target);
            entry->registrationId = 0U;
            entry->deadlineHandle = {};
            entry->deadlineOwnerLifecycle =
                Entry::DeadlineOwnerLifecycle::Retired;
            --registeredAuxiliaryDeadlineTargetCount_;
            dispatchRoutingProgress = advanceRoutingProgressLocked();
        }
        removed.reset();
        if (dispatchRoutingProgress) {
            this->dispatchRoutingProgress();
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool DashboardConnectionRegistry::removeIfDrained(
    const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
    noexcept
{
    if (target == nullptr) {
        return false;
    }
    const auto key = target->completionKey();
    const auto registrationId = target->registrationId();
    const auto generationId = target->generationId();
    return removeIfDrainedIdentity(
        key, registrationId, generationId, target);
}

std::size_t DashboardConnectionRegistry::connectionCountForGeneration(
    const std::uint64_t generationId) const noexcept
{
    if (generationId == 0U) {
        return 0U;
    }
    try {
        const std::scoped_lock lock{mutex_};
        return static_cast<std::size_t>(std::count_if(
            entries_.begin(),
            entries_.end(),
            [generationId](const Entry& entry) noexcept {
                return entry.target != nullptr &&
                    entry.generationId == generationId;
            }));
    } catch (...) {
        return MaximumConnectionCount;
    }
}

std::size_t DashboardConnectionRegistry::beginShutdownGeneration(
    const std::uint64_t generationId) noexcept
{
    if (generationId == 0U) {
        return 0U;
    }
    std::array<
        std::shared_ptr<IDashboardConnectionDispatchTarget>,
        MaximumConnectionCount>
        targets;
    std::size_t targetCount{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            for (const auto& entry : entries_) {
                if (entry.target != nullptr &&
                    entry.generationId == generationId) {
                    targets[targetCount] = entry.target;
                    ++targetCount;
                }
            }
        }
        for (std::size_t index{}; index < targetCount; ++index) {
            targets[index]->beginShutdown();
        }
        for (std::size_t index{}; index < targetCount; ++index) {
            static_cast<void>(removeIfDrained(targets[index]));
        }
        return targetCount;
    } catch (...) {
        failRouting(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard registry could not close a listener generation safely."));
        return targetCount;
    }
}

bool DashboardConnectionRegistry::removeIfDrainedIdentity(
    const DashboardIoCompletionKey key,
    const std::uint64_t registrationId,
    const std::uint64_t generationId,
    const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
    noexcept
{
    if (target == nullptr || !target->isDrained()) {
        return false;
    }

    if (!retireDeadlineOwnerIdentity(
            key, registrationId, generationId, target)) {
        return false;
    }

    std::shared_ptr<IDashboardConnectionDispatchTarget> removed;
    std::shared_ptr<IDashboardConnectionGenerationDrainObserver>
        drainObserver;
    std::shared_ptr<IDashboardConnectionRegistryDrainObserver>
        shutdownDrainObserver;
    bool dispatchRoutingProgress{};
    bool missingShutdownDrainObserver{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            auto* const entry = findByKeyLocked(key);
            if (entry == nullptr ||
                entry->registrationId != registrationId ||
                entry->generationId != generationId ||
                entry->target.get() != target.get() ||
                entry->deadlineOwnerLifecycle !=
                    Entry::DeadlineOwnerLifecycle::Retired) {
                return false;
            }

            removed = std::move(entry->target);
            entry->key = DashboardIoCompletionKey{0U};
            entry->registrationId = 0U;
            entry->generationId = 0U;
            entry->deadlineHandle = {};
            entry->deadlineOwnerLifecycle =
                Entry::DeadlineOwnerLifecycle::Retired;
            --registeredConnectionCount_;
            incrementSaturating(removedConnectionCount_);
            dispatchRoutingProgress = advanceRoutingProgressLocked();
            const bool generationStillRegistered = std::any_of(
                entries_.begin(),
                entries_.end(),
                [generationId](const Entry& candidate) noexcept {
                    return candidate.target != nullptr &&
                        candidate.generationId == generationId;
                });
            if (!generationStillRegistered) {
                drainObserver = generationDrainObserver_.lock();
            }
            const bool shutdownDrainRequired =
                shutdownDrainObserverEverBound_ &&
                (gracefulShutdownCallbacksStarted_ ||
                 shutdownCallbacksStarted_);
            if (shutdownDrainRequired &&
                registeredConnectionCount_ == 0U &&
                !shutdownDrainNotificationSent_) {
                shutdownDrainObserver = shutdownDrainObserver_.lock();
                shutdownDrainNotificationSent_ = true;
                missingShutdownDrainObserver =
                    shutdownDrainObserver == nullptr;
            }
        }
        removed.reset();
        if (drainObserver != nullptr) {
            drainObserver->generationConnectionsMayHaveDrained(
                generationId);
        }
        if (shutdownDrainObserver != nullptr) {
            shutdownDrainObserver->registryConnectionsMayHaveDrained();
        } else if (missingShutdownDrainObserver) {
            failMissingShutdownDrainObserver();
        }
        if (dispatchRoutingProgress) {
            this->dispatchRoutingProgress();
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool DashboardConnectionRegistry::retireDeadlineOwnerIdentity(
    const DashboardIoCompletionKey key,
    const std::uint64_t registrationId,
    const std::uint64_t generationId,
    const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
    noexcept
{
    if (target == nullptr) {
        return false;
    }

    std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
    DashboardDeadlineNotificationHandle handle;
    try {
        {
            const std::scoped_lock lock{mutex_};
            bridge = deadlineBridge_;
        }
        DashboardDeadlineIocpBridge::FailureNotificationDeferral
            failureNotificationDeferral;
        if (bridge != nullptr) {
            failureNotificationDeferral =
                bridge->deferFailureNotificationDispatch();
        }
        std::unique_lock routingLock{deadlineRoutingMutex_};
        {
            const std::scoped_lock lock{mutex_};
            auto* const entry = findByKeyLocked(key);
            if (entry == nullptr ||
                entry->registrationId != registrationId ||
                entry->generationId != generationId ||
                entry->target.get() != target.get()) {
                return false;
            }
            if (entry->deadlineOwnerLifecycle ==
                Entry::DeadlineOwnerLifecycle::Retired) {
                return true;
            }
            if (entry->deadlineOwnerLifecycle ==
                Entry::DeadlineOwnerLifecycle::Retiring) {
                return false;
            }

            entry->deadlineOwnerLifecycle =
                Entry::DeadlineOwnerLifecycle::Retiring;
            bridge = deadlineBridge_;
            handle = entry->deadlineHandle;
        }

        bool retirementSucceeded{};
        if (bridge != nullptr) {
            auto retired = bridge->retireOwner(handle);
            retirementSucceeded = static_cast<bool>(retired);
        }

        {
            const std::scoped_lock lock{mutex_};
            auto* const entry = findByKeyLocked(key);
            if (entry == nullptr ||
                entry->registrationId != registrationId ||
                entry->generationId != generationId ||
                entry->target.get() != target.get() ||
                entry->deadlineOwnerLifecycle !=
                    Entry::DeadlineOwnerLifecycle::Retiring) {
                return false;
            }
            entry->deadlineOwnerLifecycle = retirementSucceeded
                ? Entry::DeadlineOwnerLifecycle::Retired
                : Entry::DeadlineOwnerLifecycle::Registered;
        }
        routingLock.unlock();

        if (!retirementSucceeded) {
            bool startShutdown{};
            {
                const std::scoped_lock lock{mutex_};
                retainFatalFailureLocked(
                    registryError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The dashboard connection registry could not retire its exact deadline owner."));
                startShutdown = !shutdownCallbacksStarted_;
            }
            if (startShutdown) {
                beginShutdown();
            }
        }
        return retirementSucceeded;
    } catch (...) {
        failRouting(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection registry could not retire its deadline owner safely."));
        return false;
    }
}

bool DashboardConnectionRegistry::retireAuxiliaryDeadlineOwnerIdentity(
    const std::uint64_t registrationId,
    const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
    noexcept
{
    if (target == nullptr) {
        return false;
    }

    std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
    DashboardDeadlineNotificationHandle handle;
    try {
        {
            const std::scoped_lock lock{mutex_};
            bridge = deadlineBridge_;
        }
        DashboardDeadlineIocpBridge::FailureNotificationDeferral
            failureNotificationDeferral;
        if (bridge != nullptr) {
            failureNotificationDeferral =
                bridge->deferFailureNotificationDispatch();
        }
        std::unique_lock routingLock{deadlineRoutingMutex_};
        {
            const std::scoped_lock lock{mutex_};
            auto* const entry = findAuxiliaryByRegistrationIdLocked(
                registrationId);
            if (entry == nullptr ||
                entry->target.get() != target.get()) {
                return false;
            }
            if (entry->deadlineOwnerLifecycle ==
                Entry::DeadlineOwnerLifecycle::Retired) {
                return true;
            }
            if (entry->deadlineOwnerLifecycle ==
                Entry::DeadlineOwnerLifecycle::Retiring) {
                return false;
            }
            entry->deadlineOwnerLifecycle =
                Entry::DeadlineOwnerLifecycle::Retiring;
            bridge = deadlineBridge_;
            handle = entry->deadlineHandle;
        }

        bool retirementSucceeded{};
        if (bridge != nullptr) {
            auto retired = bridge->retireOwner(handle);
            retirementSucceeded = static_cast<bool>(retired);
        }

        {
            const std::scoped_lock lock{mutex_};
            auto* const entry = findAuxiliaryByRegistrationIdLocked(
                registrationId);
            if (entry == nullptr ||
                entry->target.get() != target.get() ||
                entry->deadlineOwnerLifecycle !=
                    Entry::DeadlineOwnerLifecycle::Retiring) {
                return false;
            }
            entry->deadlineOwnerLifecycle = retirementSucceeded
                ? Entry::DeadlineOwnerLifecycle::Retired
                : Entry::DeadlineOwnerLifecycle::Registered;
        }
        routingLock.unlock();

        if (!retirementSucceeded) {
            bool startShutdown{};
            {
                const std::scoped_lock lock{mutex_};
                retainFatalFailureLocked(registryError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The dashboard registry could not retire its exact auxiliary deadline owner."));
                startShutdown = !shutdownCallbacksStarted_;
            }
            if (startShutdown) {
                beginShutdown();
            }
        }
        return retirementSucceeded;
    } catch (...) {
        failRouting(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard registry could not retire its auxiliary deadline owner safely."));
        return false;
    }
}

void DashboardConnectionRegistry::consume(
    const DashboardIoCompletionPacket packet,
    const DWORD nativeError) noexcept
{
    try {
        if (packet.completionKey == deadlineCompletionKey_) {
            std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
            {
                const std::scoped_lock lock{mutex_};
                bridge = deadlineBridge_;
            }
            if (bridge == nullptr) {
                failRouting(registryError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A dashboard deadline packet arrived before the fixed bridge was bound."));
                return;
            }

            std::optional<Domain::Error> routingFailure;
            std::optional<WindowsDashboardDeadline> deadline;
            std::shared_ptr<IDashboardConnectionDispatchTarget>
                connectionTarget;
            std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>
                auxiliaryTarget;
            bool retiredNotification{};
            bool retiredRoutingOwnershipReduced{};
            bool routingFailed{};
            bool dispatchRoutingProgress{};
            auto failureNotificationDeferral =
                bridge->deferFailureNotificationDispatch();
            {
                const std::scoped_lock routingLock{deadlineRoutingMutex_};
                const DeadlineRoutingProgressGuard routingProgress{
                    deadlineRoutingInProgress_};
                const auto bridgeBeforeReap = bridge->snapshot();
                auto reaped = bridge->reap(
                    packet.completionKey,
                    packet.transferredBytes,
                    packet.operation,
                    nativeError);
                const auto bridgeAfterReap = bridge->snapshot();
                retiredRoutingOwnershipReduced =
                    bridgeAfterReap.retiredAwaitingReapCount() <
                        bridgeBeforeReap.retiredAwaitingReapCount() ||
                    bridgeAfterReap.drainedRetiredCount() >
                        bridgeBeforeReap.drainedRetiredCount();
                if (!reaped) {
                    routingFailure.emplace(std::move(reaped).error());
                } else {
                    auto deadlineResult = std::move(reaped).value();
                    retiredNotification =
                        deadlineResult.disposition() ==
                        DashboardDeadlineIocpReapDisposition::
                            RetiredNotificationDrained;
                    retiredRoutingOwnershipReduced =
                        retiredRoutingOwnershipReduced ||
                        retiredNotification;
                    if (!retiredNotification) {
                        const auto* const delivered =
                            deadlineResult.deadline();
                        if (delivered == nullptr ||
                            delivered->registrationId == 0U ||
                            delivered->armSequence == 0U) {
                            routingFailure.emplace(registryError(
                                Domain::ErrorCodes::IntegrityFailure,
                                "A live dashboard deadline bridge result omitted its exact owner token."));
                        } else {
                            deadline.emplace(*delivered);
                            const std::scoped_lock lock{mutex_};
                            const auto* const entry =
                                findByRegistrationIdLocked(
                                    deadline->registrationId);
                            if (entry != nullptr) {
                                connectionTarget = entry->target;
                            } else {
                                const auto* const auxiliary =
                                    findAuxiliaryByRegistrationIdLocked(
                                        deadline->registrationId);
                                if (auxiliary != nullptr) {
                                    auxiliaryTarget = auxiliary->target;
                                }
                            }
                            if (connectionTarget != nullptr ||
                                auxiliaryTarget != nullptr) {
                                incrementSaturating(
                                    deadlineDispatchCount_);
                            }
                        }
                    }
                }

                // Fatal state and the exact routing reduction become visible
                // before the routing lock is released. A concurrent finalizer
                // therefore cannot observe the drained bridge slot without
                // also observing the malformed-reap failure.
                routingFailed = routingFailure.has_value();
                if (routingFailed || retiredRoutingOwnershipReduced) {
                    const std::scoped_lock lock{mutex_};
                    if (routingFailed) {
                        retainFatalFailureLocked(
                            std::move(*routingFailure));
                    }
                    if (retiredRoutingOwnershipReduced) {
                        incrementSaturating(retiredDeadlineDrainCount_);
                        dispatchRoutingProgress =
                            advanceRoutingProgressLocked();
                    }
                }
            }
            failureNotificationDeferral.release();
            if (dispatchRoutingProgress) {
                this->dispatchRoutingProgress();
            }
            if (routingFailed) {
                beginShutdown();
                return;
            }
            if (retiredNotification) {
                return;
            }
            if (connectionTarget == nullptr &&
                auxiliaryTarget == nullptr) {
                failRouting(registryError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A dashboard deadline completion targeted an unknown owner registration."));
                return;
            }

            if (connectionTarget != nullptr) {
                connectionTarget->dispatchDeadline(*deadline);
                static_cast<void>(removeIfDrained(connectionTarget));
            } else {
                auxiliaryTarget->dispatchDeadline(*deadline);
            }
            return;
        }

        if (packet.operation == nullptr) {
            failRouting(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "A dashboard connection IOCP packet omitted its exact operation address."));
            return;
        }

        std::shared_ptr<IDashboardConnectionDispatchTarget> target;
        {
            const std::scoped_lock lock{mutex_};
            const auto* const entry = findByKeyLocked(packet.completionKey);
            if (entry != nullptr) {
                target = entry->target;
                incrementSaturating(connectionDispatchCount_);
            }
        }
        if (target == nullptr) {
            failRouting(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "A dashboard IOCP packet targeted an unknown connection completion key."));
            return;
        }

        target->dispatchIocp(
            packet.transferredBytes, packet.operation, nativeError);
        static_cast<void>(removeIfDrained(target));
    } catch (...) {
        failRouting(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection registry could not route an IOCP packet safely."));
    }
}

void DashboardConnectionRegistry::fatal(const DWORD nativeError) noexcept
{
    failRouting(
        registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard IOCP worker reported a fatal native dequeue failure."),
        nativeError);
    failFast_->failFast();
}

void DashboardConnectionRegistry::fatal(
    const DashboardConnectionEventFatalNotification notification) noexcept
{
    try {
        bool knownOwner{};
        {
            const std::scoped_lock lock{mutex_};
            knownOwner = notification.ownerRegistrationId != 0U &&
                findByRegistrationIdLocked(
                    notification.ownerRegistrationId) != nullptr;
        }

        failRouting(registryError(
            knownOwner
                ? eventFailureCode(notification.failure.kind)
                : Domain::ErrorCodes::IntegrityFailure,
            knownOwner
                ? "A registered dashboard connection event bridge reported a fatal bounded-routing failure."
                : "A dashboard connection event fatal notification targeted an unknown registration.",
            knownOwner ? notification.failure.retryable : false));
    } catch (...) {
        failRouting(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection registry could not retain an event bridge fatal notification."));
    }
}

void DashboardConnectionRegistry::connectionMayHaveDrained(
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t registrationId,
    const std::uint64_t generationId) noexcept
{
    std::shared_ptr<IDashboardConnectionDispatchTarget> target;
    try {
        {
            const std::scoped_lock lock{mutex_};
            const auto* const entry = findByKeyLocked(completionKey);
            if (entry != nullptr &&
                entry->registrationId == registrationId &&
                entry->generationId == generationId) {
                target = entry->target;
            }
        }
        if (target != nullptr) {
            static_cast<void>(removeIfDrainedIdentity(
                completionKey,
                registrationId,
                generationId,
                target));
        }
    } catch (...) {
        failRouting(registryError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard registry could not consume a connection drain edge safely."));
    }
}

void DashboardConnectionRegistry::retainFatalFailureLocked(
    Domain::Error error,
    const std::optional<DWORD> nativeError) noexcept
{
    shutdown_ = true;
    fatal_ = true;
    incrementSaturating(fatalNotificationCount_);
    if (!firstFailureSnapshot_.has_value()) {
        firstFailureSnapshot_.emplace(DashboardConnectionRegistryFailure{
            classifyFailureKind(error), error.retryable, nativeError});
    }
    if (!firstFailure_.has_value()) {
        firstFailure_.emplace(std::move(error));
    }
}

void DashboardConnectionRegistry::failRouting(
    Domain::Error error,
    const std::optional<DWORD> nativeError) noexcept
{
    try {
        {
            const std::scoped_lock lock{mutex_};
            retainFatalFailureLocked(std::move(error), nativeError);
        }
        beginShutdown();
    } catch (...) {
        std::terminate();
    }
}

void DashboardConnectionRegistry::failMissingShutdownDrainObserver()
    noexcept
{
    failRouting(registryError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard process shutdown-drain observer was not retained through the exact zero-connection edge."));
    failFast_->failFast();
}

void DashboardConnectionRegistry::failMissingRoutingProgressObserver()
    noexcept
{
    failRouting(registryError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard routing-progress observer was not retained through an exact routing-ownership reduction."));
    failFast_->failFast();
}

bool
DashboardConnectionRegistry::advanceRoutingProgressLocked() noexcept
{
    incrementSaturating(routingProgressRevision_);
    routingProgressPendingRevision_ = routingProgressRevision_;
    if (routingProgressObserverEverBound_ &&
        !routingProgressObserverFailureReported_ &&
        !routingProgressDispatchInProgress_ &&
        routingProgressPendingRevision_ >
            routingProgressDeliveredRevision_) {
        routingProgressDispatchInProgress_ = true;
        return true;
    }
    return false;
}

void DashboardConnectionRegistry::dispatchRoutingProgress() noexcept
{
    try {
        for (;;) {
            std::shared_ptr<
                IDashboardConnectionRegistryRoutingProgressObserver>
                observer;
            std::uint64_t revision{};
            bool missingRequiredObserver{};
            {
                const std::scoped_lock lock{mutex_};
                if (!routingProgressDispatchInProgress_) {
                    return;
                }
                if (routingProgressObserverFailureReported_ ||
                    routingProgressDeliveredRevision_ >=
                        routingProgressPendingRevision_) {
                    routingProgressDispatchInProgress_ = false;
                    return;
                }

                observer = routingProgressObserver_.lock();
                if (observer == nullptr) {
                    routingProgressObserverFailureReported_ = true;
                    routingProgressDispatchInProgress_ = false;
                    missingRequiredObserver = true;
                } else {
                    revision = routingProgressPendingRevision_;
                    routingProgressDeliveredRevision_ = revision;
                }
            }

            if (missingRequiredObserver) {
                failMissingRoutingProgressObserver();
                return;
            }
            observer->registryRoutingMayHaveProgressed(revision);
            observer.reset();
        }
    } catch (...) {
        std::terminate();
    }
}

DashboardConnectionRegistrySnapshot
DashboardConnectionRegistry::snapshotLocked() const noexcept
{
    return DashboardConnectionRegistrySnapshot{
        registeredConnectionCount_,
        MaximumConnectionCount,
        registeredAuxiliaryDeadlineTargetCount_,
        deadlineCompletionKey_,
        deadlineBridge_ != nullptr,
        connectionDispatchCount_,
        deadlineDispatchCount_,
        retiredDeadlineDrainCount_,
        removedConnectionCount_,
        fatalNotificationCount_,
        routingProgressRevision_,
        deadlineRoutingInProgress_.load(std::memory_order_acquire),
        shutdown_,
        fatal_,
        firstFailureSnapshot_};
}

DashboardConnectionRegistrySnapshot DashboardConnectionRegistry::snapshot()
    const noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        return snapshotLocked();
    } catch (...) {
        return DashboardConnectionRegistrySnapshot{
            0U,
            MaximumConnectionCount,
            0U,
            deadlineCompletionKey_,
            false,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
            false,
            true,
            true,
            DashboardConnectionRegistryFailure{
                DashboardConnectionRegistryFailureKind::InternalFailure,
                false,
                std::nullopt}};
    }
}

std::optional<Domain::Error>
DashboardConnectionRegistry::fullFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

void DashboardConnectionRegistry::beginGracefulShutdown() noexcept
{
    std::array<
        std::shared_ptr<IDashboardConnectionDispatchTarget>,
        MaximumConnectionCount>
        targets;
    std::shared_ptr<IDashboardConnectionRegistryDrainObserver>
        shutdownDrainObserver;
    bool missingShutdownDrainObserver{};
    try {
        const std::lock_guard fanoutLock{shutdownFanoutMutex_};
        {
            const std::scoped_lock lock{mutex_};
            shutdown_ = true;
            if (gracefulShutdownCallbacksStarted_ ||
                shutdownCallbacksStarted_) {
                return;
            }
            gracefulShutdownCallbacksStarted_ = true;
            for (std::size_t index{}; index < entries_.size(); ++index) {
                targets[index] = entries_[index].target;
            }
            if (registeredConnectionCount_ == 0U &&
                shutdownDrainObserverEverBound_ &&
                !shutdownDrainNotificationSent_) {
                shutdownDrainObserver = shutdownDrainObserver_.lock();
                shutdownDrainNotificationSent_ = true;
                missingShutdownDrainObserver =
                    shutdownDrainObserver == nullptr;
            }
        }

        for (const auto& target : targets) {
            {
                const std::scoped_lock lock{mutex_};
                if (shutdownCallbacksStarted_) {
                    break;
                }
            }
            if (target != nullptr) {
                target->beginGracefulShutdown();
            }
        }
        for (const auto& target : targets) {
            if (target != nullptr) {
                static_cast<void>(removeIfDrained(target));
            }
        }
        if (shutdownDrainObserver != nullptr) {
            shutdownDrainObserver->registryConnectionsMayHaveDrained();
        } else if (missingShutdownDrainObserver) {
            failMissingShutdownDrainObserver();
        }
    } catch (...) {
        std::terminate();
    }
}

void DashboardConnectionRegistry::beginShutdown() noexcept
{
    std::array<
        std::shared_ptr<IDashboardConnectionDispatchTarget>,
        MaximumConnectionCount>
        targets;
    std::array<
        std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>,
        MaximumAuxiliaryDeadlineTargetCount>
        auxiliaryTargets;
    std::shared_ptr<IDashboardConnectionRegistryDrainObserver>
        shutdownDrainObserver;
    bool missingShutdownDrainObserver{};
    try {
        const std::lock_guard fanoutLock{shutdownFanoutMutex_};
        {
            const std::scoped_lock lock{mutex_};
            shutdown_ = true;
            if (shutdownCallbacksStarted_) {
                return;
            }
            shutdownCallbacksStarted_ = true;
            for (std::size_t index{}; index < entries_.size(); ++index) {
                targets[index] = entries_[index].target;
            }
            for (std::size_t index{};
                 index < auxiliaryDeadlineEntries_.size();
                 ++index) {
                auxiliaryTargets[index] =
                    auxiliaryDeadlineEntries_[index].target;
            }
            if (registeredConnectionCount_ == 0U &&
                shutdownDrainObserverEverBound_ &&
                !shutdownDrainNotificationSent_) {
                shutdownDrainObserver = shutdownDrainObserver_.lock();
                shutdownDrainNotificationSent_ = true;
                missingShutdownDrainObserver =
                    shutdownDrainObserver == nullptr;
            }
        }

        for (const auto& target : targets) {
            if (target != nullptr) {
                target->beginShutdown();
            }
        }
        for (const auto& target : auxiliaryTargets) {
            if (target != nullptr) {
                target->beginShutdown();
            }
        }
        for (const auto& target : targets) {
            if (target != nullptr) {
                static_cast<void>(retireDeadlineOwnerIdentity(
                    target->completionKey(),
                    target->registrationId(),
                    target->generationId(),
                    target));
                static_cast<void>(removeIfDrained(target));
            }
        }
        if (shutdownDrainObserver != nullptr) {
            shutdownDrainObserver->registryConnectionsMayHaveDrained();
        } else if (missingShutdownDrainObserver) {
            failMissingShutdownDrainObserver();
        }
    } catch (...) {
        std::terminate();
    }
}

Domain::Result<DashboardDeadlineRoutingFinalizeDisposition>
DashboardConnectionRegistry::finalizeDeadlineRouting() noexcept
{
    using FinalizeResult = Domain::Result<
        DashboardDeadlineRoutingFinalizeDisposition>;
    try {
        std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
        {
            const std::scoped_lock lock{mutex_};
            bridge = deadlineBridge_;
        }
        DashboardDeadlineIocpBridge::FailureNotificationDeferral
            failureNotificationDeferral;
        if (bridge != nullptr) {
            failureNotificationDeferral =
                bridge->deferFailureNotificationDispatch();
        }
        const std::scoped_lock routingLock{deadlineRoutingMutex_};
        std::size_t registeredConnectionCount{};
        std::size_t registeredAuxiliaryCount{};
        bool bridgeEverBound{};
        bool shutdown{};
        bool fatal{};
        {
            const std::scoped_lock lock{mutex_};
            bridge = deadlineBridge_;
            registeredConnectionCount = registeredConnectionCount_;
            registeredAuxiliaryCount =
                registeredAuxiliaryDeadlineTargetCount_;
            bridgeEverBound = deadlineBridgeEverBound_;
            shutdown = shutdown_;
            fatal = fatal_;
        }

        if (fatal) {
            return FinalizeResult::failure(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "Fatal dashboard connection routing cannot be finalized as a successful drain."));
        }
        if (!shutdown) {
            return FinalizeResult::failure(registryError(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard deadline routing cannot be finalized before registry shutdown begins."));
        }
        if (bridge == nullptr) {
            if (!bridgeEverBound && registeredConnectionCount == 0U &&
                registeredAuxiliaryCount == 0U) {
                return FinalizeResult::success(
                    DashboardDeadlineRoutingFinalizeDisposition::
                        Finalized);
            }
            return FinalizeResult::failure(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "Dashboard deadline routing lost its bound bridge before finalization."));
        }
        const auto bridgeSnapshot = bridge->snapshot();
        if (bridgeSnapshot.isFatal()) {
            return FinalizeResult::failure(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "Fatal dashboard deadline bridge routing cannot be finalized as a successful drain."));
        }
        if (registeredConnectionCount != 0U ||
            registeredAuxiliaryCount != 0U) {
            return FinalizeResult::success(
                DashboardDeadlineRoutingFinalizeDisposition::Pending);
        }

        const auto bridgeRegistered =
            bridgeSnapshot.registeredOwnerCount();
        const auto bridgePosted = bridgeSnapshot.postedOperationCount();
        const auto bridgeRetired =
            bridgeSnapshot.retiredAwaitingReapCount();
        if (bridgeRegistered != 0U) {
            return FinalizeResult::failure(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "Dashboard deadline routing retained a live bridge owner after every registry route was removed."));
        }
        if (bridgePosted != bridgeRetired) {
            return FinalizeResult::failure(registryError(
                Domain::ErrorCodes::IntegrityFailure,
                "Dashboard deadline routing retained a posted operation that was not an exact retired tombstone."));
        }
        if (bridgeRetired != 0U) {
            return FinalizeResult::success(
                DashboardDeadlineRoutingFinalizeDisposition::Pending);
        }
        bridge->shutdown();
        return FinalizeResult::success(
            DashboardDeadlineRoutingFinalizeDisposition::Finalized);
    } catch (...) {
        return FinalizeResult::failure(registryError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard deadline routing could not be finalized safely."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
