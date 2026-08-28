#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"

#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using namespace std::chrono_literals;

using Deadline = Windows::WindowsDashboardDeadline;
using DeadlineKind = Windows::WindowsDashboardDeadlineKind;
using DeadlineRequest = Windows::WindowsDashboardDeadlineRequest;
using Scheduler = Windows::WindowsDashboardDeadlineScheduler;

static_assert(std::is_final_v<Scheduler>);
static_assert(std::is_abstract_v<Windows::IWindowsDashboardDeadlineSink>);
static_assert(std::is_aggregate_v<DeadlineRequest>);
static_assert(!std::is_copy_constructible_v<Scheduler>);
static_assert(!std::is_move_constructible_v<Scheduler>);
static_assert(Scheduler::HardMaximumScheduledCount == 44U);
static_assert(noexcept(Scheduler::create({}, {})));
static_assert(noexcept(std::declval<Scheduler&>().schedule({})));
static_assert(noexcept(std::declval<Scheduler&>().cancel(1U, 1U)));
static_assert(noexcept(std::declval<const Scheduler&>().snapshot()));
static_assert(noexcept(std::declval<Scheduler&>().shutdown()));

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

class RecordingSink final : public Windows::IWindowsDashboardDeadlineSink {
public:
    void signal(Deadline deadline) noexcept override
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                deadlines_.push_back(std::move(deadline));
            }
            changed_.notify_all();
        } catch (...) {
            failed_.store(true, std::memory_order_release);
            changed_.notify_all();
        }
    }

    [[nodiscard]] bool waitForCount(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] {
            return failed_.load(std::memory_order_acquire) ||
                deadlines_.size() >= count;
        }) && !failed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<Deadline> deadlines() const
    {
        const std::lock_guard lock{mutex_};
        return deadlines_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Deadline> deadlines_;
    std::atomic_bool failed_{};
};

class OffsetFrozenClock final : public Contracts::IClock {
public:
    explicit OffsetFrozenClock(
        const Domain::MonotonicTimePoint initial) noexcept
        : ticks_{initial.time_since_epoch().count()}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        calls_.fetch_add(1U, std::memory_order_relaxed);
        return Domain::MonotonicTimePoint{
            Domain::MonotonicTimePoint::duration{
                ticks_.load(std::memory_order_acquire)}};
    }

    void set(const Domain::MonotonicTimePoint value) noexcept
    {
        ticks_.store(
            value.time_since_epoch().count(), std::memory_order_release);
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<Domain::MonotonicTimePoint::duration::rep> ticks_{};
    mutable std::atomic_size_t calls_{};
};

class ReleasingSink final : public Windows::IWindowsDashboardDeadlineSink {
public:
    explicit ReleasingSink(std::unique_ptr<Scheduler>& owner) noexcept
        : owner_{owner}
    {
    }

    void signal(Deadline) noexcept override
    {
        owner_.reset();
        {
            const std::lock_guard lock{mutex_};
            signalled_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait()
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 2s, [this] { return signalled_; });
    }

private:
    std::unique_ptr<Scheduler>& owner_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool signalled_{};
};

class BlockingSink final : public Windows::IWindowsDashboardDeadlineSink {
public:
    void signal(Deadline deadline) noexcept override
    {
        try {
            {
                std::unique_lock lock{mutex_};
                if (entered_) {
                    unexpectedSignal_ = true;
                    changed_.notify_all();
                    return;
                }
                observed_.emplace(std::move(deadline));
                entered_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this] { return released_; });
                completed_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitUntilEntered()
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitUntilCompleted()
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 2s, [this] { return completed_; });
    }

    [[nodiscard]] std::optional<Deadline> observed() const
    {
        const std::lock_guard lock{mutex_};
        return observed_;
    }

    [[nodiscard]] bool unexpectedSignal() const
    {
        const std::lock_guard lock{mutex_};
        return unexpectedSignal_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Deadline> observed_;
    bool entered_{};
    bool released_{};
    bool completed_{};
    bool unexpectedSignal_{};
};

class BlockingSinkReleaseGuard final {
public:
    explicit BlockingSinkReleaseGuard(
        std::shared_ptr<BlockingSink> sink) noexcept
        : sink_{std::move(sink)}
    {
    }

    ~BlockingSinkReleaseGuard() noexcept { sink_->release(); }

    BlockingSinkReleaseGuard(const BlockingSinkReleaseGuard&) = delete;
    BlockingSinkReleaseGuard& operator=(
        const BlockingSinkReleaseGuard&) = delete;

private:
    std::shared_ptr<BlockingSink> sink_;
};

