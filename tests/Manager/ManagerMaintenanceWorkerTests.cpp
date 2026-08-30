#include "ManagerMaintenanceWorker.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerMaintenanceService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Hosts::Manager::Detail {

struct ManagerMaintenanceWorkerTestAccess final {
    [[nodiscard]] static bool waitUntilExternalJoinOwned(
        ManagerMaintenanceWorker& worker,
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{worker.lifecycleMutex_};
            return worker.lifecycleCondition_.wait_for(
                lock,
                timeout,
                [&worker]() noexcept { return worker.shutdownJoinOwned_; });
        } catch (...) {
            return false;
        }
    }

    static void releaseExternalJoinWaitForFailedTestCleanup(
        ManagerMaintenanceWorker& worker) noexcept
    {
        try {
            {
                const std::lock_guard lock{worker.lifecycleMutex_};
                worker.shutdownJoinOwned_ = false;
            }
            worker.lifecycleCondition_.notify_all();
        } catch (...) {
            worker.lifecycleCondition_.notify_all();
        }
    }
};

} // namespace ForgeConductor::Hosts::Manager::Detail

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Host::ManagerMaintenanceWorker>);
static_assert(std::is_final_v<Host::ManagerMaintenanceWorkerTiming>);
static_assert(!std::is_copy_constructible_v<Host::ManagerMaintenanceWorker>);
static_assert(!std::is_move_constructible_v<Host::ManagerMaintenanceWorker>);

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw TestFailure{message};
    }
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string& message)
{
    require(!result, message + " unexpectedly succeeded");
    require(result.error().code == code, message + " returned the wrong error");
}

[[nodiscard]] Domain::Error injectedFailure(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::string{message}, true);
}

class AdvancingClock final : public Contracts::IClock {
public:
    AdvancingClock(
        const Domain::MonotonicTimePoint monotonic,
        const std::chrono::milliseconds step = 1s) noexcept
        : monotonic_{monotonic}, step_{step}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        const auto sequence = next_.fetch_add(1U, std::memory_order_acq_rel);
        return monotonic_ + step_ * static_cast<std::int64_t>(sequence);
    }

private:
    Domain::MonotonicTimePoint monotonic_;
    std::chrono::milliseconds step_;
    mutable std::atomic_uint64_t next_{};
};

class SequentialUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto sequence = next_.fetch_add(1U, std::memory_order_acq_rel);
            std::ostringstream text;
            text << "00000000-0000-4000-8000-" << std::hex
                 << std::setfill('0') << std::setw(12) << sequence;
            return Domain::Uuid::parse(text.str());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(
                injectedFailure("UUID generation failed."));
        }
    }

private:
    std::atomic_uint64_t next_{1U};
};

struct MaintenancePass final {
    std::string operationId;
    std::string correlationId;
    Domain::MonotonicTimePoint deadline;
    std::chrono::steady_clock::time_point enteredAt;
    bool cancellationPossible{};
    bool cancellationRequested{};
};

class RecordingMaintenanceService final
    : public Contracts::IManagerMaintenanceService {
public:
    [[nodiscard]] Domain::Result<void> reconcile(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++activeCalls_;
            maxActiveCalls_ = (std::max)(maxActiveCalls_, activeCalls_);
            calls_.push_back(MaintenancePass{
                context.operationId.value(),
                context.correlationId.value(),
                context.deadline,
                std::chrono::steady_clock::now(),
                context.cancellation.stop_possible(),
                context.cancellation.stop_requested()});
            condition_.notify_all();
            if (block_) {
                condition_.wait(
                    lock,
                    [this]() noexcept { return released_; });
            }
            --activeCalls_;
            const bool fail = failPasses_;
            lock.unlock();
            if (fail) {
                return Domain::Result<void>::failure(
                    injectedFailure("The maintenance pass failed."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(
                injectedFailure("The maintenance recorder failed."));
        }
    }

    void failPasses() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            failPasses_ = true;
        } catch (...) {
        }
    }

    void blockPasses() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            block_ = true;
            released_ = false;
        } catch (...) {
        }
    }

    void release() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            condition_.notify_all();
        } catch (...) {
            condition_.notify_all();
        }
    }

    [[nodiscard]] bool waitForPassCount(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 2s) const noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return condition_.wait_for(
                lock,
                timeout,
                [this, count]() noexcept { return calls_.size() >= count; });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::vector<MaintenancePass> calls() const
    {
        const std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::size_t maxActiveCalls() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return maxActiveCalls_;
        } catch (...) {
            return 0U;
        }
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::vector<MaintenancePass> calls_;
    std::size_t activeCalls_{};
    std::size_t maxActiveCalls_{};
    bool failPasses_{};
    bool block_{};
    bool released_{};
};

