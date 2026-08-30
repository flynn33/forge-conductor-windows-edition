#include "ManagerProcessStopSignal.h"
#include "ManagerProcessStopWatcher.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace ForgeConductor::Hosts::Manager::Detail {

struct ManagerProcessStopWatcherTestAccess final {
    [[nodiscard]] static bool waitUntilExternalJoinOwned(
        ManagerProcessStopWatcher& watcher,
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{watcher.lifecycleMutex_};
            return watcher.lifecycleCondition_.wait_for(
                lock,
                timeout,
                [&watcher]() noexcept { return watcher.cancelJoinOwned_; });
        } catch (...) {
            return false;
        }
    }

    static void releaseExternalJoinWaitForFailedTestCleanup(
        ManagerProcessStopWatcher& watcher) noexcept
    {
        try {
            {
                const std::lock_guard lock{watcher.lifecycleMutex_};
                watcher.cancelJoinOwned_ = false;
            }
            watcher.lifecycleCondition_.notify_all();
        } catch (...) {
            watcher.lifecycleCondition_.notify_all();
        }
    }
};

} // namespace ForgeConductor::Hosts::Manager::Detail

namespace {

namespace Host = ForgeConductor::Hosts::Manager;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Host::ManagerProcessStopWatcher>);
static_assert(std::is_final_v<Host::ManagerProcessHostShutdownTarget>);
static_assert(!std::is_copy_constructible_v<Host::ManagerProcessStopWatcher>);
static_assert(!std::is_move_constructible_v<Host::ManagerProcessStopWatcher>);

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

struct TargetState final {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::size_t shutdownCalls{};
    std::size_t destructions{};
    bool shutdownEntered{};
    bool blockShutdown{};
    bool shutdownReleased{};

    [[nodiscard]] bool waitUntilShutdown(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex};
            return changed.wait_for(
                lock,
                timeout,
                [this]() noexcept { return shutdownEntered; });
        } catch (...) {
            return false;
        }
    }

    void releaseShutdown() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex};
                shutdownReleased = true;
            }
            changed.notify_all();
        } catch (...) {
            changed.notify_all();
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            return shutdownCalls;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t destroyed() const noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            return destructions;
        } catch (...) {
            return 0U;
        }
    }
};

class RecordingShutdownTarget final
    : public Host::IManagerProcessShutdownTarget {
public:
    explicit RecordingShutdownTarget(std::shared_ptr<TargetState> state)
        : state_{std::move(state)}
    {
        if (!state_) {
            throw std::invalid_argument{"A target state is required."};
        }
    }

    ~RecordingShutdownTarget() noexcept override
    {
        try {
            {
                const std::lock_guard lock{state_->mutex};
                ++state_->destructions;
            }
            state_->changed.notify_all();
        } catch (...) {
            state_->changed.notify_all();
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::unique_lock lock{state_->mutex};
            ++state_->shutdownCalls;
            state_->shutdownEntered = true;
            state_->changed.notify_all();
            if (state_->blockShutdown) {
                state_->changed.wait(
                    lock,
                    [this]() noexcept { return state_->shutdownReleased; });
            }
        } catch (...) {
        }
    }

private:
    std::shared_ptr<TargetState> state_;
};

struct RecursiveTargetState final {
    std::mutex mutex;
    std::condition_variable changed;
    Host::ManagerProcessStopWatcher* watcher{};
    std::size_t shutdownCalls{};
    bool shutdownEntered{};
    bool recursiveCancelAllowed{};
    bool recursiveCancelReturned{};

    void attach(Host::ManagerProcessStopWatcher& value) noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            watcher = &value;
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilShutdown(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex};
            return changed.wait_for(
                lock,
                timeout,
                [this]() noexcept { return shutdownEntered; });
        } catch (...) {
            return false;
        }
    }

    void allowRecursiveCancel() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex};
                recursiveCancelAllowed = true;
            }
            changed.notify_all();
        } catch (...) {
            changed.notify_all();
        }
    }

    [[nodiscard]] bool waitUntilRecursiveCancelReturns(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex};
            return changed.wait_for(
                lock,
                timeout,
                [this]() noexcept { return recursiveCancelReturned; });
        } catch (...) {
            return false;
        }
    }
};