[[nodiscard]] std::unique_ptr<Scheduler> scheduler(
    const std::shared_ptr<RecordingSink>& sink,
    const std::size_t maximum = Scheduler::HardMaximumScheduledCount)
{
    return take(Scheduler::create(
        std::make_shared<Windows::SystemClock>(), sink, maximum));
}

void constructionRejectsInvalidDependenciesAndLimits()
{
    const auto clock = std::make_shared<Windows::SystemClock>();
    const auto sink = std::make_shared<RecordingSink>();

    const auto missingClock = Scheduler::create({}, sink);
    require(!missingClock, "missing clock was accepted");
    require(
        missingClock.error().code == Domain::ErrorCodes::InvalidRequest,
        "missing clock used the wrong error code");

    const std::weak_ptr<Windows::IWindowsDashboardDeadlineSink> expired;
    const auto missingSink = Scheduler::create(clock, expired);
    require(!missingSink, "expired sink was accepted");

    const auto zero = Scheduler::create(clock, sink, 0U);
    require(!zero, "zero capacity was accepted");

    const auto oversized = Scheduler::create(
        clock, sink, Scheduler::HardMaximumScheduledCount + 1U);
    require(!oversized, "capacity above the hard ceiling was accepted");
}

void registrationIsBoundedAndReplacementDoesNotAccumulate()
{
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, 2U);
    const auto future = std::chrono::steady_clock::now() + 1h;

    const auto first = take(owner->schedule(
        {1U, DeadlineKind::HeaderIngress, future}));
    const auto second = take(owner->schedule(
        {2U, DeadlineKind::SocketLifetime, future}));
    require(first.registrationId == 1U, "first token changed its owner id");
    require(first.armSequence == 1U, "first token did not start at one");
    require(first.kind == DeadlineKind::HeaderIngress,
            "first token changed its kind");
    require(first.deadline == future, "first token changed its deadline");
    require(second.registrationId == 2U, "second token changed its owner id");
    require(second.armSequence == 2U,
            "arm sequence was not global across owner ids");
    auto snapshot = owner->snapshot();
    require(snapshot.scheduledCount() == 2U, "capacity count was incorrect");
    require(snapshot.maximumScheduledCount() == 2U,
            "configured capacity was not retained");
    require(!snapshot.isShutdown(), "live scheduler reported shutdown");

    const auto missingOwner = owner->schedule(
        {0U, DeadlineKind::HeaderIngress, future});
    require(!missingOwner, "zero owner identifier was accepted");
    require(
        missingOwner.error().code == Domain::ErrorCodes::InvalidRequest,
        "zero owner identifier used the wrong error code");

    const auto exhausted = owner->schedule(
        {3U, DeadlineKind::ServerSentEventsLifetime, future});
    require(!exhausted, "capacity overflow was accepted");
    require(
        exhausted.error().code == Domain::ErrorCodes::LimitExceeded &&
            exhausted.error().retryable,
        "capacity overflow did not use the retryable limit error");

    const auto replacement = take(owner->schedule(
        {1U, DeadlineKind::ListenerRetirement, future + 1s}));
    require(replacement.armSequence == 3U,
            "failed admissions consumed or replacement reused a sequence");
    require(owner->snapshot().scheduledCount() == 2U,
            "replacement accumulated a stale entry");
    require(!owner->cancel(first.registrationId, first.armSequence),
            "stale cancellation removed a successor arm");
    require(owner->snapshot().scheduledCount() == 2U,
            "stale cancellation changed deadline count");
    require(owner->cancel(second.registrationId, second.armSequence),
            "live deadline was not cancelled");
    require(!owner->cancel(second.registrationId, second.armSequence),
            "cancel succeeded twice");
    require(!owner->cancel(0U, 0U),
            "invalid registration cancellation succeeded");
    require(owner->snapshot().scheduledCount() == 1U,
            "cancel did not release one entry");
}

