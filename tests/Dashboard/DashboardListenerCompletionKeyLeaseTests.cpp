#include "Infrastructure/Windows/Detail/DashboardListenerCompletionKeyLease.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using Authority = Detail::DashboardFixedIocpKeyAuthority;
using Key = Detail::DashboardIoCompletionKey;
using Lease = Detail::DashboardListenerCompletionKeyLease;
using Pool = Detail::DashboardListenerCompletionKeyLeasePool;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view message)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

static_assert(std::is_final_v<Lease>);
static_assert(std::is_final_v<Pool>);
static_assert(!std::is_default_constructible_v<Lease>);
static_assert(!std::is_copy_constructible_v<Lease>);
static_assert(!std::is_copy_assignable_v<Lease>);
static_assert(std::is_nothrow_move_constructible_v<Lease>);
static_assert(!std::is_move_assignable_v<Lease>);
static_assert(std::is_nothrow_destructible_v<Lease>);
static_assert(!std::is_copy_constructible_v<Pool>);
static_assert(!std::is_move_constructible_v<Pool>);
static_assert(noexcept(std::declval<const Lease&>().ownsSlot()));
static_assert(noexcept(std::declval<const Lease&>().completionKey()));
static_assert(noexcept(Pool::create(std::declval<const Authority&>())));
static_assert(noexcept(std::declval<const Pool&>().tryAcquire()));

void deterministicAcquireMoveConflictAndReuse()
{
    const auto authority = take(Authority::create());
    auto pool = take(Pool::create(authority));

    auto first = take(pool->tryAcquire());
    require(first.ownsSlot(), "slot A acquisition did not own a slot");
    require(first.completionKey() == authority.listenerSlotA(),
            "the first acquisition did not deterministically select slot A");

    auto moved = Lease{std::move(first)};
    require(!first.ownsSlot() && first.completionKey() == Key{0U},
            "a moved-from listener lease retained authority");
    require(moved.ownsSlot() &&
                moved.completionKey() == authority.listenerSlotA(),
            "moving a listener lease lost slot-A authority");

    auto second = take(pool->tryAcquire());
    require(second.completionKey() == authority.listenerSlotB(),
            "the second acquisition did not deterministically select slot B");

    const auto full = pool->tryAcquire();
    require(!full, "a third simultaneous listener lease was issued");
    require(full.error().code == Domain::ErrorCodes::Conflict,
            "pool exhaustion used the wrong stable error code");
    require(full.error().retryable,
            "pool exhaustion was not classified as retryable");

    {
        auto retainedA = Lease{std::move(moved)};
        require(retainedA.completionKey() == authority.listenerSlotA(),
                "a second move changed the leased slot");
    }

    auto reusedA = take(pool->tryAcquire());
    require(reusedA.completionKey() == authority.listenerSlotA(),
            "slot A was not returned by lease destruction");
}

void leaseKeepsPoolStateAlive()
{
    const auto authority = take(Authority::create());
    auto pool = take(Pool::create(authority));
    auto lease = take(pool->tryAcquire());

    pool.reset();
    require(lease.ownsSlot(),
            "destroying the pool invalidated a live listener lease");
    require(lease.completionKey() == authority.listenerSlotA(),
            "destroying the pool changed a live listener key");
}

void concurrentAcquisitionIsBoundedAndReusable()
{
    constexpr std::size_t WorkerCount = 32U;
    const auto authority = take(Authority::create());
    auto pool = take(Pool::create(authority));
    std::latch start{1U};
    std::mutex leasesMutex;
    std::vector<Lease> leases;
    leases.reserve(2U);
    std::atomic_size_t conflictCount{};
    std::atomic_size_t unexpectedFailureCount{};
    std::vector<std::jthread> workers;
    workers.reserve(WorkerCount);

    for (std::size_t index{}; index < WorkerCount; ++index) {
        workers.emplace_back([&]() noexcept {
            start.wait();
            auto acquired = pool->tryAcquire();
            if (acquired) {
                const std::scoped_lock lock{leasesMutex};
                leases.emplace_back(std::move(acquired).value());
                return;
            }
            if (acquired.error().code == Domain::ErrorCodes::Conflict &&
                acquired.error().retryable) {
                conflictCount.fetch_add(1U, std::memory_order_relaxed);
                return;
            }
            unexpectedFailureCount.fetch_add(
                1U, std::memory_order_relaxed);
        });
    }

    start.count_down();
    workers.clear();

    require(leases.size() == 2U,
            "concurrent acquisition did not retain exactly two leases");
    require(conflictCount.load(std::memory_order_relaxed) ==
                WorkerCount - 2U,
            "concurrent pool exhaustion did not reject every extra caller");
    require(unexpectedFailureCount.load(std::memory_order_relaxed) == 0U,
            "concurrent acquisition produced an unexpected error");

    const auto slotACount = std::count_if(
        leases.begin(),
        leases.end(),
        [&](const Lease& lease) noexcept {
            return lease.completionKey() == authority.listenerSlotA();
        });
    const auto slotBCount = std::count_if(
        leases.begin(),
        leases.end(),
        [&](const Lease& lease) noexcept {
            return lease.completionKey() == authority.listenerSlotB();
        });
    require(slotACount == 1 && slotBCount == 1,
            "concurrent acquisition duplicated or invented a listener key");

    leases.clear();
    auto reusedA = take(pool->tryAcquire());
    auto reusedB = take(pool->tryAcquire());
    require(reusedA.completionKey() == authority.listenerSlotA() &&
                reusedB.completionKey() == authority.listenerSlotB(),
            "destroyed concurrent leases were not deterministically reusable");
}

} // namespace

int main()
{
    try {
        deterministicAcquireMoveConflictAndReuse();
        leaseKeepsPoolStateAlive();
        concurrentAcquisitionIsBoundedAndReusable();
        std::cout
            << "Dashboard listener completion-key lease tests passed ("
            << assertionCount.load(std::memory_order_relaxed)
            << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "Dashboard listener completion-key lease tests failed: "
            << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard listener completion-key lease tests failed "
                     "with an unknown error.\n";
        return 1;
    }
}