struct RecursiveShutdownState final {
    std::mutex mutex;
    std::condition_variable changed;
    Host::ManagerMaintenanceWorker* worker{};
    bool passEntered{};
    bool recursiveShutdownAllowed{};
    bool recursiveShutdownReturned{};

    void attach(Host::ManagerMaintenanceWorker& value) noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            worker = &value;
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilPassEntered(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex};
            return changed.wait_for(
                lock, timeout, [this]() noexcept { return passEntered; });
        } catch (...) {
            return false;
        }
    }

    void allowRecursiveShutdown() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex};
                recursiveShutdownAllowed = true;
            }
            changed.notify_all();
        } catch (...) {
            changed.notify_all();
        }
    }

    [[nodiscard]] bool waitUntilRecursiveShutdownReturns(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex};
            return changed.wait_for(
                lock,
                timeout,
                [this]() noexcept { return recursiveShutdownReturned; });
        } catch (...) {
            return false;
        }
    }
};

class RecursiveShutdownMaintenanceService final
    : public Contracts::IManagerMaintenanceService {
public:
    explicit RecursiveShutdownMaintenanceService(
        std::shared_ptr<RecursiveShutdownState> state)
        : state_{std::move(state)}
    {
        if (!state_) {
            throw std::invalid_argument{
                "A recursive maintenance state is required."};
        }
    }

    [[nodiscard]] Domain::Result<void> reconcile(
        const Domain::OperationContext&) noexcept override
    {
        Host::ManagerMaintenanceWorker* worker = nullptr;
        try {
            {
                std::unique_lock lock{state_->mutex};
                state_->passEntered = true;
                state_->changed.notify_all();
                state_->changed.wait(lock, [this]() noexcept {
                    return state_->recursiveShutdownAllowed;
                });
                worker = state_->worker;
            }

            if (worker != nullptr) {
                worker->shutdown();
            }

            {
                const std::lock_guard lock{state_->mutex};
                state_->recursiveShutdownReturned = true;
            }
            state_->changed.notify_all();
            return Domain::Result<void>::success();
        } catch (...) {
            state_->changed.notify_all();
            return Domain::Result<void>::failure(
                injectedFailure("Recursive maintenance shutdown failed."));
        }
    }

private:
    std::shared_ptr<RecursiveShutdownState> state_;
};

struct Fixture final {
    explicit Fixture(
        const Host::ManagerMaintenanceWorkerTiming timing = {15ms, 125ms})
        : clock{std::make_shared<AdvancingClock>(
              Domain::MonotonicTimePoint{std::chrono::seconds{123}})},
          uuids{std::make_shared<SequentialUuidGenerator>()},
          service{std::make_shared<RecordingMaintenanceService>()},
          worker{service, clock, uuids, timing}
    {
    }

    std::shared_ptr<AdvancingClock> clock;
    std::shared_ptr<SequentialUuidGenerator> uuids;
    std::shared_ptr<RecordingMaintenanceService> service;
    Host::ManagerMaintenanceWorker worker;
};