void expiredDeadlinesAreDeliveredOnceInStableOrder()
{
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, 4U);
    const auto due = std::chrono::steady_clock::now() + 250ms;

    const auto first = take(owner->schedule(
        {8U, DeadlineKind::SocketLifetime, due}));
    const auto second = take(owner->schedule(
        {7U, DeadlineKind::HeaderIngress, due}));
    require(sink->waitForCount(2U), "expired deadlines were not delivered");

    const auto delivered = sink->deadlines();
    require(delivered.size() == 2U, "expired deadline count was incorrect");
    require(delivered[0U].registrationId == 7U &&
                delivered[1U].registrationId == 8U,
            "equal deadlines did not use stable registration order");
    require(delivered[0U].kind == DeadlineKind::HeaderIngress,
            "deadline kind was not retained");
    require(delivered[0U].armSequence == second.armSequence,
            "deadline arm sequence was not retained");
    require(delivered[1U].armSequence == first.armSequence,
            "second delivered arm sequence was not retained");
    require(owner->snapshot().scheduledCount() == 0U,
            "delivered deadlines remained registered");

    std::this_thread::sleep_for(20ms);
    require(sink->deadlines().size() == 2U,
            "an expired deadline was delivered more than once");
}

void undefinedKindsAndCapacityRejectionsDoNotConsumeSequences()
{
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, 1U);
    const auto future = std::chrono::steady_clock::now() + 1h;
    constexpr auto UndefinedKind =
        static_cast<DeadlineKind>(0xffU);

    const auto invalidEmpty = owner->schedule(
        {1U, UndefinedKind, future});
    require(!invalidEmpty, "an undefined deadline kind was accepted");
    require(invalidEmpty.error().code == Domain::ErrorCodes::InvalidRequest,
            "an undefined deadline kind used the wrong error code");
    require(owner->snapshot().scheduledCount() == 0U,
            "an undefined deadline kind consumed capacity");

    const auto first = take(owner->schedule(
        {1U, DeadlineKind::HandlerExecution, future}));
    require(first.armSequence == 1U,
            "an undefined deadline kind consumed an arm sequence");

    const auto invalidReplacement = owner->schedule(
        {1U, UndefinedKind, future + 1s});
    require(!invalidReplacement,
            "an undefined replacement deadline kind was accepted");
    require(owner->snapshot().scheduledCount() == 1U,
            "an undefined replacement changed live capacity");

    const auto capacityRejected = owner->schedule(
        {2U, DeadlineKind::ServerSentEventsDelivery, future});
    require(!capacityRejected,
            "a capacity-exhausted deadline was accepted");
    require(capacityRejected.error().code == Domain::ErrorCodes::LimitExceeded,
            "capacity rejection used the wrong error code");
    require(owner->cancel(first.registrationId, first.armSequence),
            "the original deadline changed after rejected work");

    const auto second = take(owner->schedule(
        {2U, DeadlineKind::ServerSentEventsDelivery, future}));
    require(second.armSequence == 2U,
            "a rejected deadline consumed an arm sequence");
    require(owner->cancel(second.registrationId, second.armSequence),
            "the successor deadline could not be cancelled");
}

void injectedOffsetFrozenClockUsesBoundedRelativeWaits()
{
    const auto injectedNow = std::chrono::steady_clock::now() + 24h;
    const auto due = injectedNow + 100ms;
    const auto clock = std::make_shared<OffsetFrozenClock>(injectedNow);
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = take(Scheduler::create(clock, sink, 1U));
    const auto scheduled = take(owner->schedule(
        {41U, DeadlineKind::HandlerExecution, due}));
    const auto callsBeforeWait = clock->calls();

    require(!sink->waitForCount(1U, 350ms),
            "a frozen injected clock delivered from wall-clock passage");
    const auto callsDuringWait = clock->calls() - callsBeforeWait;
    require(callsDuringWait <= 12U,
            "a frozen injected clock caused a hot reevaluation loop");
    require(owner->snapshot().scheduledCount() == 1U,
            "a frozen injected clock discarded its future deadline");

    clock->set(due);
    require(sink->waitForCount(1U, 2s),
            "an advanced injected clock did not release its deadline");
    const auto delivered = sink->deadlines();
    require(delivered.size() == 1U && delivered.front() == scheduled,
            "offset-clock delivery changed the exact deadline token");
    require(owner->snapshot().scheduledCount() == 0U,
            "offset-clock delivery retained its deadline");
}

void everyDefinedDeadlineKindRoundTrips()
{
    constexpr std::array Kinds{
        DeadlineKind::HeaderIngress,
        DeadlineKind::HandlerExecution,
        DeadlineKind::SocketLifetime,
        DeadlineKind::ServerSentEventsLifetime,
        DeadlineKind::ServerSentEventsDelivery,
        DeadlineKind::ListenerRetirement,
        DeadlineKind::ShutdownDrain};
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, Kinds.size());
    const auto due = std::chrono::steady_clock::now() - 1s;

    for (std::size_t index{}; index < Kinds.size(); ++index) {
        const auto scheduled = take(owner->schedule(
            {index + 1U, Kinds[index], due}));
        require(scheduled.kind == Kinds[index],
                "a defined deadline kind changed during scheduling");
    }
    require(sink->waitForCount(Kinds.size()),
            "not every defined deadline kind was delivered");

    const auto delivered = sink->deadlines();
    require(delivered.size() == Kinds.size(),
            "defined deadline delivery count changed");
    for (std::size_t index{}; index < Kinds.size(); ++index) {
        require(delivered[index].kind == Kinds[index],
                "a defined deadline kind changed during delivery");
    }
}

