#include "Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error bridgeError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard deadline IOCP bridge operation failed and its diagnostic could not be retained.",
            retryable);
    }
}

[[nodiscard]] Domain::Error integrityError(std::string message) noexcept
{
    return bridgeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] DashboardDeadlineIocpFailure classifyFailure(
    const Domain::Error& error) noexcept
{
    DashboardDeadlineIocpFailureKind kind{
        DashboardDeadlineIocpFailureKind::Other};
    if (error.code == Domain::ErrorCodes::InvalidRequest) {
        kind = DashboardDeadlineIocpFailureKind::InvalidRequest;
    } else if (error.code == Domain::ErrorCodes::Conflict) {
        kind = DashboardDeadlineIocpFailureKind::Conflict;
    } else if (error.code == Domain::ErrorCodes::LimitExceeded) {
        kind = DashboardDeadlineIocpFailureKind::LimitExceeded;
    } else if (error.code == Domain::ErrorCodes::TransportClosed) {
        kind = DashboardDeadlineIocpFailureKind::TransportClosed;
    } else if (error.code == Domain::ErrorCodes::IntegrityFailure) {
        kind = DashboardDeadlineIocpFailureKind::IntegrityFailure;
    } else if (error.code == Domain::ErrorCodes::InternalFailure) {
        kind = DashboardDeadlineIocpFailureKind::InternalFailure;
    }
    return DashboardDeadlineIocpFailure{kind, error.retryable};
}

} // namespace

DashboardDeadlineIocpReapResult::DashboardDeadlineIocpReapResult(
    const DashboardDeadlineIocpReapDisposition disposition,
    std::optional<WindowsDashboardDeadline> deadline) noexcept
    : disposition_{disposition}, deadline_{std::move(deadline)}
{
}

DashboardDeadlineIocpSnapshot::DashboardDeadlineIocpSnapshot(
    const std::size_t registeredOwnerCount,
    const std::size_t postedOperationCount,
    const std::size_t retiredAwaitingReapCount,
    const std::size_t maximumOwnerCount,
    const std::uint64_t successfulPostCount,
    const std::uint64_t coalescedSignalCount,
    const std::uint64_t deliveredDeadlineCount,
    const std::uint64_t drainedRetiredCount,
    const bool shutdown,
    const bool fatal,
    std::optional<DashboardDeadlineIocpFailure> failure) noexcept
    : registeredOwnerCount_{registeredOwnerCount},
      postedOperationCount_{postedOperationCount},
      retiredAwaitingReapCount_{retiredAwaitingReapCount},
      maximumOwnerCount_{maximumOwnerCount},
      successfulPostCount_{successfulPostCount},
      coalescedSignalCount_{coalescedSignalCount},
      deliveredDeadlineCount_{deliveredDeadlineCount},
      drainedRetiredCount_{drainedRetiredCount},
      shutdown_{shutdown},
      fatal_{fatal},
      failure_{std::move(failure)}
{
}

DashboardDeadlineIocpBridge::DashboardDeadlineIocpBridge(
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey,
    std::unique_ptr<DashboardDeadlineNotificationMailbox> mailbox,
    SlotOwners slots) noexcept
    : kernel_{std::addressof(kernel)},
      completionKey_{completionKey},
      mailbox_{std::move(mailbox)},
      slots_{std::move(slots)}
{
}

DashboardDeadlineIocpBridge::FailureNotificationDeferral::
    FailureNotificationDeferral(
        DashboardDeadlineIocpBridge& owner) noexcept
    : owner_{std::addressof(owner)}
{
}

DashboardDeadlineIocpBridge::FailureNotificationDeferral::
    FailureNotificationDeferral(
        FailureNotificationDeferral&& other) noexcept
    : owner_{std::exchange(other.owner_, nullptr)}
{
}

DashboardDeadlineIocpBridge::FailureNotificationDeferral&
DashboardDeadlineIocpBridge::FailureNotificationDeferral::operator=(
    FailureNotificationDeferral&& other) noexcept
{
    if (this != std::addressof(other)) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
}

