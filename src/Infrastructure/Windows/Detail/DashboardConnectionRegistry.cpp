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

} // namespace

DashboardConnectionRegistrySnapshot::DashboardConnectionRegistrySnapshot(
    const std::size_t registeredConnectionCount,
    const std::size_t maximumConnectionCount,
    const DashboardIoCompletionKey deadlineCompletionKey,
    const bool deadlineBridgeBound,
    const std::uint64_t connectionDispatchCount,
    const std::uint64_t deadlineDispatchCount,
    const std::uint64_t retiredDeadlineDrainCount,
    const std::uint64_t removedConnectionCount,
    const std::uint64_t fatalNotificationCount,
    const bool deadlineRoutingInProgress,
    const bool shutdown,
    const bool fatal,
    std::optional<DashboardConnectionRegistryFailure> failure) noexcept
    : registeredConnectionCount_{registeredConnectionCount},
      maximumConnectionCount_{maximumConnectionCount},
      deadlineCompletionKey_{deadlineCompletionKey},
      deadlineBridgeBound_{deadlineBridgeBound},
      connectionDispatchCount_{connectionDispatchCount},
      deadlineDispatchCount_{deadlineDispatchCount},
      retiredDeadlineDrainCount_{retiredDeadlineDrainCount},
      removedConnectionCount_{removedConnectionCount},
      fatalNotificationCount_{fatalNotificationCount},
      deadlineRoutingInProgress_{deadlineRoutingInProgress},
      shutdown_{shutdown},
      fatal_{fatal},
      failure_{std::move(failure)}
{
}

DashboardConnectionRegistry::DashboardConnectionRegistry(
    const DashboardIoCompletionKey deadlineCompletionKey) noexcept
    : deadlineCompletionKey_{deadlineCompletionKey}
{
}

Domain::Result<std::shared_ptr<DashboardConnectionRegistry>>
DashboardConnectionRegistry::create(
    const DashboardIoCompletionKey deadlineCompletionKey) noexcept
{
    using RegistryResult =
        Domain::Result<std::shared_ptr<DashboardConnectionRegistry>>;
    if (deadlineCompletionKey.value() ==
        DashboardIocpWorkerKernel::ShutdownKeyValue) {
        return RegistryResult::failure(registryError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard connection registry cannot use the reserved IOCP shutdown key for deadlines."));
    }

    try {
        return RegistryResult::success(
            std::shared_ptr<DashboardConnectionRegistry>{
                new DashboardConnectionRegistry{deadlineCompletionKey}});
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

bool DashboardConnectionRegistry::hasDuplicateLocked(
    const DashboardIoCompletionKey key,
    const std::uint64_t registrationId,
    const IDashboardConnectionDispatchTarget* const target) const noexcept
{
    return std::any_of(
        entries_.begin(),
        entries_.end(),
        [key, registrationId, target](const Entry& entry) noexcept {
            return entry.target != nullptr &&
                (entry.key == key ||
                 entry.registrationId == registrationId ||
                 entry.target.get() == target);
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

        auto deadlineOwner = bridge->registerOwner(registrationId);
        if (!deadlineOwner) {
            auto error = std::move(deadlineOwner).error();
            if (error.code == Domain::ErrorCodes::Conflict ||
                error.code == Domain::ErrorCodes::IntegrityFailure ||
                error.code == Domain::ErrorCodes::InternalFailure) {
                failRouting(Domain::Error{error});
            }
            target->beginShutdown();
            return Domain::Result<void>::failure(std::move(error));
        }
        const auto deadlineHandle = std::move(deadlineOwner).value();

        failure.reset();
        structuralFailure = false;
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard connection registry closed during deadline owner registration."));
            } else if (hasDuplicateLocked(
                           key, registrationId, target.get())) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::Conflict,
                    "A dashboard connection registry identity became registered concurrently."));
                retainFatalFailureLocked(Domain::Error{*failure});
                structuralFailure = true;
            } else if (registeredConnectionCount_ >=
                       MaximumConnectionCount) {
                failure.emplace(registryError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The dashboard connection registry reached its fixed forty-connection capacity.",
                    true));
            } else {
                auto* const vacant = findVacantLocked();
                if (vacant == nullptr) {
                    failure.emplace(registryError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The dashboard connection registry count did not match its fixed entry array."));
                    retainFatalFailureLocked(Domain::Error{*failure});
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
                }
            }
        }

        if (failure.has_value()) {
            const auto retired = bridge->retireOwner(deadlineHandle);
            if (!retired && !bridge->snapshot().isShutdown()) {
                failRouting(registryError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The dashboard connection registry could not retire an uncommitted deadline owner."));
            }
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
        }
        removed.reset();
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
            std::shared_ptr<IDashboardConnectionDispatchTarget> target;
            bool retiredNotification{};
            {
                const std::scoped_lock routingLock{deadlineRoutingMutex_};
                const DeadlineRoutingProgressGuard routingProgress{
                    deadlineRoutingInProgress_};
                auto reaped = bridge->reap(
                    packet.completionKey,
                    packet.transferredBytes,
                    packet.operation,
                    nativeError);
                if (!reaped) {
                    routingFailure.emplace(std::move(reaped).error());
                } else {
                    auto deadlineResult = std::move(reaped).value();
                    retiredNotification =
                        deadlineResult.disposition() ==
                        DashboardDeadlineIocpReapDisposition::
                            RetiredNotificationDrained;
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
                                target = entry->target;
                                incrementSaturating(
                                    deadlineDispatchCount_);
                            }
                        }
                    }
                }
            }

            if (routingFailure.has_value()) {
                failRouting(std::move(*routingFailure));
                return;
            }
            if (retiredNotification) {
                const std::scoped_lock lock{mutex_};
                incrementSaturating(retiredDeadlineDrainCount_);
                return;
            }
            if (target == nullptr) {
                failRouting(registryError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A dashboard deadline completion targeted an unknown connection registration."));
                return;
            }

            target->dispatchDeadline(*deadline);
            static_cast<void>(removeIfDrained(target));
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

DashboardConnectionRegistrySnapshot
DashboardConnectionRegistry::snapshotLocked() const noexcept
{
    return DashboardConnectionRegistrySnapshot{
        registeredConnectionCount_,
        MaximumConnectionCount,
        deadlineCompletionKey_,
        deadlineBridge_ != nullptr,
        connectionDispatchCount_,
        deadlineDispatchCount_,
        retiredDeadlineDrainCount_,
        removedConnectionCount_,
        fatalNotificationCount_,
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
            deadlineCompletionKey_,
            false,
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

void DashboardConnectionRegistry::beginShutdown() noexcept
{
    std::array<
        std::shared_ptr<IDashboardConnectionDispatchTarget>,
        MaximumConnectionCount>
        targets;
    std::shared_ptr<DashboardDeadlineIocpBridge> bridge;
    try {
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
            bridge = deadlineBridge_;
        }

        for (const auto& target : targets) {
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
        if (bridge != nullptr) {
            bridge->shutdown();
        }
    } catch (...) {
        std::terminate();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