void replacementControlsTheDeliveredValue()
{
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, 1U);
    const auto future = std::chrono::steady_clock::now() + 1h;
    const auto past = std::chrono::steady_clock::now() - 1s;

    const auto original = take(owner->schedule(
        {11U, DeadlineKind::HeaderIngress, future}));
    const auto replacement = take(owner->schedule(
        {11U, DeadlineKind::ShutdownDrain, past}));
    require(replacement.armSequence > original.armSequence,
            "replacement did not receive a newer global token");
    require(sink->waitForCount(1U), "replacement was not delivered");
    const auto delivered = sink->deadlines();
    require(delivered.size() == 1U, "replacement delivered multiple entries");
    require(delivered.front().registrationId == 11U &&
                delivered.front().armSequence == replacement.armSequence &&
                delivered.front().kind == DeadlineKind::ShutdownDrain,
            "the superseded deadline was delivered");
}

void rearmAfterDequeueReceivesAnUnambiguousNewToken()
{
    const auto sink = std::make_shared<BlockingSink>();
    auto owner = take(Scheduler::create(
        std::make_shared<Windows::SystemClock>(), sink, 1U));
    BlockingSinkReleaseGuard releaseGuard{sink};
    const auto now = std::chrono::steady_clock::now();

    const auto oldArm = take(owner->schedule(
        {21U, DeadlineKind::HeaderIngress, now - 1s}));
    require(sink->waitUntilEntered(),
            "old arm was not dequeued into the blocking sink");
    require(owner->snapshot().scheduledCount() == 0U,
            "dequeued old arm remained live in the scheduler");

    const auto successor = take(owner->schedule(
        {21U, DeadlineKind::HeaderIngress, now + 1h}));
    require(successor.registrationId == oldArm.registrationId,
            "successor changed the stable owner identifier");
    require(successor.armSequence > oldArm.armSequence,
            "successor reused the dequeued arm token");
    require(owner->snapshot().scheduledCount() == 1U,
            "successor arm was not retained while old delivery was blocked");
    require(!owner->cancel(oldArm.registrationId, oldArm.armSequence),
            "stale exact cancellation removed the successor");
    require(owner->snapshot().scheduledCount() == 1U,
            "stale cancellation changed successor state");

    sink->release();
    require(sink->waitUntilCompleted(),
            "blocked old delivery did not complete after release");
    const auto observed = sink->observed();
    require(observed.has_value(), "blocking sink lost the old token");
    require(observed.value() == oldArm,
            "old delivery changed after the successor was armed");
    require(!sink->unexpectedSignal(),
            "future successor was delivered before cancellation");
    require(owner->cancel(
                successor.registrationId, successor.armSequence),
            "exact successor cancellation failed");
    owner->shutdown();
}

void shutdownIsIdempotentAndClosesRegistration()
{
    const auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, 2U);
    const auto future = std::chrono::steady_clock::now() + 1h;
    const auto scheduled = take(owner->schedule(
        {1U, DeadlineKind::HeaderIngress, future}));

    owner->shutdown();
    owner->shutdown();
    const auto snapshot = owner->snapshot();
    require(snapshot.isShutdown(), "shutdown state was not published");
    require(snapshot.scheduledCount() == 0U,
            "shutdown retained deadline entries");
    require(!owner->cancel(
                scheduled.registrationId, scheduled.armSequence),
            "shutdown scheduler cancelled a cleared entry");

    const auto rejected = owner->schedule(
        {2U, DeadlineKind::SocketLifetime, future});
    require(!rejected, "shutdown scheduler accepted work");
    require(
        rejected.error().code == Domain::ErrorCodes::TransportClosed,
        "shutdown registration used the wrong error code");
}