class RecursiveCancellationTarget final
    : public Host::IManagerProcessShutdownTarget {
public:
    explicit RecursiveCancellationTarget(
        std::shared_ptr<RecursiveTargetState> state)
        : state_{std::move(state)}
    {
        if (!state_) {
            throw std::invalid_argument{"A recursive target state is required."};
        }
    }

    void shutdown() noexcept override
    {
        Host::ManagerProcessStopWatcher* watcher = nullptr;
        try {
            {
                std::unique_lock lock{state_->mutex};
                ++state_->shutdownCalls;
                state_->shutdownEntered = true;
                state_->changed.notify_all();
                state_->changed.wait(
                    lock,
                    [this]() noexcept {
                        return state_->recursiveCancelAllowed;
                    });
                watcher = state_->watcher;
            }

            if (watcher != nullptr) {
                watcher->cancel();
            }

            {
                const std::lock_guard lock{state_->mutex};
                state_->recursiveCancelReturned = true;
            }
            state_->changed.notify_all();
        } catch (...) {
            state_->changed.notify_all();
        }
    }

private:
    std::shared_ptr<RecursiveTargetState> state_;
};

void stopEdgeShutsDownTargetExactlyOnce()
{
    Host::ManagerProcessStopSignal signal;
    const auto state = std::make_shared<TargetState>();
    const auto target = std::make_shared<RecordingShutdownTarget>(state);
    Host::ManagerProcessStopWatcher watcher{signal, target};

    require(signal.requestStop(), "first process-stop edge was not published");
    require(
        state->waitUntilShutdown(2s),
        "process-stop edge did not reach the shutdown target");
    watcher.cancel();

    require(state->calls() == 1U, "stop edge called shutdown more than once");
    require(!signal.requestStop(), "one-shot process-stop edge was republished");
    watcher.cancel();
    require(state->calls() == 1U, "idempotent cancel repeated shutdown");
}

void cancellationWakesWaitWithoutShuttingDownTarget()
{
    Host::ManagerProcessStopSignal signal;
    const auto state = std::make_shared<TargetState>();
    const auto target = std::make_shared<RecordingShutdownTarget>(state);
    Host::ManagerProcessStopWatcher watcher{signal, target};

    const auto started = std::chrono::steady_clock::now();
    watcher.cancel();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    require(elapsed < 2s, "watcher cancellation did not wake its event wait");
    require(state->calls() == 0U, "cancellation invoked process shutdown");
    require(!signal.requested(), "cancellation published a process-stop edge");
    watcher.cancel();
    require(state->calls() == 0U, "repeated cancellation invoked shutdown");
}

void concurrentCancellationHasOneExactJoinOwner()
{
    constexpr std::size_t CallerCount = 8U;
    Host::ManagerProcessStopSignal signal;
    const auto state = std::make_shared<TargetState>();
    {
        const std::lock_guard lock{state->mutex};
        state->blockShutdown = true;
    }
    const auto target = std::make_shared<RecordingShutdownTarget>(state);
    Host::ManagerProcessStopWatcher watcher{signal, target};

    require(signal.requestStop(), "blocking stop edge was not published");
    require(
        state->waitUntilShutdown(2s),
        "blocking shutdown target was not entered");

    std::atomic_size_t invoked{};
    std::atomic_size_t returned{};
    std::array<std::jthread, CallerCount> callers;
    for (auto& caller : callers) {
        caller = std::jthread{[&watcher, &invoked, &returned] {
            invoked.fetch_add(1U, std::memory_order_acq_rel);
            watcher.cancel();
            returned.fetch_add(1U, std::memory_order_acq_rel);
        }};
    }

    const auto invocationDeadline = std::chrono::steady_clock::now() + 2s;
    while (invoked.load(std::memory_order_acquire) != CallerCount &&
           std::chrono::steady_clock::now() < invocationDeadline) {
        std::this_thread::yield();
    }
    if (invoked.load(std::memory_order_acquire) != CallerCount) {
        state->releaseShutdown();
        for (auto& caller : callers) {
            caller.join();
        }
        throw TestFailure{"concurrent cancel callers did not start"};
    }

    require(
        returned.load(std::memory_order_acquire) == 0U,
        "a concurrent cancel returned before the worker could be joined");
    state->releaseShutdown();
    for (auto& caller : callers) {
        caller.join();
    }

    require(
        returned.load(std::memory_order_acquire) == CallerCount,
        "not every concurrent cancel observed joined state");
    require(state->calls() == 1U, "concurrent cancel repeated shutdown");
}

