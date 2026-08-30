#include "ManagerProcessWorkerGroup.h"

#include <algorithm>
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
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Host::ManagerProcessWorkerGroup>);
static_assert(!std::is_copy_constructible_v<Host::ManagerProcessWorkerGroup>);
static_assert(!std::is_move_constructible_v<Host::ManagerProcessWorkerGroup>);

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

class EventLog final {
public:
    void append(std::string event)
    {
        const std::lock_guard lock{mutex_};
        events_.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<std::string> snapshot() const
    {
        const std::lock_guard lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};

struct WorkerState final {
    WorkerState(std::string workerName, std::shared_ptr<EventLog> eventLog)
        : name{std::move(workerName)}, events{std::move(eventLog)}
    {
    }

    void failStart() noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            startFails = true;
        } catch (...) {
        }
    }

    void blockStart() noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            startBlocks = true;
            startEntered = false;
            startReleased = false;
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilStart(
        const std::chrono::milliseconds timeout = 2s) noexcept
    {
        try {
            std::unique_lock lock{mutex};
            return changed.wait_for(
                lock,
                timeout,
                [this]() noexcept { return startEntered; });
        } catch (...) {
            return false;
        }
    }

    void blockShutdown() noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            shutdownBlocks = true;
            shutdownReleased = false;
        } catch (...) {
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

    [[nodiscard]] bool waitUntilShutdown(
        const std::chrono::milliseconds timeout = 2s) noexcept
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

    [[nodiscard]] std::size_t starts() const noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            return startCalls;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t begins() const noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            return beginCalls;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t shutdowns() const noexcept
    {
        try {
            const std::lock_guard lock{mutex};
            return shutdownCalls;
        } catch (...) {
            return 0U;
        }
    }

    std::string name;
    std::shared_ptr<EventLog> events;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::size_t startCalls{};
    std::size_t beginCalls{};
    std::size_t shutdownCalls{};
    bool startFails{};
    bool startBlocks{};
    bool startEntered{};
    bool startReleased{};
    bool shutdownBlocks{};
    bool shutdownEntered{};
    bool shutdownReleased{};
};