DashboardDeadlineIocpBridge::FailureNotificationDeferral::
    ~FailureNotificationDeferral() noexcept
{
    release();
}

void DashboardDeadlineIocpBridge::FailureNotificationDeferral::release()
    noexcept
{
    auto* const owner = std::exchange(owner_, nullptr);
    if (owner != nullptr) {
        owner->releaseFailureNotificationDeferral();
    }
}

class DashboardDeadlineIocpBridge::FailureDispatchGuard final {
public:
    explicit FailureDispatchGuard(
        DashboardDeadlineIocpBridge& owner) noexcept
        : owner_{owner}
    {
    }

    ~FailureDispatchGuard() noexcept
    {
        owner_.dispatchFatalFailureIfRequired();
    }

    FailureDispatchGuard(const FailureDispatchGuard&) = delete;
    FailureDispatchGuard& operator=(const FailureDispatchGuard&) = delete;

private:
    DashboardDeadlineIocpBridge& owner_;
};

DashboardDeadlineIocpBridge::~DashboardDeadlineIocpBridge() noexcept
{
    shutdown();
    const std::scoped_lock lock{mutex_};
    for (const auto& slot : slots_) {
        if (slot->lifecycle == SlotLifecycle::Posted ||
            slot->lifecycle == SlotLifecycle::PostedRetired) {
            // A queued packet still contains this slot's address. Destroying
            // it would convert a bounded drain obligation into use-after-free.
            std::terminate();
        }
    }
}

