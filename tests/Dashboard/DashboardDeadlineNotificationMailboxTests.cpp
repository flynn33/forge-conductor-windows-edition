#include "Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using Deadline = Windows::WindowsDashboardDeadline;
using DeadlineKind = Windows::WindowsDashboardDeadlineKind;
using Disposition = Detail::DashboardDeadlinePublishDisposition;
using Handle = Detail::DashboardDeadlineNotificationHandle;
using Mailbox = Detail::DashboardDeadlineNotificationMailbox;

static_assert(std::is_final_v<Mailbox>);
static_assert(!std::is_copy_constructible_v<Mailbox>);
static_assert(!std::is_move_constructible_v<Mailbox>);
static_assert(Mailbox::HardMaximumOwnerCount == 44U);
static_assert(noexcept(Mailbox::create()));
static_assert(noexcept(std::declval<Mailbox&>().registerOwner(1U)));
static_assert(noexcept(std::declval<Mailbox&>().publish({})));
static_assert(noexcept(std::declval<Mailbox&>().take({})));
static_assert(noexcept(std::declval<Mailbox&>().retire({})));
static_assert(noexcept(std::declval<const Mailbox&>().snapshot()));
static_assert(noexcept(std::declval<Mailbox&>().shutdown()));

std::size_t assertionCount{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        fail(message);
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

[[nodiscard]] std::unique_ptr<Mailbox> mailbox(
    const std::size_t maximum = Mailbox::HardMaximumOwnerCount)
{
    return take(Mailbox::create(maximum));
}

[[nodiscard]] Deadline deadline(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence)
{
    return Deadline{
        registrationId,
        armSequence,
        DeadlineKind::SocketLifetime,
        std::chrono::steady_clock::now()};
}

void constructionAndRegistrationAreBounded()
{
    const auto zero = Mailbox::create(0U);
    require(!zero, "zero mailbox capacity was accepted");
    require(zero.error().code == Domain::ErrorCodes::InvalidRequest,
            "zero capacity used the wrong error code");
    const auto oversized = Mailbox::create(
        Mailbox::HardMaximumOwnerCount + 1U);
    require(!oversized, "oversized mailbox capacity was accepted");

    auto owner = mailbox(2U);
    const auto missing = owner->registerOwner(0U);
    require(!missing, "zero owner identifier was accepted");

    const auto first = take(owner->registerOwner(7U));
    const auto second = take(owner->registerOwner(8U));
    require(first.slotIndex != second.slotIndex,
            "two owners shared a live mailbox slot");
    require(first.generation != 0U && second.generation != 0U,
            "registration returned a zero generation");

    const auto snapshot = owner->snapshot();
    require(snapshot.registeredCount() == 2U,
            "registered owner count was incorrect");
    require(snapshot.retiredAwaitingReapCount() == 0U,
            "registration created a retired tombstone");
    require(snapshot.pendingNotificationCount() == 0U,
            "registration created a pending notification");
    require(snapshot.maximumOwnerCount() == 2U,
            "configured capacity was not retained");
    require(!snapshot.isShutdown(), "live mailbox reported shutdown");

    const auto duplicate = owner->registerOwner(7U);
    require(!duplicate, "duplicate owner registration was accepted");
    require(duplicate.error().code == Domain::ErrorCodes::Conflict,
            "duplicate registration used the wrong error code");
    const auto exhausted = owner->registerOwner(9U);
    require(!exhausted, "capacity overflow was accepted");
    require(exhausted.error().code == Domain::ErrorCodes::LimitExceeded &&
                exhausted.error().retryable,
            "capacity overflow was not a retryable limit error");

    require(owner->retire(first), "live owner could not be retired");
    require(!owner->retire(first), "retirement succeeded twice");
    const auto replacement = take(owner->registerOwner(9U));
    require(replacement.generation != first.generation,
            "a recycled slot reused its generation");
    require(replacement.registrationId == 9U,
            "a recycled slot changed the new owner identifier");
    const auto reused = owner->registerOwner(8U);
    require(!reused,
            "a retired dashboard owner identifier was reused");
    require(reused.error().code == Domain::ErrorCodes::Conflict,
            "identifier reuse returned the wrong error code");
    require(!owner->take(first).has_value(),
            "a stale handle consumed a recycled slot");
    require(!owner->retire(first),
            "a stale handle retired a recycled slot");
}

void publicationCoalescesToOneLatestValue()
{
    auto owner = mailbox(1U);
    const auto handle = take(owner->registerOwner(31U));

    const auto invalidOwner = owner->publish(deadline(0U, 1U));
    require(!invalidOwner, "zero notification owner was accepted");
    const auto invalidArm = owner->publish(deadline(31U, 0U));
    require(!invalidArm, "zero notification arm was accepted");
    const auto unknown = owner->publish(deadline(32U, 1U));
    require(!unknown, "unknown notification owner was accepted");
    require(unknown.error().code == Domain::ErrorCodes::TransportClosed,
            "unknown owner used the wrong error code");

    const auto first = take(owner->publish(deadline(31U, 1U)));
    require(first.disposition == Disposition::NotificationRequired,
            "empty mailbox did not request an IOCP notification");
    require(first.handle == handle,
            "publication returned the wrong IOCP mailbox handle");
    const auto second = take(owner->publish(deadline(31U, 2U)));
    require(second.disposition == Disposition::Coalesced,
            "occupied mailbox requested a second notification");
    require(second.handle == handle,
            "coalescing changed the IOCP mailbox handle");
    require(owner->snapshot().pendingNotificationCount() == 1U,
            "coalescing exceeded capacity one");

    const auto delivered = owner->take(handle);
    require(delivered.has_value(), "pending notification had no value");
    require(delivered->registrationId == 31U &&
                delivered->armSequence == 2U,
            "coalescing did not retain the latest token");
    require(!owner->take(handle).has_value(),
            "one notification was reaped twice");

    const auto stale = owner->publish(deadline(31U, 2U));
    require(!stale, "stale notification token was accepted");
    require(stale.error().code == Domain::ErrorCodes::Conflict,
            "stale notification used the wrong error code");

    require(take(owner->publish(deadline(31U, 3U))).disposition ==
                Disposition::NotificationRequired,
            "reaped mailbox did not request a fresh notification");
    require(owner->retire(handle),
            "owner with a pending notification could not retire");
    auto snapshot = owner->snapshot();
    require(snapshot.registeredCount() == 0U,
            "retired owner remained registered");
    require(snapshot.retiredAwaitingReapCount() == 1U &&
                snapshot.pendingNotificationCount() == 1U,
            "retirement did not retain exactly one pending tombstone");
    require(!owner->take(handle).has_value(),
            "retired owner exposed a discarded deadline");
    snapshot = owner->snapshot();
    require(snapshot.retiredAwaitingReapCount() == 0U &&
                snapshot.pendingNotificationCount() == 0U,
            "reaping did not release the retired tombstone");

    const auto afterRetire = owner->publish(deadline(31U, 4U));
    require(!afterRetire, "retired owner accepted publication");
}

void uniqueOwnerIdsMayArriveOutOfNumericOrder()
{
    auto owner = mailbox(3U);
    const auto higher = take(owner->registerOwner(502U));
    const auto lower = take(owner->registerOwner(501U));
    const auto highest = take(owner->registerOwner(503U));

    require(higher.registrationId == 502U &&
                lower.registrationId == 501U &&
                highest.registrationId == 503U,
            "out-of-order registration changed an owner identifier");
    require(higher.slotIndex != lower.slotIndex &&
                higher.slotIndex != highest.slotIndex &&
                lower.slotIndex != highest.slotIndex,
            "out-of-order unique owners shared a fixed slot");
    require(owner->snapshot().registeredCount() == 3U,
            "out-of-order registration changed bounded accounting");
    require(owner->retire(higher) && owner->retire(lower) &&
                owner->retire(highest),
            "out-of-order owners did not retire exactly");
    require(owner->snapshot().registeredCount() == 0U,
            "out-of-order retirement leaked a fixed slot");
}

void shutdownRetainsOnlyNotificationsThatNeedReaping()
{
    auto owner = mailbox(2U);
    const auto pending = take(owner->registerOwner(41U));
    const auto idle = take(owner->registerOwner(42U));
    static_cast<void>(take(owner->publish(deadline(41U, 1U))));

    owner->shutdown();
    owner->shutdown();
    auto snapshot = owner->snapshot();
    require(snapshot.isShutdown(), "shutdown state was not retained");
    require(snapshot.registeredCount() == 0U,
            "shutdown retained an active owner");
    require(snapshot.retiredAwaitingReapCount() == 1U &&
                snapshot.pendingNotificationCount() == 1U,
            "shutdown retained more than the posted notification tombstone");
    require(!owner->take(idle).has_value(),
            "shutdown retained an idle owner slot");

    const auto registration = owner->registerOwner(43U);
    require(!registration, "shutdown mailbox accepted an owner");
    require(registration.error().code == Domain::ErrorCodes::TransportClosed,
            "shutdown registration used the wrong error code");
    const auto publication = owner->publish(deadline(41U, 2U));
    require(!publication, "shutdown mailbox accepted publication");
    require(publication.error().code == Domain::ErrorCodes::TransportClosed,
            "shutdown publication used the wrong error code");

    require(!owner->take(pending).has_value(),
            "shutdown tombstone exposed a discarded deadline");
    snapshot = owner->snapshot();
    require(snapshot.retiredAwaitingReapCount() == 0U &&
                snapshot.pendingNotificationCount() == 0U,
            "shutdown tombstone was not released after reap");
}

void concurrentPublicationNeverCreatesASecondPendingNotification()
{
    auto owner = mailbox(1U);
    const auto handle = take(owner->registerOwner(91U));
    constexpr std::uint64_t PublicationCount = 20'000U;
    std::atomic_bool notificationPosted{};
    std::atomic_bool producerDone{};
    std::atomic_bool failed{};
    std::atomic_uint64_t latestTaken{};
    std::atomic_size_t maximumPending{};

    std::jthread consumer{[&](std::stop_token) {
        while (!producerDone.load(std::memory_order_acquire) ||
               notificationPosted.load(std::memory_order_acquire)) {
            if (!notificationPosted.exchange(
                    false, std::memory_order_acq_rel)) {
                std::this_thread::yield();
                continue;
            }
            const auto value = owner->take(handle);
            if (value.has_value()) {
                latestTaken.store(
                    value->armSequence, std::memory_order_release);
            }
            const auto pending =
                owner->snapshot().pendingNotificationCount();
            auto observed = maximumPending.load(std::memory_order_acquire);
            while (pending > observed &&
                   !maximumPending.compare_exchange_weak(
                       observed, pending, std::memory_order_acq_rel)) {
            }
        }
    }};

    for (std::uint64_t sequence = 1U; sequence <= PublicationCount;
         ++sequence) {
        const auto result = owner->publish(deadline(91U, sequence));
        if (!result) {
            failed.store(true, std::memory_order_release);
            break;
        }
        if (result.value().disposition ==
            Disposition::NotificationRequired) {
            notificationPosted.store(true, std::memory_order_release);
        }
        const auto pending = owner->snapshot().pendingNotificationCount();
        auto observed = maximumPending.load(std::memory_order_acquire);
        while (pending > observed &&
               !maximumPending.compare_exchange_weak(
                   observed, pending, std::memory_order_acq_rel)) {
        }
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    require(!failed.load(std::memory_order_acquire),
            "concurrent publication returned a typed failure");
    require(maximumPending.load(std::memory_order_acquire) <= 1U,
            "concurrent publication exceeded capacity one");
    require(latestTaken.load(std::memory_order_acquire) == PublicationCount,
            "concurrent publication lost the latest deadline");
    require(owner->snapshot().pendingNotificationCount() == 0U,
            "concurrent publication left a notification pending");
}

} // namespace

int main()
{
    try {
        constructionAndRegistrationAreBounded();
        uniqueOwnerIdsMayArriveOutOfNumericOrder();
        publicationCoalescesToOneLatestValue();
        shutdownRetainsOnlyNotificationsThatNeedReaping();
        concurrentPublicationNeverCreatesASecondPendingNotification();
        std::cout << "Dashboard deadline notification mailbox tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "Dashboard deadline notification mailbox tests failed: "
            << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard deadline notification mailbox tests failed with an unknown error.\n";
        return 1;
    }
}
