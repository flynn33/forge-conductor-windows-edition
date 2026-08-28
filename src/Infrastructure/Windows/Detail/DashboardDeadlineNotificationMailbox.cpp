#include "Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error mailboxError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

} // namespace

DashboardDeadlineNotificationSnapshot::DashboardDeadlineNotificationSnapshot(
    const std::size_t registeredCount,
    const std::size_t retiredAwaitingReapCount,
    const std::size_t pendingNotificationCount,
    const std::size_t maximumOwnerCount,
    const bool shutdown) noexcept
    : registeredCount_{registeredCount},
      retiredAwaitingReapCount_{retiredAwaitingReapCount},
      pendingNotificationCount_{pendingNotificationCount},
      maximumOwnerCount_{maximumOwnerCount},
      shutdown_{shutdown}
{
}

class DashboardDeadlineNotificationMailbox::Impl final {
public:
    explicit Impl(const std::size_t maximumOwnerCount) noexcept
        : maximumOwnerCount_{maximumOwnerCount}
    {
    }

    [[nodiscard]] Domain::Result<DashboardDeadlineNotificationHandle>
    registerOwner(const std::uint64_t registrationId) noexcept
    {
        using RegisterResult =
            Domain::Result<DashboardDeadlineNotificationHandle>;
        try {
            if (registrationId == 0U) {
                return RegisterResult::failure(mailboxError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A dashboard deadline mailbox owner requires a nonzero identifier."));
            }

            const std::lock_guard lock{mutex_};
            if (shutdown_) {
                return RegisterResult::failure(mailboxError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard deadline notification mailbox is shut down."));
            }
            if (generationExhausted_) {
                return RegisterResult::failure(mailboxError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The dashboard deadline mailbox generation is exhausted."));
            }
            if (findByRegistrationId(registrationId) != slotsEnd()) {
                return RegisterResult::failure(mailboxError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard deadline mailbox owner identifier is already active."));
            }

            const auto available = std::find_if(
                slotsBegin(), slotsEnd(), [](const Slot& slot) noexcept {
                    return !slot.occupied;
                });
            if (available == slotsEnd()) {
                return RegisterResult::failure(mailboxError(
                    Domain::ErrorCodes::LimitExceeded,
                    "Dashboard deadline notification capacity is exhausted.",
                    true));
            }

            available->occupied = true;
            available->registrationId = registrationId;
            available->generation = nextGeneration_;
            if (nextGeneration_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                generationExhausted_ = true;
            } else {
                ++nextGeneration_;
            }

            return RegisterResult::success(
                DashboardDeadlineNotificationHandle{
                    static_cast<std::size_t>(available - slots_.begin()),
                    available->generation,
                    registrationId});
        } catch (...) {
            return RegisterResult::failure(mailboxError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard deadline mailbox owner could not be registered."));
        }
    }

    [[nodiscard]] Domain::Result<DashboardDeadlinePublishResult> publish(
        WindowsDashboardDeadline deadline) noexcept
    {
        using PublishResult = Domain::Result<DashboardDeadlinePublishResult>;
        try {
            if (deadline.registrationId == 0U ||
                deadline.armSequence == 0U) {
                return PublishResult::failure(mailboxError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A dashboard deadline notification requires an exact nonzero token."));
            }

            const std::lock_guard lock{mutex_};
            if (shutdown_) {
                return PublishResult::failure(mailboxError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard deadline notification mailbox is shut down."));
            }
            const auto slot = findByRegistrationId(deadline.registrationId);
            if (slot == slotsEnd() || slot->retired) {
                return PublishResult::failure(mailboxError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard deadline mailbox owner is not active."));
            }
            if (deadline.armSequence <= slot->lastPublishedSequence) {
                return PublishResult::failure(mailboxError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard deadline notification token is stale."));
            }

            slot->lastPublishedSequence = deadline.armSequence;
            slot->latest = std::move(deadline);
            const DashboardDeadlineNotificationHandle handle{
                static_cast<std::size_t>(slot - slots_.begin()),
                slot->generation,
                slot->registrationId};
            if (slot->notificationPending) {
                return PublishResult::success(
                    DashboardDeadlinePublishResult{
                        DashboardDeadlinePublishDisposition::Coalesced,
                        handle});
            }
            slot->notificationPending = true;
            return PublishResult::success(
                DashboardDeadlinePublishResult{
                    DashboardDeadlinePublishDisposition::NotificationRequired,
                    handle});
        } catch (...) {
            return PublishResult::failure(mailboxError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard deadline notification could not be published."));
        }
    }

    [[nodiscard]] std::optional<WindowsDashboardDeadline> take(
        const DashboardDeadlineNotificationHandle& handle) noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            auto* const slot = slotFor(handle);
            if (slot == nullptr || !slot->notificationPending) {
                return std::nullopt;
            }

            slot->notificationPending = false;
            auto result = std::move(slot->latest);
            slot->latest.reset();
            if (slot->retired) {
                release(*slot);
            }
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] bool retire(
        const DashboardDeadlineNotificationHandle& handle) noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            auto* const slot = slotFor(handle);
            if (slot == nullptr || slot->retired) {
                return false;
            }

            slot->retired = true;
            slot->latest.reset();
            if (!slot->notificationPending) {
                release(*slot);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] DashboardDeadlineNotificationSnapshot snapshot()
        const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            std::size_t registered{};
            std::size_t retired{};
            std::size_t pending{};
            for (auto iterator = slotsBegin(); iterator != slotsEnd();
                 ++iterator) {
                if (!iterator->occupied) {
                    continue;
                }
                if (iterator->retired) {
                    ++retired;
                } else {
                    ++registered;
                }
                if (iterator->notificationPending) {
                    ++pending;
                }
            }
            return DashboardDeadlineNotificationSnapshot{
                registered,
                retired,
                pending,
                maximumOwnerCount_,
                shutdown_};
        } catch (...) {
            return DashboardDeadlineNotificationSnapshot{
                0U, 0U, 0U, maximumOwnerCount_, true};
        }
    }

    void shutdown() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            shutdown_ = true;
            for (auto iterator = slotsBegin(); iterator != slotsEnd();
                 ++iterator) {
                if (!iterator->occupied) {
                    continue;
                }
                iterator->retired = true;
                iterator->latest.reset();
                if (!iterator->notificationPending) {
                    release(*iterator);
                }
            }
        } catch (...) {
        }
    }

private:
    struct Slot final {
        std::optional<WindowsDashboardDeadline> latest;
        std::uint64_t registrationId{};
        std::uint64_t generation{};
        std::uint64_t lastPublishedSequence{};
        bool occupied{};
        bool retired{};
        bool notificationPending{};
    };

    using Iterator = std::array<Slot, HardMaximumOwnerCount>::iterator;
    using ConstIterator =
        std::array<Slot, HardMaximumOwnerCount>::const_iterator;

    [[nodiscard]] Iterator slotsBegin() noexcept { return slots_.begin(); }

    [[nodiscard]] Iterator slotsEnd() noexcept
    {
        return slots_.begin() +
            static_cast<std::ptrdiff_t>(maximumOwnerCount_);
    }

    [[nodiscard]] ConstIterator slotsBegin() const noexcept
    {
        return slots_.begin();
    }

    [[nodiscard]] ConstIterator slotsEnd() const noexcept
    {
        return slots_.begin() +
            static_cast<std::ptrdiff_t>(maximumOwnerCount_);
    }

    [[nodiscard]] Iterator findByRegistrationId(
        const std::uint64_t registrationId) noexcept
    {
        return std::find_if(
            slotsBegin(), slotsEnd(),
            [registrationId](const Slot& slot) noexcept {
                return slot.occupied &&
                    slot.registrationId == registrationId;
            });
    }

    [[nodiscard]] Slot* slotFor(
        const DashboardDeadlineNotificationHandle& handle) noexcept
    {
        if (handle.registrationId == 0U || handle.generation == 0U ||
            handle.slotIndex >= maximumOwnerCount_) {
            return nullptr;
        }
        auto& slot = slots_[handle.slotIndex];
        if (!slot.occupied || slot.registrationId != handle.registrationId ||
            slot.generation != handle.generation) {
            return nullptr;
        }
        return &slot;
    }

    static void release(Slot& slot) noexcept { slot = Slot{}; }

    const std::size_t maximumOwnerCount_{};
    mutable std::mutex mutex_;
    std::array<Slot, HardMaximumOwnerCount> slots_{};
    std::uint64_t nextGeneration_{1U};
    bool generationExhausted_{};
    bool shutdown_{};
};

Domain::Result<std::unique_ptr<DashboardDeadlineNotificationMailbox>>
DashboardDeadlineNotificationMailbox::create(
    const std::size_t maximumOwnerCount) noexcept
{
    using MailboxResult = Domain::Result<
        std::unique_ptr<DashboardDeadlineNotificationMailbox>>;
    try {
        if (maximumOwnerCount == 0U ||
            maximumOwnerCount > HardMaximumOwnerCount) {
            return MailboxResult::failure(mailboxError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard deadline mailbox requires a bounded positive capacity."));
        }
        return MailboxResult::success(
            std::unique_ptr<DashboardDeadlineNotificationMailbox>{
                new DashboardDeadlineNotificationMailbox{
                    maximumOwnerCount}});
    } catch (...) {
        return MailboxResult::failure(mailboxError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline notification mailbox could not be created."));
    }
}

DashboardDeadlineNotificationMailbox::DashboardDeadlineNotificationMailbox(
    const std::size_t maximumOwnerCount)
    : implementation_{std::make_unique<Impl>(maximumOwnerCount)}
{
}

DashboardDeadlineNotificationMailbox::~DashboardDeadlineNotificationMailbox()
    noexcept
{
    shutdown();
}

Domain::Result<DashboardDeadlineNotificationHandle>
DashboardDeadlineNotificationMailbox::registerOwner(
    const std::uint64_t registrationId) noexcept
{
    return implementation_->registerOwner(registrationId);
}

Domain::Result<DashboardDeadlinePublishResult>
DashboardDeadlineNotificationMailbox::publish(
    WindowsDashboardDeadline deadline) noexcept
{
    return implementation_->publish(std::move(deadline));
}

std::optional<WindowsDashboardDeadline>
DashboardDeadlineNotificationMailbox::take(
    const DashboardDeadlineNotificationHandle& handle) noexcept
{
    return implementation_->take(handle);
}

bool DashboardDeadlineNotificationMailbox::retire(
    const DashboardDeadlineNotificationHandle& handle) noexcept
{
    return implementation_->retire(handle);
}

DashboardDeadlineNotificationSnapshot
DashboardDeadlineNotificationMailbox::snapshot() const noexcept
{
    return implementation_->snapshot();
}

void DashboardDeadlineNotificationMailbox::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