Domain::Result<void>
DashboardDeadlineIocpBridge::bindFailureObserver(
    std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver> observer)
    noexcept
{
    try {
        auto pinned = observer.lock();
        if (pinned == nullptr) {
            return Domain::Result<void>::failure(bridgeError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard deadline IOCP bridge requires a live failure observer."));
        }

        const std::scoped_lock lock{mutex_};
        if (shutdown_ || fatal_) {
            return Domain::Result<void>::failure(bridgeError(
                Domain::ErrorCodes::TransportClosed,
                "The dashboard deadline IOCP bridge is closed to failure-observer binding."));
        }
        if (failureObserverEverBound_) {
            return Domain::Result<void>::failure(bridgeError(
                Domain::ErrorCodes::Conflict,
                "The dashboard deadline IOCP bridge failure observer is already bound."));
        }
        failureObserver_ = std::move(observer);
        failureObserverEverBound_ = true;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(bridgeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline IOCP bridge failure observer could not be bound safely."));
    }
}

DashboardDeadlineIocpBridge::FailureNotificationDeferral
DashboardDeadlineIocpBridge::deferFailureNotificationDispatch() noexcept
{
    try {
        std::unique_lock lock{failureNotificationDispatchMutex_};
        failureNotificationDispatchChanged_.wait(
            lock,
            [this] { return !failureNotificationDispatchInProgress_; });
        ++failureNotificationDeferralCount_;
        return FailureNotificationDeferral{*this};
    } catch (...) {
        std::terminate();
    }
}

Domain::Result<std::shared_ptr<DashboardDeadlineIocpBridge>>
DashboardDeadlineIocpBridge::create(
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey) noexcept
{
    using BridgeResult =
        Domain::Result<std::shared_ptr<DashboardDeadlineIocpBridge>>;
    if (completionKey.value() ==
        DashboardIocpWorkerKernel::ShutdownKeyValue) {
        return BridgeResult::failure(bridgeError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard deadline IOCP bridge cannot use the reserved shutdown key."));
    }

    auto mailbox = DashboardDeadlineNotificationMailbox::create(SlotCount);
    if (!mailbox) {
        return BridgeResult::failure(std::move(mailbox).error());
    }

    try {
        SlotOwners slots{};
        for (auto& slot : slots) {
            slot = std::make_unique<NotificationSlot>();
        }
        return BridgeResult::success(
            std::shared_ptr<DashboardDeadlineIocpBridge>{
                new DashboardDeadlineIocpBridge{
                    kernel,
                    completionKey,
                    std::move(mailbox).value(),
                    std::move(slots)}});
    } catch (...) {
        return BridgeResult::failure(bridgeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline IOCP bridge could not allocate its 44 stable notification slots."));
    }
}

Domain::Result<DashboardDeadlineNotificationHandle>
DashboardDeadlineIocpBridge::registerOwner(
    const std::uint64_t registrationId) noexcept
{
    using RegisterResult =
        Domain::Result<DashboardDeadlineNotificationHandle>;
    FailureDispatchGuard failureDispatch{*this};
    try {
        const std::scoped_lock lock{mutex_};
        auto registered = mailbox_->registerOwner(registrationId);
        if (!registered) {
            return RegisterResult::failure(std::move(registered).error());
        }

        const auto handle = std::move(registered).value();
        if (handle.slotIndex >= SlotCount) {
            retainFatalFailureLocked(integrityError(
                "The dashboard deadline mailbox returned an out-of-range slot index."));
            return RegisterResult::failure(integrityError(
                "The dashboard deadline mailbox returned an out-of-range slot index."));
        }

        auto& slot = *slots_[handle.slotIndex];
        if (slot.lifecycle != SlotLifecycle::Vacant ||
            slot.handle.registrationId != 0U ||
            slot.handle.generation != 0U) {
            static_cast<void>(mailbox_->retire(handle));
            retainFatalFailureLocked(integrityError(
                "The dashboard deadline mailbox selected a non-vacant IOCP notification slot."));
            return RegisterResult::failure(integrityError(
                "The dashboard deadline mailbox selected a non-vacant IOCP notification slot."));
        }

        slot.handle = handle;
        slot.lifecycle = SlotLifecycle::Registered;
        return RegisterResult::success(handle);
    } catch (...) {
        return RegisterResult::failure(bridgeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline IOCP bridge could not register an owner."));
    }
}

Domain::Result<void> DashboardDeadlineIocpBridge::retireOwner(
    const DashboardDeadlineNotificationHandle& handle) noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        if (handle.slotIndex >= SlotCount ||
            !(slots_[handle.slotIndex]->handle == handle)) {
            return Domain::Result<void>::failure(bridgeError(
                Domain::ErrorCodes::Conflict,
                "The dashboard deadline IOCP retirement handle is stale or belongs to another owner."));
        }

        auto& slot = *slots_[handle.slotIndex];
        if (slot.lifecycle != SlotLifecycle::Registered &&
            slot.lifecycle != SlotLifecycle::Posted) {
            return Domain::Result<void>::failure(bridgeError(
                Domain::ErrorCodes::Conflict,
                "The dashboard deadline IOCP owner was already retired."));
        }
        if (!mailbox_->retire(handle)) {
            return Domain::Result<void>::failure(integrityError(
                "The dashboard deadline mailbox rejected an exact live retirement handle."));
        }

        if (slot.lifecycle == SlotLifecycle::Posted) {
            slot.lifecycle = SlotLifecycle::PostedRetired;
        } else {
            slot.handle = {};
            slot.operation = {};
            slot.lifecycle = SlotLifecycle::Vacant;
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(bridgeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline IOCP bridge could not retire an owner."));
    }
}

void DashboardDeadlineIocpBridge::signal(
    WindowsDashboardDeadline deadline) noexcept
{
    FailureDispatchGuard failureDispatch{*this};
    try {
        const std::scoped_lock lock{mutex_};
        if (shutdown_) {
            return;
        }

        auto published = mailbox_->publish(std::move(deadline));
        if (!published) {
            auto error = std::move(published).error();
            // A deadline may leave the scheduler immediately before its owner
            // retires. That exact late signal is an ordinary closed-owner
            // race; all other publication failures are fatal invariants.
            if (error.code != Domain::ErrorCodes::TransportClosed) {
                retainFatalFailureLocked(std::move(error));
            }
            return;
        }

        const auto publication = std::move(published).value();
        const auto& handle = publication.handle;
        if (handle.slotIndex >= SlotCount) {
            retainFatalFailureLocked(integrityError(
                "The dashboard deadline publication referenced an out-of-range IOCP slot."));
            return;
        }

        auto& slot = *slots_[handle.slotIndex];
        if (publication.disposition ==
            DashboardDeadlinePublishDisposition::Coalesced) {
            if (slot.lifecycle != SlotLifecycle::Posted ||
                !(slot.handle == handle)) {
                retainFatalFailureLocked(integrityError(
                    "A coalesced dashboard deadline did not match its one posted notification slot."));
                return;
            }
            ++coalescedSignalCount_;
            return;
        }

        if (slot.lifecycle != SlotLifecycle::Registered ||
            !(slot.handle == handle)) {
            static_cast<void>(mailbox_->retire(handle));
            static_cast<void>(mailbox_->take(handle));
            retainFatalFailureLocked(integrityError(
                "A new dashboard deadline notification did not match a registered idle IOCP slot."));
            return;
        }

        slot.operation = {};
        slot.lifecycle = SlotLifecycle::Posted;
        auto posted = kernel_->postAdmitted(
            0U, completionKey_, std::addressof(slot.operation));
        if (!posted) {
            // No kernel packet exists on a failed post. Retire and take the
            // exact generation while it is still known, then vacate the
            // stable slot before making fatal state observable.
            auto error = std::move(posted).error();
            drainExactPostedHandleLocked(handle.slotIndex);
            retainFatalFailureLocked(std::move(error));
            return;
        }

        ++successfulPostCount_;
    } catch (...) {
        try {
            const std::scoped_lock lock{mutex_};
            retainFatalFailureLocked(bridgeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard deadline IOCP bridge could not publish a notification safely."));
        } catch (...) {
            std::terminate();
        }
    }
}

Domain::Result<std::size_t>
DashboardDeadlineIocpBridge::findOperationLocked(
    OVERLAPPED* const operation) const noexcept
{
    if (operation == nullptr) {
        return Domain::Result<std::size_t>::failure(integrityError(
            "A dashboard deadline IOCP completion carried a null OVERLAPPED pointer."));
    }
    for (std::size_t index{}; index < SlotCount; ++index) {
        if (std::addressof(slots_[index]->operation) == operation) {
            return Domain::Result<std::size_t>::success(index);
        }
    }
    return Domain::Result<std::size_t>::failure(integrityError(
        "A dashboard deadline IOCP completion did not match any of the 44 owned OVERLAPPED slots."));
}

bool DashboardDeadlineIocpBridge::validatePostedHandleLocked(
    const std::size_t index) const noexcept
{
    if (index >= SlotCount) {
        return false;
    }
    const auto& slot = *slots_[index];
    return (slot.lifecycle == SlotLifecycle::Posted ||
            slot.lifecycle == SlotLifecycle::PostedRetired) &&
        slot.handle.slotIndex == index &&
        slot.handle.generation != 0U &&
        slot.handle.registrationId != 0U;
}

void DashboardDeadlineIocpBridge::drainExactPostedHandleLocked(
    const std::size_t index) noexcept
{
    if (index >= SlotCount) {
        return;
    }
    auto& slot = *slots_[index];
    const bool wasRetired =
        slot.lifecycle == SlotLifecycle::PostedRetired;
    if (!wasRetired && !mailbox_->retire(slot.handle)) {
        // The bridge lock makes this impossible for an exact live posted
        // generation. Continuing would report failure while knowingly
        // retaining a tombstone that no later packet can reap.
        std::terminate();
    }
    static_cast<void>(mailbox_->take(slot.handle));
    slot.handle = {};
    slot.operation = {};
    slot.lifecycle = SlotLifecycle::Vacant;
    if (wasRetired) {
        ++drainedRetiredCount_;
    }
}

Domain::Result<DashboardDeadlineIocpReapResult>
DashboardDeadlineIocpBridge::reap(
    const DashboardIoCompletionKey completionKey,
    const DWORD transferredBytes,
    OVERLAPPED* const operation,
    const DWORD nativeError) noexcept
{
    using ReapResult =
        Domain::Result<DashboardDeadlineIocpReapResult>;
    FailureDispatchGuard failureDispatch{*this};
    try {
        const std::scoped_lock lock{mutex_};
        auto found = findOperationLocked(operation);
        if (!found) {
            auto error = std::move(found).error();
            retainFatalFailureLocked(Domain::Error{error});
            return ReapResult::failure(std::move(error));
        }

        const std::size_t index = std::move(found).value();
        if (!validatePostedHandleLocked(index)) {
            auto error = integrityError(
                "A dashboard deadline IOCP completion targeted a slot without an exact posted generation.");
            retainFatalFailureLocked(Domain::Error{error});
            return ReapResult::failure(std::move(error));
        }

        if (!(completionKey == completionKey_) ||
            transferredBytes != 0U || nativeError != ERROR_SUCCESS) {
            // The sole kernel packet has already been dequeued. Consume the
            // exact generation and vacate its stable operation before
            // reporting packet corruption, so no tombstone can be stranded.
            drainExactPostedHandleLocked(index);
            auto error = integrityError(
                "A dashboard deadline IOCP completion violated its exact key, zero-byte, or success contract.");
            retainFatalFailureLocked(Domain::Error{error});
            return ReapResult::failure(std::move(error));
        }

        auto& slot = *slots_[index];
        const auto lifecycle = slot.lifecycle;
        const auto handle = slot.handle;
        auto deadline = mailbox_->take(handle);

        if (lifecycle == SlotLifecycle::PostedRetired) {
            slot.handle = {};
            slot.operation = {};
            slot.lifecycle = SlotLifecycle::Vacant;
            ++drainedRetiredCount_;
            if (deadline.has_value()) {
                auto error = integrityError(
                    "A retired dashboard deadline notification exposed a deadline value while being reaped.");
                retainFatalFailureLocked(Domain::Error{error});
                return ReapResult::failure(std::move(error));
            }
            return ReapResult::success(DashboardDeadlineIocpReapResult{
                DashboardDeadlineIocpReapDisposition::
                    RetiredNotificationDrained,
                std::nullopt});
        }

        if (!deadline.has_value() ||
            deadline->registrationId != handle.registrationId ||
            deadline->armSequence == 0U) {
            static_cast<void>(mailbox_->retire(handle));
            slot.handle = {};
            slot.operation = {};
            slot.lifecycle = SlotLifecycle::Vacant;
            auto error = integrityError(
                "A dashboard deadline IOCP completion did not match the exact live mailbox generation and token.");
            retainFatalFailureLocked(Domain::Error{error});
            return ReapResult::failure(std::move(error));
        }

        slot.operation = {};
        slot.lifecycle = SlotLifecycle::Registered;
        ++deliveredDeadlineCount_;
        return ReapResult::success(DashboardDeadlineIocpReapResult{
            DashboardDeadlineIocpReapDisposition::DeadlineDelivered,
            std::move(deadline)});
    } catch (...) {
        return ReapResult::failure(bridgeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline IOCP bridge could not reap a notification safely."));
    }
}

void DashboardDeadlineIocpBridge::transitionToShutdownLocked() noexcept
{
    shutdown_ = true;
    mailbox_->shutdown();
    for (auto& slotOwner : slots_) {
        auto& slot = *slotOwner;
        if (slot.lifecycle == SlotLifecycle::Registered) {
            slot.handle = {};
            slot.operation = {};
            slot.lifecycle = SlotLifecycle::Vacant;
        } else if (slot.lifecycle == SlotLifecycle::Posted) {
            slot.lifecycle = SlotLifecycle::PostedRetired;
        }
    }
}

void DashboardDeadlineIocpBridge::retainFatalFailureLocked(
    Domain::Error error) noexcept
{
    // Close publication first. Callers cannot observe fatal status until the
    // outer bridge mutex is released, by which point only posted tombstones
    // remain in the mailbox.
    transitionToShutdownLocked();
    fatal_ = true;
    if (!firstFailureSnapshot_.has_value()) {
        firstFailureSnapshot_.emplace(classifyFailure(error));
    }
    if (!firstFailure_.has_value()) {
        firstFailure_.emplace(std::move(error));
    }
}

void DashboardDeadlineIocpBridge::dispatchFatalFailureIfRequired()
    noexcept
{
    try {
        std::shared_ptr<IDashboardDeadlineIocpBridgeFailureObserver>
            observer;
        DashboardDeadlineIocpFailure failure;
        bool missingBoundObserver{};
        {
            const std::scoped_lock dispatchLock{
                failureNotificationDispatchMutex_};
            if (failureNotificationDeferralCount_ != 0U ||
                failureNotificationDispatchInProgress_) {
                return;
            }
            {
                const std::scoped_lock lock{mutex_};
                if (!fatal_ || !failureObserverEverBound_ ||
                    failureNotificationSent_) {
                    return;
                }
                failureNotificationSent_ = true;
                observer = failureObserver_.lock();
                missingBoundObserver = observer == nullptr;
                failure = firstFailureSnapshot_.value_or(
                    DashboardDeadlineIocpFailure{
                        DashboardDeadlineIocpFailureKind::InternalFailure,
                        false});
            }
            failureNotificationDispatchInProgress_ = true;
        }

        if (observer != nullptr) {
            observer->dashboardDeadlineIocpBridgeFailed(failure);
        } else if (missingBoundObserver) {
            // A successfully bound managed observer is a mandatory lifetime
            // owner. Losing it would turn a fatal deadline route into an
            // unbounded silent shutdown wait.
            std::terminate();
        }

        {
            const std::scoped_lock lock{
                failureNotificationDispatchMutex_};
            failureNotificationDispatchInProgress_ = false;
        }
        failureNotificationDispatchChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void DashboardDeadlineIocpBridge::releaseFailureNotificationDeferral()
    noexcept
{
    try {
        bool finalRelease{};
        {
            const std::scoped_lock lock{
                failureNotificationDispatchMutex_};
            if (failureNotificationDeferralCount_ == 0U) {
                std::terminate();
            }
            --failureNotificationDeferralCount_;
            finalRelease = failureNotificationDeferralCount_ == 0U;
        }
        if (finalRelease) {
            dispatchFatalFailureIfRequired();
        }
    } catch (...) {
        std::terminate();
    }
}

DashboardDeadlineIocpSnapshot
DashboardDeadlineIocpBridge::snapshotLocked() const noexcept
{
    const auto mailboxSnapshot = mailbox_->snapshot();
    std::size_t posted{};
    for (const auto& slot : slots_) {
        if (slot->lifecycle == SlotLifecycle::Posted ||
            slot->lifecycle == SlotLifecycle::PostedRetired) {
            ++posted;
        }
    }
    return DashboardDeadlineIocpSnapshot{
        mailboxSnapshot.registeredCount(),
        posted,
        mailboxSnapshot.retiredAwaitingReapCount(),
        mailboxSnapshot.maximumOwnerCount(),
        successfulPostCount_,
        coalescedSignalCount_,
        deliveredDeadlineCount_,
        drainedRetiredCount_,
        shutdown_,
        fatal_,
        firstFailureSnapshot_};
}

DashboardDeadlineIocpSnapshot DashboardDeadlineIocpBridge::snapshot()
    const noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        return snapshotLocked();
    } catch (...) {
        return DashboardDeadlineIocpSnapshot{
            0U,
            0U,
            0U,
            SlotCount,
            0U,
            0U,
            0U,
            0U,
            true,
            true,
            DashboardDeadlineIocpFailure{
                DashboardDeadlineIocpFailureKind::InternalFailure,
                false}};
    }
}

std::optional<Domain::Error>
DashboardDeadlineIocpBridge::fullFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

void DashboardDeadlineIocpBridge::shutdown() noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        if (!shutdown_) {
            transitionToShutdownLocked();
        }
    } catch (...) {
        std::terminate();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