void immediateAndPeriodicPassesHaveFreshBoundedContexts()
{
    constexpr auto Timeout = 125ms;
    Fixture fixture{{15ms, Timeout}};

    require(fixture.worker.start().hasValue(), "maintenance worker start");
    require(
        fixture.service->waitForPassCount(3U),
        "immediate and periodic passes did not run");
    fixture.worker.shutdown();

    const auto calls = fixture.service->calls();
    require(calls.size() >= 3U, "fewer than three passes were retained");
    require(
        fixture.service->maxActiveCalls() == 1U,
        "maintenance passes overlapped");
    for (std::size_t index = 0U; index < calls.size(); ++index) {
        const auto expectedDeadline =
            Domain::MonotonicTimePoint{std::chrono::seconds{123}} +
            std::chrono::seconds{static_cast<std::int64_t>(index)} + Timeout;
        require(
            calls[index].deadline == expectedDeadline,
            "maintenance deadline did not use the bounded timeout");
        require(
            calls[index].cancellationPossible,
            "maintenance context did not carry worker cancellation");
        require(
            !calls[index].cancellationRequested,
            "maintenance pass began with cancellation requested");
        for (std::size_t prior = 0U; prior < index; ++prior) {
            require(
                calls[index].operationId != calls[prior].operationId,
                "maintenance operation UUID was reused");
            require(
                calls[index].correlationId != calls[prior].correlationId,
                "maintenance correlation UUID was reused");
        }
    }
}

void failedPassWaitsForNextIntervalWithoutSpinning()
{
    constexpr auto Interval = 60ms;
    Fixture fixture{{Interval, 125ms}};
    fixture.service->failPasses();

    require(fixture.worker.start().hasValue(), "failing worker start");
    require(
        fixture.service->waitForPassCount(2U),
        "failure did not permit the next scheduled pass");
    fixture.worker.shutdown();

    const auto calls = fixture.service->calls();
    require(calls.size() >= 2U, "failing passes were not recorded");
    require(
        calls[1U].enteredAt - calls[0U].enteredAt >= 40ms,
        "typed maintenance failure caused an immediate retry spin");
    require(
        fixture.service->maxActiveCalls() == 1U,
        "failing maintenance passes overlapped");
}