void concurrentShutdownClaimsTheWorkerExactlyOnce()
{
    const auto sink = std::make_shared<BlockingSink>();
    auto owner = take(Scheduler::create(
        std::make_shared<Windows::SystemClock>(), sink, 1U));
    constexpr std::size_t CallerCount = 32U;
    std::vector<std::jthread> callers;
    callers.reserve(CallerCount);
    BlockingSinkReleaseGuard releaseGuard{sink};
    const auto past = std::chrono::steady_clock::now() - 1s;
    const auto scheduled = take(owner->schedule(
        {1U, DeadlineKind::SocketLifetime, past}));
    require(sink->waitUntilEntered(),
            "shutdown test deadline did not enter its blocking sink");
    std::latch ready{CallerCount};
    std::latch start{1};
    std::atomic_size_t returned{};

    for (std::size_t index{}; index < CallerCount; ++index) {
        callers.emplace_back([&](std::stop_token) {
            ready.count_down();
            start.wait();
            owner->shutdown();
            returned.fetch_add(1U, std::memory_order_release);
        });
    }
    ready.wait();
    start.count_down();

    const auto shutdownObservedBy = std::chrono::steady_clock::now() + 2s;
    while (!owner->snapshot().isShutdown() &&
           std::chrono::steady_clock::now() < shutdownObservedBy) {
        std::this_thread::yield();
    }
    require(owner->snapshot().isShutdown(),
            "concurrent callers did not begin shutdown");
    std::this_thread::sleep_for(250ms);
    require(returned.load(std::memory_order_acquire) == 0U,
            "an off-worker shutdown returned before worker exit");

    sink->release();
    callers.clear();
    require(returned.load(std::memory_order_acquire) == CallerCount,
            "not every concurrent shutdown caller returned");

    const auto snapshot = owner->snapshot();
    require(snapshot.isShutdown(),
            "concurrent shutdown did not retain shutdown state");
    require(snapshot.scheduledCount() == 0U,
            "concurrent shutdown retained a deadline");
    require(!owner->cancel(
                scheduled.registrationId, scheduled.armSequence),
            "concurrent shutdown left its exact arm cancellable");
    const auto rejected = owner->schedule(
        {2U,
         DeadlineKind::HeaderIngress,
         std::chrono::steady_clock::now() + 1h});
    require(!rejected, "concurrently shut down scheduler accepted work");
    require(rejected.error().code == Domain::ErrorCodes::TransportClosed,
            "concurrent shutdown used the wrong rejection code");
}

void expiredSinkIsSafeAndStillReleasesEntries()
{
    auto sink = std::make_shared<RecordingSink>();
    auto owner = scheduler(sink, 1U);
    sink.reset();
    const auto scheduled = owner->schedule(
        {1U,
         DeadlineKind::ServerSentEventsLifetime,
         std::chrono::steady_clock::now() - 1s});
    require(static_cast<bool>(scheduled),
            "deadline with expired observer was rejected");

    const auto end = std::chrono::steady_clock::now() + 2s;
    while (owner->snapshot().scheduledCount() != 0U &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
    }
    require(owner->snapshot().scheduledCount() == 0U,
            "expired observer retained a due entry");
}

void sinkMayReleaseTheOuterOwnerWithoutSelfJoin()
{
    std::unique_ptr<Scheduler> owner;
    const auto sink = std::make_shared<ReleasingSink>(owner);
    owner = take(Scheduler::create(
        std::make_shared<Windows::SystemClock>(), sink, 1U));
    const auto scheduled = owner->schedule(
        {1U,
         DeadlineKind::ShutdownDrain,
         std::chrono::steady_clock::now() - 1s});
    require(static_cast<bool>(scheduled),
            "reentrant release deadline was rejected");
    require(sink->wait(), "reentrant release sink was not invoked");
    require(owner == nullptr, "sink did not release the outer owner");
}

} // namespace

int main()
{
    try {
        constructionRejectsInvalidDependenciesAndLimits();
        registrationIsBoundedAndReplacementDoesNotAccumulate();
        undefinedKindsAndCapacityRejectionsDoNotConsumeSequences();
        injectedOffsetFrozenClockUsesBoundedRelativeWaits();
        expiredDeadlinesAreDeliveredOnceInStableOrder();
        everyDefinedDeadlineKindRoundTrips();
        replacementControlsTheDeliveredValue();
        rearmAfterDequeueReceivesAnUnambiguousNewToken();
        shutdownIsIdempotentAndClosesRegistration();
        concurrentShutdownClaimsTheWorkerExactlyOnce();
        expiredSinkIsSafeAndStillReleasesEntries();
        sinkMayReleaseTheOuterOwnerWithoutSelfJoin();
        std::cout << "Windows dashboard deadline scheduler tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows dashboard deadline scheduler tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Windows dashboard deadline scheduler tests failed with an unknown error.\n";
        return 1;
    }
}