void recursiveWorkerCancellationDoesNotWaitOnExternalJoinOwner()
{
    Host::ManagerProcessStopSignal signal;
    const auto state = std::make_shared<RecursiveTargetState>();
    const auto target = std::make_shared<RecursiveCancellationTarget>(state);
    Host::ManagerProcessStopWatcher watcher{signal, target};
    state->attach(watcher);

    require(signal.requestStop(), "recursive stop edge was not published");
    require(
        state->waitUntilShutdown(2s),
        "recursive shutdown target was not entered");

    std::atomic_bool externalCancelEntered{};
    std::atomic_bool externalCancelReturned{};
    std::jthread externalCanceller{[&] {
        externalCancelEntered.store(true, std::memory_order_release);
        watcher.cancel();
        externalCancelReturned.store(true, std::memory_order_release);
    }};

    const bool externalJoinOwned =
        Host::Detail::ManagerProcessStopWatcherTestAccess::
            waitUntilExternalJoinOwned(watcher, 2s);
    state->allowRecursiveCancel();
    const bool recursiveCancelReturned =
        state->waitUntilRecursiveCancelReturns(2s);
    if (!recursiveCancelReturned) {
        Host::Detail::ManagerProcessStopWatcherTestAccess::
            releaseExternalJoinWaitForFailedTestCleanup(watcher);
    }
    externalCanceller.join();

    require(
        externalCancelEntered.load(std::memory_order_acquire),
        "external cancel caller did not enter");
    require(externalJoinOwned, "external cancel did not claim the join edge");
    require(
        recursiveCancelReturned,
        "worker-thread recursive cancel waited on its external join owner");
    require(
        externalCancelReturned.load(std::memory_order_acquire),
        "external exact-join owner did not return");
    require(state->shutdownCalls == 1U, "recursive target shutdown was repeated");
}

void watcherRetainsTargetThroughWorkerAndDestruction()
{
    Host::ManagerProcessStopSignal signal;
    const auto state = std::make_shared<TargetState>();
    std::weak_ptr<Host::IManagerProcessShutdownTarget> targetLifetime;
    {
        auto target = std::make_shared<RecordingShutdownTarget>(state);
        targetLifetime = target;
        Host::ManagerProcessStopWatcher watcher{signal, target};
        target.reset();

        require(
            !targetLifetime.expired(),
            "watcher did not retain the shutdown target");
        require(signal.requestStop(), "lifetime stop edge was not published");
        require(
            state->waitUntilShutdown(2s),
            "retained target did not receive shutdown");
        watcher.cancel();
        require(
            !targetLifetime.expired(),
            "watcher released its target before its own destruction");
        require(state->destroyed() == 0U, "target was destroyed too early");
    }

    require(targetLifetime.expired(), "watcher leaked the shutdown target");
    require(state->destroyed() == 1U, "target destruction was not exact");
}

void nullDependenciesAreRejected()
{
    Host::ManagerProcessStopSignal signal;
    bool watcherRejected = false;
    try {
        Host::ManagerProcessStopWatcher watcher{
            signal, std::shared_ptr<Host::IManagerProcessShutdownTarget>{}};
    } catch (const std::invalid_argument&) {
        watcherRejected = true;
    }
    require(watcherRejected, "watcher accepted a null shutdown target");

    bool adapterRejected = false;
    try {
        Host::ManagerProcessHostShutdownTarget target{
            std::shared_ptr<Host::ManagerProcessHost>{}};
    } catch (const std::invalid_argument&) {
        adapterRejected = true;
    }
    require(adapterRejected, "host adapter accepted a null process host");
}

} // namespace

int main()
{
    try {
        stopEdgeShutsDownTargetExactlyOnce();
        cancellationWakesWaitWithoutShuttingDownTarget();
        concurrentCancellationHasOneExactJoinOwner();
        recursiveWorkerCancellationDoesNotWaitOnExternalJoinOwner();
        watcherRetainsTargetThroughWorkerAndDestruction();
        nullDependenciesAreRejected();
        std::cout << "Manager process stop watcher tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager process stop watcher tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager process stop watcher tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