void beginShutdownIsNonblockingAndConcurrentShutdownExactJoins()
{
    constexpr std::size_t CallerCount = 8U;
    Fixture fixture{{1s, 125ms}};
    fixture.service->blockPasses();
    require(fixture.worker.start().hasValue(), "blocking worker start");
    require(
        fixture.service->waitForPassCount(1U),
        "blocking maintenance pass was not entered");

    const auto beginStarted = std::chrono::steady_clock::now();
    fixture.worker.beginShutdown();
    require(
        std::chrono::steady_clock::now() - beginStarted < 250ms,
        "beginShutdown waited for the active maintenance pass");

    std::atomic_size_t invoked{};
    std::atomic_size_t returned{};
    std::array<std::jthread, CallerCount> callers;
    for (auto& caller : callers) {
        caller = std::jthread{[&fixture, &invoked, &returned] {
            invoked.fetch_add(1U, std::memory_order_acq_rel);
            fixture.worker.shutdown();
            returned.fetch_add(1U, std::memory_order_acq_rel);
        }};
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (invoked.load(std::memory_order_acquire) != CallerCount &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (invoked.load(std::memory_order_acquire) != CallerCount) {
        fixture.service->release();
        for (auto& caller : callers) {
            caller.join();
        }
        throw TestFailure{"concurrent shutdown callers did not start"};
    }
    require(
        returned.load(std::memory_order_acquire) == 0U,
        "shutdown returned before the active pass could be exact-joined");

    fixture.service->release();
    for (auto& caller : callers) {
        caller.join();
    }
    require(
        returned.load(std::memory_order_acquire) == CallerCount,
        "concurrent shutdown callers did not share stopped state");
    require(
        fixture.service->calls().size() == 1U,
        "shutdown admitted a successor maintenance pass");
    fixture.worker.beginShutdown();
    fixture.worker.shutdown();
}

void recursiveWorkerShutdownDoesNotWaitOnExternalJoinOwner()
{
    const auto clock =
        std::make_shared<AdvancingClock>(Domain::MonotonicTimePoint{});
    const auto uuids = std::make_shared<SequentialUuidGenerator>();
    const auto state = std::make_shared<RecursiveShutdownState>();
    const auto service =
        std::make_shared<RecursiveShutdownMaintenanceService>(state);
    Host::ManagerMaintenanceWorker worker{
        service, clock, uuids, {1s, 125ms}};
    state->attach(worker);

    require(worker.start().hasValue(), "recursive maintenance worker start");
    require(
        state->waitUntilPassEntered(2s),
        "recursive maintenance pass was not entered");

    std::atomic_bool externalShutdownEntered{};
    std::atomic_bool externalShutdownReturned{};
    std::jthread externalShutdown{[&] {
        externalShutdownEntered.store(true, std::memory_order_release);
        worker.shutdown();
        externalShutdownReturned.store(true, std::memory_order_release);
    }};

    const bool externalJoinOwned =
        Host::Detail::ManagerMaintenanceWorkerTestAccess::
            waitUntilExternalJoinOwned(worker, 2s);
    state->allowRecursiveShutdown();
    const bool recursiveShutdownReturned =
        state->waitUntilRecursiveShutdownReturns(2s);
    if (!recursiveShutdownReturned) {
        Host::Detail::ManagerMaintenanceWorkerTestAccess::
            releaseExternalJoinWaitForFailedTestCleanup(worker);
    }
    externalShutdown.join();

    require(
        externalShutdownEntered.load(std::memory_order_acquire),
        "external maintenance shutdown did not enter");
    require(
        externalJoinOwned,
        "external maintenance shutdown did not claim the join edge");
    require(
        recursiveShutdownReturned,
        "worker-thread recursive shutdown waited on its external join owner");
    require(
        externalShutdownReturned.load(std::memory_order_acquire),
        "external maintenance exact-join owner did not return");
}

void lifecycleAndTimingBoundsAreEnforced()
{
    Fixture fixture;
    require(fixture.worker.start().hasValue(), "bounded worker start");
    requireError(
        fixture.worker.start(),
        Domain::ErrorCodes::Conflict,
        "second maintenance start");
    fixture.worker.shutdown();
    requireError(
        fixture.worker.start(),
        Domain::ErrorCodes::TransportClosed,
        "maintenance start after shutdown");

    const auto clock =
        std::make_shared<AdvancingClock>(Domain::MonotonicTimePoint{});
    const auto uuids = std::make_shared<SequentialUuidGenerator>();
    const auto service = std::make_shared<RecordingMaintenanceService>();
    const auto requireInvalidTiming = [&](
                                          const Host::ManagerMaintenanceWorkerTiming timing) {
        bool rejected = false;
        try {
            Host::ManagerMaintenanceWorker worker{
                service, clock, uuids, timing};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "invalid maintenance timing was accepted");
    };
    requireInvalidTiming({0ms, 1s});
    requireInvalidTiming({1ms, 0ms});
    requireInvalidTiming({
        Host::ManagerMaintenanceWorkerTiming::MaximumInterval + 1ms, 1s});
    requireInvalidTiming({
        1ms,
        Host::ManagerMaintenanceWorkerTiming::MaximumOperationTimeout + 1ms});

    bool nullServiceRejected = false;
    try {
        Host::ManagerMaintenanceWorker worker{
            std::shared_ptr<Contracts::IManagerMaintenanceService>{},
            clock,
            uuids};
    } catch (const std::invalid_argument&) {
        nullServiceRejected = true;
    }
    require(nullServiceRejected, "null maintenance service was accepted");
}

} // namespace

int main()
{
    try {
        immediateAndPeriodicPassesHaveFreshBoundedContexts();
        failedPassWaitsForNextIntervalWithoutSpinning();
        beginShutdownIsNonblockingAndConcurrentShutdownExactJoins();
        recursiveWorkerShutdownDoesNotWaitOnExternalJoinOwner();
        lifecycleAndTimingBoundsAreEnforced();
        std::cout << "Manager maintenance worker tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager maintenance worker tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager maintenance worker tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