class RecordingWorker final : public Host::IManagerTransitionWorker {
public:
    explicit RecordingWorker(std::shared_ptr<WorkerState> state)
        : state_{std::move(state)}
    {
        if (!state_ || !state_->events) {
            throw std::invalid_argument{"A recording worker state is required."};
        }
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        try {
            bool fail = false;
            {
                std::unique_lock lock{state_->mutex};
                ++state_->startCalls;
                state_->startEntered = true;
                state_->events->append(state_->name + ".start");
                state_->changed.notify_all();
                if (state_->startBlocks) {
                    state_->changed.wait(
                        lock,
                        [this]() noexcept { return state_->startReleased; });
                }
                fail = state_->startFails;
            }
            if (fail) {
                return Domain::Result<void>::failure(
                    injectedFailure(state_->name + " start failed."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(
                injectedFailure("Recording worker start failed."));
        }
    }

    void beginShutdown() noexcept override
    {
        try {
            {
                const std::lock_guard lock{state_->mutex};
                ++state_->beginCalls;
                state_->startReleased = true;
            }
            state_->changed.notify_all();
            state_->events->append(state_->name + ".begin");
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        try {
            {
                std::unique_lock lock{state_->mutex};
                ++state_->shutdownCalls;
                state_->shutdownEntered = true;
                state_->changed.notify_all();
                if (state_->shutdownBlocks) {
                    state_->changed.wait(
                        lock,
                        [this]() noexcept {
                            return state_->shutdownReleased;
                        });
                }
            }
            state_->events->append(state_->name + ".shutdown");
        } catch (...) {
        }
    }

private:
    std::shared_ptr<WorkerState> state_;
};

struct GroupFixture final {
    GroupFixture()
        : events{std::make_shared<EventLog>()},
          first{std::make_shared<WorkerState>("first", events)},
          second{std::make_shared<WorkerState>("second", events)},
          third{std::make_shared<WorkerState>("third", events)}
    {
    }

    [[nodiscard]] std::vector<std::unique_ptr<Host::IManagerTransitionWorker>>
    workers() const
    {
        std::vector<std::unique_ptr<Host::IManagerTransitionWorker>> result;
        result.reserve(3U);
        result.push_back(std::make_unique<RecordingWorker>(first));
        result.push_back(std::make_unique<RecordingWorker>(second));
        result.push_back(std::make_unique<RecordingWorker>(third));
        return result;
    }

    std::shared_ptr<EventLog> events;
    std::shared_ptr<WorkerState> first;
    std::shared_ptr<WorkerState> second;
    std::shared_ptr<WorkerState> third;
};

void startOrderAndReverseShutdownAreExact()
{
    GroupFixture fixture;
    Host::ManagerProcessWorkerGroup group{fixture.workers()};

    require(group.start().hasValue(), "worker group start");
    requireError(
        group.start(),
        Domain::ErrorCodes::Conflict,
        "second worker group start");
    const auto beginStarted = std::chrono::steady_clock::now();
    group.beginShutdown();
    require(
        std::chrono::steady_clock::now() - beginStarted < 250ms,
        "worker group beginShutdown performed a join");
    group.shutdown();
    group.beginShutdown();
    group.shutdown();

    const std::vector<std::string> expected{
        "first.start",
        "second.start",
        "third.start",
        "third.begin",
        "second.begin",
        "first.begin",
        "third.shutdown",
        "second.shutdown",
        "first.shutdown"};
    require(
        fixture.events->snapshot() == expected,
        "worker group lifecycle order was not exact");
    require(fixture.first->starts() == 1U, "first worker start was not exact");
    require(fixture.second->begins() == 1U, "second worker begin was not exact");
    require(fixture.third->shutdowns() == 1U, "third worker shutdown was not exact");
}

void startFailureRollsBackOnlyStartedPrefixInReverse()
{
    GroupFixture fixture;
    fixture.third->failStart();
    Host::ManagerProcessWorkerGroup group{fixture.workers()};

    const auto result = group.start();
    requireError(
        result,
        Domain::ErrorCodes::InternalFailure,
        "worker group start failure");
    requireError(
        group.start(),
        Domain::ErrorCodes::TransportClosed,
        "worker group restart after rollback");
    group.shutdown();

    const std::vector<std::string> expected{
        "first.start",
        "second.start",
        "third.start",
        "second.begin",
        "first.begin",
        "second.shutdown",
        "first.shutdown"};
    require(
        fixture.events->snapshot() == expected,
        "failed worker group did not roll back the started prefix exactly");
    require(fixture.third->begins() == 0U, "failed child was rollback-signalled");
    require(fixture.third->shutdowns() == 0U, "failed child was rollback-joined");
}

void concurrentShutdownHasOneReverseFinalizationOwner()
{
    constexpr std::size_t CallerCount = 8U;
    GroupFixture fixture;
    fixture.third->blockShutdown();
    Host::ManagerProcessWorkerGroup group{fixture.workers()};
    require(group.start().hasValue(), "concurrent group start");
    group.beginShutdown();

    std::atomic_size_t invoked{};
    std::atomic_size_t returned{};
    std::array<std::jthread, CallerCount> callers;
    for (auto& caller : callers) {
        caller = std::jthread{[&group, &invoked, &returned] {
            invoked.fetch_add(1U, std::memory_order_acq_rel);
            group.shutdown();
            returned.fetch_add(1U, std::memory_order_acq_rel);
        }};
    }

    require(
        fixture.third->waitUntilShutdown(),
        "reverse group shutdown did not enter the final child first");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (invoked.load(std::memory_order_acquire) != CallerCount &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (invoked.load(std::memory_order_acquire) != CallerCount) {
        fixture.third->releaseShutdown();
        for (auto& caller : callers) {
            caller.join();
        }
        throw TestFailure{"concurrent group shutdown callers did not start"};
    }
    require(
        returned.load(std::memory_order_acquire) == 0U,
        "group shutdown returned before reverse finalization completed");

    fixture.third->releaseShutdown();
    for (auto& caller : callers) {
        caller.join();
    }
    require(
        returned.load(std::memory_order_acquire) == CallerCount,
        "concurrent group shutdown did not publish stopped state");
    require(fixture.first->shutdowns() == 1U, "first child shutdown repeated");
    require(fixture.second->shutdowns() == 1U, "second child shutdown repeated");
    require(fixture.third->shutdowns() == 1U, "third child shutdown repeated");
}

void beginShutdownDuringStartRollsBackWithoutWaiting()
{
    GroupFixture fixture;
    fixture.first->blockStart();
    Host::ManagerProcessWorkerGroup group{fixture.workers()};
    std::optional<Domain::Result<void>> startResult;
    std::jthread starter{[&group, &startResult] {
        startResult.emplace(group.start());
    }};

    require(
        fixture.first->waitUntilStart(),
        "worker group did not enter the first child start");
    const auto beginStarted = std::chrono::steady_clock::now();
    group.beginShutdown();
    require(
        std::chrono::steady_clock::now() - beginStarted < 250ms,
        "worker group beginShutdown waited for startup finalization");
    starter.join();

    require(startResult.has_value(), "racing group start returned no result");
    requireError(
        *startResult,
        Domain::ErrorCodes::TransportClosed,
        "worker group stop during startup");
    require(fixture.first->starts() == 1U, "first child was not started once");
    require(fixture.second->starts() == 0U, "second child started after stop");
    require(fixture.third->starts() == 0U, "third child started after stop");
    require(fixture.first->shutdowns() == 1U, "first racing child was not joined");
    require(fixture.second->shutdowns() == 1U, "second stopped child was not finalized");
    require(fixture.third->shutdowns() == 1U, "third stopped child was not finalized");
    group.shutdown();
}

void workerSetAndLifecycleBoundsAreEnforced()
{
    bool emptyRejected = false;
    try {
        Host::ManagerProcessWorkerGroup group{
            std::vector<std::unique_ptr<Host::IManagerTransitionWorker>>{}};
    } catch (const std::invalid_argument&) {
        emptyRejected = true;
    }
    require(emptyRejected, "empty worker group was accepted");

    bool nullRejected = false;
    try {
        std::vector<std::unique_ptr<Host::IManagerTransitionWorker>> workers;
        workers.push_back(nullptr);
        Host::ManagerProcessWorkerGroup group{std::move(workers)};
    } catch (const std::invalid_argument&) {
        nullRejected = true;
    }
    require(nullRejected, "null worker group child was accepted");

    bool capacityRejected = false;
    try {
        const auto events = std::make_shared<EventLog>();
        std::vector<std::unique_ptr<Host::IManagerTransitionWorker>> workers;
        for (std::size_t index = 0U;
             index < Host::ManagerProcessWorkerGroup::MaximumWorkers + 1U;
             ++index) {
            auto state = std::make_shared<WorkerState>(
                "bounded-" + std::to_string(index), events);
            workers.push_back(std::make_unique<RecordingWorker>(state));
        }
        Host::ManagerProcessWorkerGroup group{std::move(workers)};
    } catch (const std::invalid_argument&) {
        capacityRejected = true;
    }
    require(capacityRejected, "oversized worker group was accepted");

    GroupFixture fixture;
    Host::ManagerProcessWorkerGroup group{fixture.workers()};
    group.shutdown();
    requireError(
        group.start(),
        Domain::ErrorCodes::TransportClosed,
        "worker group start after pre-shutdown");
}

} // namespace

int main()
{
    try {
        startOrderAndReverseShutdownAreExact();
        startFailureRollsBackOnlyStartedPrefixInReverse();
        concurrentShutdownHasOneReverseFinalizationOwner();
        beginShutdownDuringStartRollsBackWithoutWaiting();
        workerSetAndLifecycleBoundsAreEnforced();
        std::cout << "Manager process worker group tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager process worker group tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager process worker group tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
