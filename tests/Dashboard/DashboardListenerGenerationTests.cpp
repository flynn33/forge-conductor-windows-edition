#include "Infrastructure/Windows/Detail/DashboardListenerGeneration.h"
#include "Infrastructure/Windows/Detail/DashboardListenerGenerationCoordinator.h"
#include "Infrastructure/Windows/Detail/DashboardListeningSocket.h"
#include "Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.h"
#include "Infrastructure/Windows/Detail/DashboardWinsockRuntime.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/Error.h"

#include <MSWSock.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using namespace std::chrono_literals;

using AcceptOwner = Detail::IDashboardListenerGenerationAcceptOwner;
using Authority = Detail::DashboardFixedIocpKeyAuthority;
using CompletionKey = Detail::DashboardIoCompletionKey;
using ConnectionControl =
    Detail::IDashboardListenerGenerationConnectionControl;
using Coordinator = Detail::DashboardListenerGenerationCoordinator;
using Deadline = Windows::WindowsDashboardDeadline;
using DeadlineKind = Windows::WindowsDashboardDeadlineKind;
using Factory = Detail::IDashboardListenerGenerationFactory;
using Generation = Detail::DashboardListenerGeneration;
using GenerationInterface = Detail::IDashboardListenerGeneration;
using GenerationDeadlineScheduler =
    Detail::IDashboardListenerGenerationDeadlineScheduler;
using Lifecycle = Detail::DashboardListenerGenerationLifecycle;
using ListenerLeasePool =
    Detail::DashboardListenerCompletionKeyLeasePool;
using RegistrationHost =
    Detail::IDashboardListenerGenerationRegistrationHost;
using RuntimeIdentity = Detail::DashboardConnectionRuntimeIdentity;
using RuntimeServices = Detail::DashboardConnectionRuntimeServices;
using Scheduler = Windows::WindowsDashboardDeadlineScheduler;
using TransitionGate = Detail::DashboardListenerGenerationTransitionGate;

static_assert(std::is_final_v<Generation>);
static_assert(std::is_final_v<Coordinator>);
static_assert(std::is_final_v<TransitionGate>);
static_assert(std::is_abstract_v<GenerationInterface>);
static_assert(std::is_abstract_v<Factory>);
static_assert(std::is_abstract_v<RegistrationHost>);
static_assert(std::is_abstract_v<ConnectionControl>);
static_assert(std::is_abstract_v<GenerationDeadlineScheduler>);
static_assert(Generation::RetirementLifetime == 5s);
static_assert(Generation::CancellationReapLifetime == 5s);
static_assert(!std::is_copy_constructible_v<Generation>);
static_assert(!std::is_move_constructible_v<Generation>);
static_assert(noexcept(std::declval<Generation&>().consume({}, 0U)));
static_assert(noexcept(std::declval<Generation&>().dispatchDeadline({})));
static_assert(noexcept(std::declval<Generation&>().beginShutdown()));
static_assert(noexcept(std::declval<Generation&>().beginGracefulShutdown(
    std::declval<TransitionGate::Guard&>())));
static_assert(noexcept(std::declval<Coordinator&>().beginGracefulShutdown()));
static_assert(noexcept(std::declval<const Generation&>().snapshot()));

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

void takeVoid(Domain::Result<void> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const bool retryable,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, "wrong stable error code");
    require(result.error().retryable == retryable,
            "wrong retryable classification");
}

class FrozenClock final : public Contracts::IClock {
public:
    explicit FrozenClock(const Domain::MonotonicTimePoint now) noexcept
        : now_{now}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return now_;
    }

private:
    Domain::MonotonicTimePoint now_;
};

class FixedUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        return Domain::Uuid::parse(
            "11111111-1111-4111-8111-111111111111");
    }
};

class ActiveState final : public Detail::IDashboardOperationalStateSource {
public:
    [[nodiscard]] bool operationalServiceActive() const noexcept override
    {
        return true;
    }
};

class DeadlineSink final : public Windows::IWindowsDashboardDeadlineSink {
public:
    void signal(Deadline deadline) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        last_.emplace(std::move(deadline));
    }

private:
    std::mutex mutex_;
    std::optional<Deadline> last_;
};

class FakeAcceptOwner final : public AcceptOwner {
public:
    explicit FakeAcceptOwner(const RuntimeIdentity identity) noexcept
        : identity_{identity}
    {
    }

    [[nodiscard]] RuntimeIdentity identity() const noexcept override
    {
        return identity_;
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        ++startCalls;
        if (failStart) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "accept start failed"));
        }
        started = true;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> closeAdmission() noexcept override
    {
        ++closeCalls;
        closed = true;
        if (drainOnClose) {
            drained = true;
        }
        if (closeFailure.has_value()) {
            return Domain::Result<void>::failure(*closeFailure);
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> forceCloseListener()
        noexcept override
    {
        ++forceCloseCalls;
        closed = true;
        forceClosed = true;
        if (drainOnForceClose) {
            drained = true;
        }
        if (forceCloseFailure.has_value()) {
            return Domain::Result<void>::failure(*forceCloseFailure);
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> consume(
        Detail::DashboardIoCompletionPacket,
        DWORD) noexcept override
    {
        ++consumeCalls;
        if (drainOnConsume &&
            consumeCalls >= consumeCallsRequiredForDrain) {
            drained = true;
        }
        if (failConsume) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "accept resume failed after registration"));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool fullyDrained() const noexcept override
    {
        return drained.load(std::memory_order_acquire);
    }

    bool failStart{};
    bool failConsume{};
    bool drainOnClose{};
    bool drainOnForceClose{};
    bool drainOnConsume{true};
    bool started{};
    bool closed{};
    bool forceClosed{};
    std::atomic_bool drained{};
    std::size_t startCalls{};
    std::size_t closeCalls{};
    std::size_t forceCloseCalls{};
    std::size_t consumeCalls{};
    std::size_t consumeCallsRequiredForDrain{1U};
    std::optional<Domain::Error> closeFailure;
    std::optional<Domain::Error> forceCloseFailure;

private:
    const RuntimeIdentity identity_;
};

class FakeConnectionControl final : public ConnectionControl {
public:
    [[nodiscard]] std::size_t connectionCountForGeneration(
        const std::uint64_t generationId) const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return generationId == generationId_ ? count_ : 0U;
    }

    void beginShutdownGeneration(
        const std::uint64_t generationId) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (generationId == generationId_) {
            ++shutdownCalls_;
            count_ = 0U;
        }
    }

    void set(const std::uint64_t generationId, const std::size_t count)
        noexcept
    {
        const std::scoped_lock lock{mutex_};
        generationId_ = generationId;
        count_ = count;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return shutdownCalls_;
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t generationId_{};
    std::size_t count_{};
    std::size_t shutdownCalls_{};
};

class FakeOverloadResponder final
    : public Detail::IDashboardAdmissionOverloadResponder {
public:
    void respond(Detail::DashboardAdmissionOverloadWork) noexcept override
    {
    }

    [[nodiscard]] std::size_t cancelGeneration(
        const std::uint64_t generationId) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        cancelledIds_.push_back(generationId);
        if (stageTerminalOnCancel && !terminalPending_ &&
            !terminalDelivered_) {
            terminalPending_ = true;
        }
        return cancelledWorkCount;
    }

    void drainTerminalGenerationNotifications() noexcept override
    {
        std::function<void()> callback;
        {
            const std::scoped_lock lock{mutex_};
            ++terminalDrainCalls_;
            if (terminalPending_) {
                terminalPending_ = false;
                terminalDelivered_ = true;
                callback = terminalCallback_;
            }
        }
        if (callback) {
            callback();
        }
    }

    void setTerminalCallback(std::function<void()> callback)
    {
        const std::scoped_lock lock{mutex_};
        terminalCallback_ = std::move(callback);
    }

    [[nodiscard]] std::size_t terminalDrainCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return terminalDrainCalls_;
    }

    [[nodiscard]] bool callbackSourceLockAvailable() noexcept
    {
        if (!mutex_.try_lock()) {
            return false;
        }
        mutex_.unlock();
        return true;
    }

    [[nodiscard]] std::vector<std::uint64_t> cancelledIds() const
    {
        const std::scoped_lock lock{mutex_};
        return cancelledIds_;
    }

    std::size_t cancelledWorkCount{1U};
    bool stageTerminalOnCancel{};

private:
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> cancelledIds_;
    std::function<void()> terminalCallback_;
    std::size_t terminalDrainCalls_{};
    bool terminalPending_{};
    bool terminalDelivered_{};
};

class RecordingListenerFailFast final
    : public Detail::IDashboardListenerGenerationFailFast {
public:
    void failFast() noexcept override
    {
        calls.fetch_add(1U, std::memory_order_relaxed);
    }

    std::atomic_size_t calls{};
};

class ReentrantListenerFailFast final
    : public Detail::IDashboardListenerGenerationFailFast {
public:
    void bind(std::weak_ptr<Generation> generation) noexcept
    {
        generation_ = std::move(generation);
    }

    void failFast() noexcept override
    {
        calls.fetch_add(1U, std::memory_order_relaxed);
        const auto generation = generation_.lock();
        if (generation != nullptr) {
            reentryStarted.store(true, std::memory_order_release);
            generation->beginShutdown();
            reentryCompleted.store(true, std::memory_order_release);
        }
    }

    std::atomic_size_t calls{};
    std::atomic_bool reentryStarted{};
    std::atomic_bool reentryCompleted{};

private:
    std::weak_ptr<Generation> generation_;
};

class ControlledGenerationDeadlineScheduler final
    : public GenerationDeadlineScheduler {
public:
    enum class SecondSchedule : std::uint8_t {
        ReturnSuccessAfterRelease,
        FailImmediately,
    };

    explicit ControlledGenerationDeadlineScheduler(
        const SecondSchedule secondSchedule) noexcept
        : secondSchedule_{secondSchedule}
    {
    }

    [[nodiscard]] Domain::Result<Deadline> schedule(
        Windows::WindowsDashboardDeadlineRequest request) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++scheduleCalls_;
            const Deadline scheduled{
                request.registrationId,
                nextArmSequence_++,
                request.kind,
                request.deadline};
            if (scheduleCalls_ == 2U) {
                secondEntered_ = true;
                if (secondSchedule_ ==
                    SecondSchedule::ReturnSuccessAfterRelease) {
                    live_.emplace(scheduled);
                }
                changed_.notify_all();
                if (secondSchedule_ ==
                    SecondSchedule::ReturnSuccessAfterRelease) {
                    changed_.wait(lock, [this]() noexcept {
                        return releaseSecond_;
                    });
                } else {
                    return Domain::Result<Deadline>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "Injected cancellation-watchdog schedule failure.",
                            true));
                }
            } else {
                live_.emplace(scheduled);
            }
            return Domain::Result<Deadline>::success(scheduled);
        } catch (...) {
            return Domain::Result<Deadline>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Controlled deadline scheduling failed safely."));
        }
    }

    [[nodiscard]] bool cancel(
        const std::uint64_t registrationId,
        const std::uint64_t armSequence) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (!live_.has_value() ||
            live_->registrationId != registrationId ||
            live_->armSequence != armSequence) {
            return false;
        }
        ++cancelCalls_;
        lastCancelled_.emplace(*live_);
        live_.reset();
        return true;
    }

    [[nodiscard]] bool waitForSecondSchedule(
        const std::chrono::milliseconds timeout) noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this]() noexcept {
            return secondEntered_;
        });
    }

    void releaseSecondSchedule() noexcept
    {
        {
            const std::scoped_lock lock{mutex_};
            releaseSecond_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t scheduleCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return scheduleCalls_;
    }

    [[nodiscard]] std::size_t scheduledCount() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return live_.has_value() ? 1U : 0U;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return cancelCalls_;
    }

    [[nodiscard]] std::optional<Deadline> lastCancelled() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return lastCancelled_;
    }

private:
    const SecondSchedule secondSchedule_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Deadline> live_;
    std::optional<Deadline> lastCancelled_;
    std::uint64_t nextArmSequence_{1U};
    std::size_t scheduleCalls_{};
    std::size_t cancelCalls_{};
    bool secondEntered_{};
    bool releaseSecond_{};
};

class GenerationDrainObserver final
    : public Detail::IDashboardListenerGenerationDrainObserver {
public:
    void generationMayHaveDrained(
        const std::uint64_t registrationId) noexcept override
    {
        observedId.store(registrationId, std::memory_order_release);
        calls.fetch_add(1U, std::memory_order_relaxed);
    }

    std::atomic_uint64_t observedId{};
    std::atomic_size_t calls{};
};

struct RuntimeFixture final {
    explicit RuntimeFixture(
        const Domain::MonotonicTimePoint now =
            Domain::MonotonicTimePoint{1s})
        : clock{std::make_shared<FrozenClock>(now)},
          uuid{std::make_shared<FixedUuidGenerator>()},
          state{std::make_shared<ActiveState>()},
          deadlineSink{std::make_shared<DeadlineSink>()},
          scheduler{take(Scheduler::create(clock, deadlineSink))},
          runtime{take(RuntimeServices::create(clock, uuid, state))}
    {
    }

    ~RuntimeFixture() noexcept { scheduler->shutdown(); }

    std::shared_ptr<FrozenClock> clock;
    std::shared_ptr<FixedUuidGenerator> uuid;
    std::shared_ptr<ActiveState> state;
    std::shared_ptr<DeadlineSink> deadlineSink;
    std::unique_ptr<Scheduler> scheduler;
    std::unique_ptr<RuntimeServices> runtime;
};

void testModeledSharedCallbackPinRetainsListenerKeyLease()
{
    RuntimeFixture fixture;
    const auto authority = take(Authority::create());
    auto leasePool = take(ListenerLeasePool::create(authority));
    auto listenerLease = take(leasePool->tryAcquire());
    const RuntimeIdentity identity{
        40U, listenerLease.completionKey()};
    auto accept = std::make_unique<FakeAcceptOwner>(identity);
    accept->drainOnForceClose = true;
    auto connections = std::make_shared<FakeConnectionControl>();
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto ordinaryOwner = take(Generation::create(
        identity,
        std::move(listenerLease),
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate));

    // Model the shared generation pin that completion-router and deadline
    // callbacks take before dispatching outside their registration locks.
    auto modeledCallbackPin = ordinaryOwner;
    ordinaryOwner->beginShutdown();
    require(ordinaryOwner->fullyDrained(),
            "the leased generation did not drain before owner release");
    ordinaryOwner.reset();

    {
        auto slotB = take(leasePool->tryAcquire());
        require(slotB.completionKey() == authority.listenerSlotB(),
                "an outstanding generation pin released listener slot A");
        const auto exhausted = leasePool->tryAcquire();
        require(!exhausted &&
                    exhausted.error().code == Domain::ErrorCodes::Conflict &&
                    exhausted.error().retryable,
                "a modeled callback pin allowed a third listener lease");
    }

    {
        auto slotBAgain = take(leasePool->tryAcquire());
        require(slotBAgain.completionKey() == authority.listenerSlotB(),
                "listener slot A became reusable before the final pin left");
    }

    modeledCallbackPin.reset();
    auto reusedA = take(leasePool->tryAcquire());
    require(reusedA.completionKey() == authority.listenerSlotA(),
            "the final generation pin did not return listener slot A");
}

void testConcreteGracefulShutdownPreservesConnectionsUntilHardEscalation()
{
    RuntimeFixture fixture;
    constexpr std::uint64_t generationId{51U};
    const auto completionKey = CompletionKey{0x5100U};
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{generationId, completionKey});
    auto* const acceptView = accept.get();
    acceptView->drainOnClose = false;
    acceptView->drainOnForceClose = true;
    auto connections = std::make_shared<FakeConnectionControl>();
    connections->set(generationId, 2U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto generation = take(Generation::create(
        RuntimeIdentity{generationId, completionKey},
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate));

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
    }
    {
        auto transition = gate->enter();
        takeVoid(generation->beginRetirement(transition));
    }
    require(generation->snapshot().retirementDeadline() != nullptr,
            "concrete graceful test did not begin with a live retirement arm");
    {
        auto transition = gate->enter();
        takeVoid(generation->beginGracefulShutdown(transition));
    }
    {
        auto transition = gate->enter();
        takeVoid(generation->beginGracefulShutdown(transition));
    }

    const auto graceful = generation->snapshot();
    require(graceful.lifecycle() == Lifecycle::ShuttingDown &&
                acceptView->closeCalls == 2U &&
                acceptView->forceCloseCalls == 0U,
            "graceful listener shutdown repeated its close or entered force-close policy");
    require(connections->shutdownCalls() == 0U &&
                connections->connectionCountForGeneration(generationId) ==
                    2U,
            "graceful listener shutdown shortened existing connections");
    require(overload->cancelledIds() ==
                std::vector<std::uint64_t>{generationId},
            "graceful listener shutdown did not cancel exact overload work once");
    require(graceful.retirementDeadline() == nullptr &&
                graceful.retirementCancellationRequested() &&
                !graceful.listenerForceCloseRequested() &&
                graceful.cancellationReapDeadline() == nullptr,
            "graceful listener shutdown retained retirement or entered hard watchdog policy");

    generation->beginShutdown();
    require(acceptView->forceCloseCalls == 1U &&
                connections->shutdownCalls() == 1U,
            "hard escalation did not force-close graceful generation ownership");
    require(overload->cancelledIds() ==
                std::vector<std::uint64_t>{generationId, generationId},
            "hard escalation skipped exact overload cancellation");
    require(generation->fullyDrained(),
            "hard escalation did not drain the graceful generation seam");
}

void testExactRetirementDeadlineAndPartialAcceptDrain()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{41U, CompletionKey{0x4100U}});
    auto* const acceptView = accept.get();
    auto connections = std::make_shared<FakeConnectionControl>();
    connections->set(41U, 1U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto generation = take(Generation::create(
        RuntimeIdentity{41U, CompletionKey{0x4100U}},
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate));
    auto observer = std::make_shared<GenerationDrainObserver>();
    takeVoid(generation->bindDrainObserver(observer));

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
    }
    require(acceptView->started, "generation did not start acceptance");
    require(generation->snapshot().lifecycle() == Lifecycle::Admitting,
            "started generation used wrong lifecycle");

    {
        auto transition = gate->enter();
        takeVoid(generation->beginRetirement(transition));
    }
    const auto retiring = generation->snapshot();
    require(retiring.lifecycle() == Lifecycle::Retiring,
            "generation did not enter retirement");
    require(retiring.retirementDeadline() != nullptr,
            "retirement omitted exact deadline arm");
    require(
        retiring.retirementDeadline()->deadline ==
            fixture.clock->monotonicNow() + 5s,
        "retirement deadline was not exactly five seconds");
    require(acceptView->closed,
            "retirement did not close listener admission");

    auto stale = *retiring.retirementDeadline();
    ++stale.armSequence;
    generation->dispatchDeadline(stale);
    require(connections->shutdownCalls() == 0U,
            "stale deadline force-closed a generation");
    require(generation->snapshot().retirementDeadline() != nullptr,
            "stale deadline removed exact live arm");

    generation->dispatchDeadline(*retiring.retirementDeadline());
    require(connections->shutdownCalls() == 1U,
            "exact deadline did not force-close exact generation");
    require(overload->cancelledIds() == std::vector<std::uint64_t>{41U},
            "exact deadline cancelled the wrong overload generation");
    require(generation->snapshot().retirementCancellationRequested(),
            "exact deadline did not request retirement cancellation");
    require(!generation->fullyDrained(),
            "partial accept drain released generation too early");

    generation->consume(
        Detail::DashboardIoCompletionPacket{
            0U, CompletionKey{0x4100U}, nullptr},
        ERROR_OPERATION_ABORTED);
    require(generation->fullyDrained(),
            "last accept completion did not drain generation");
    require(observer->calls.load(std::memory_order_relaxed) == 1U,
            "generation drain observer did not receive one edge");
    require(observer->observedId.load(std::memory_order_acquire) == 41U,
            "generation drain observer received wrong identity");
}

void testPostRegistrationResumeFailureForceClosesGeneration()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{42U, CompletionKey{0x4200U}});
    auto* const acceptView = accept.get();
    acceptView->failConsume = true;
    acceptView->drainOnConsume = true;
    auto connections = std::make_shared<FakeConnectionControl>();
    // Models one existing connection plus the just-registered handoff owner.
    connections->set(42U, 2U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto generation = take(Generation::create(
        RuntimeIdentity{42U, CompletionKey{0x4200U}},
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate));

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
    }
    generation->consume(
        Detail::DashboardIoCompletionPacket{
            0U, CompletionKey{0x4200U}, nullptr},
        ERROR_SUCCESS);

    require(acceptView->closed,
            "post-registration resume failure left admission open");
    require(connections->shutdownCalls() == 1U,
            "post-registration resume failure did not force-close generation");
    require(overload->cancelledIds() == std::vector<std::uint64_t>{42U},
            "resume failure cancelled the wrong overload generation");
    require(connections->connectionCountForGeneration(42U) == 0U,
            "force-close seam retained generation connections");
    require(generation->fullyDrained(),
            "resume failure owner did not drain after exact close/reap seam");
}

void testCancellationFailureForceCloseAndBoundedReapWatchdog()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{45U, CompletionKey{0x4500U}});
    auto* const acceptView = accept.get();
    const auto cancellationFailure = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "Injected exact CancelIoEx failure.");
    acceptView->closeFailure = cancellationFailure;
    acceptView->forceCloseFailure = cancellationFailure;
    auto connections = std::make_shared<FakeConnectionControl>();
    connections->set(45U, 1U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto failFast = std::make_shared<RecordingListenerFailFast>();
    auto generation = take(Generation::create(
        RuntimeIdentity{45U, CompletionKey{0x4500U}},
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate,
        failFast));

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
        takeVoid(generation->beginRetirement(transition));
    }
    const auto retiring = generation->snapshot();
    require(retiring.lifecycle() == Lifecycle::Retiring &&
                retiring.hasFailure(),
            "cancellation failure escaped retirement retention");
    require(!retiring.listenerForceCloseRequested() &&
                retiring.cancellationReapDeadline() == nullptr,
            "retirement force-closed before its hard boundary");
    require(acceptView->closeCalls == 1U &&
                acceptView->forceCloseCalls == 0U,
            "retirement used the wrong cancellation phase");
    const auto retained = generation->fullFailure();
    require(retained.has_value() && *retained == cancellationFailure,
            "generation did not retain the exact cancellation failure");

    const auto retirementDeadline = *retiring.retirementDeadline();
    generation->dispatchDeadline(retirementDeadline);
    const auto forced = generation->snapshot();
    require(acceptView->forceClosed &&
                acceptView->forceCloseCalls == 1U &&
                forced.listenerForceCloseRequested(),
            "retirement boundary did not force-close the listener once");
    require(forced.retirementDeadline() == nullptr &&
                forced.cancellationReapDeadline() != nullptr,
            "retirement boundary did not replace its deadline with a watchdog");
    const auto reapDeadline = *forced.cancellationReapDeadline();
    require(reapDeadline.armSequence != retirementDeadline.armSequence &&
                reapDeadline.deadline ==
                    retirementDeadline.deadline + 5s,
            "cancellation-reap watchdog did not use one exact five-second arm");
    require(connections->shutdownCalls() == 1U &&
                overload->cancelledIds() ==
                    std::vector<std::uint64_t>{45U},
            "retirement boundary missed exact generation shutdown");

    generation->dispatchDeadline(retirementDeadline);
    require(failFast->calls.load(std::memory_order_relaxed) == 0U &&
                generation->snapshot().cancellationReapDeadline() !=
                    nullptr,
            "stale retirement deadline consumed the cancellation watchdog");

    generation->dispatchDeadline(reapDeadline);
    const auto expired = generation->snapshot();
    require(failFast->calls.load(std::memory_order_relaxed) == 1U &&
                expired.failFastCount() == 1U &&
                expired.lifecycle() == Lifecycle::Fatal,
            "expired cancellation-reap watchdog did not invoke fail-fast once");
    require(expired.cancellationReapDeadline() == nullptr,
            "expired cancellation-reap watchdog remained armed");
    generation->dispatchDeadline(reapDeadline);
    require(failFast->calls.load(std::memory_order_relaxed) == 1U &&
                generation->snapshot().failFastCount() == 1U,
            "stale watchdog repeated the fail-fast boundary");

    acceptView->drained.store(true, std::memory_order_release);
    generation->ownershipMayHaveDrained();
    require(generation->fullyDrained(),
            "post-watchdog exact ownership drain was not collectable");
}

void testCancellationWatchdogPublicationRaceCancelsUncommittedArm()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{46U, CompletionKey{0x4600U}});
    auto* const acceptView = accept.get();
    auto connections = std::make_shared<FakeConnectionControl>();
    connections->set(46U, 0U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto scheduler =
        std::make_shared<ControlledGenerationDeadlineScheduler>(
            ControlledGenerationDeadlineScheduler::SecondSchedule::
                ReturnSuccessAfterRelease);
    auto failFast = std::make_shared<RecordingListenerFailFast>();
    auto generation = take(Generation::create(
        RuntimeIdentity{46U, CompletionKey{0x4600U}},
        std::move(accept),
        scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate,
        failFast));

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
        takeVoid(generation->beginRetirement(transition));
    }
    const auto retirement = *generation->snapshot().retirementDeadline();

    std::thread deadlineThread{[generation, retirement]() noexcept {
        generation->dispatchDeadline(retirement);
    }};
    const auto entered = scheduler->waitForSecondSchedule(2s);
    if (!entered) {
        scheduler->releaseSecondSchedule();
        deadlineThread.join();
        fail("cancellation watchdog did not enter its publication barrier");
    }

    // Models the last overload-held accept resume token returning after the
    // arm became live in the scheduler but before Generation received it.
    acceptView->drained.store(true, std::memory_order_release);
    generation->ownershipMayHaveDrained();
    require(generation->fullyDrained(),
            "last ownership edge did not drain during schedule publication");

    scheduler->releaseSecondSchedule();
    deadlineThread.join();

    const auto cancelled = scheduler->lastCancelled();
    require(scheduler->scheduleCalls() == 2U &&
                scheduler->cancelCalls() == 1U &&
                scheduler->scheduledCount() == 0U,
            "drain race retained a live uncommitted watchdog arm");
    require(cancelled.has_value() &&
                cancelled->registrationId == 46U &&
                cancelled->armSequence != retirement.armSequence,
            "drain race cancelled anything other than the successor arm");
    const auto drained = generation->snapshot();
    require(drained.lifecycle() == Lifecycle::Drained &&
                drained.cancellationReapDeadline() == nullptr,
            "drain race published a watchdog into a drained owner");
    require(failFast->calls.load(std::memory_order_relaxed) == 0U,
            "successful drained publication race invoked fail-fast");
}

void testWatchdogScheduleFailureInvokesFailFastAfterTransitionRelease()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{47U, CompletionKey{0x4700U}});
    auto* const acceptView = accept.get();
    auto connections = std::make_shared<FakeConnectionControl>();
    connections->set(47U, 1U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto scheduler =
        std::make_shared<ControlledGenerationDeadlineScheduler>(
            ControlledGenerationDeadlineScheduler::SecondSchedule::
                FailImmediately);
    auto failFast = std::make_shared<ReentrantListenerFailFast>();
    auto generation = take(Generation::create(
        RuntimeIdentity{47U, CompletionKey{0x4700U}},
        std::move(accept),
        scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate,
        failFast));
    failFast->bind(generation);

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
        takeVoid(generation->beginRetirement(transition));
    }
    const auto retirement = *generation->snapshot().retirementDeadline();
    generation->dispatchDeadline(retirement);

    require(scheduler->scheduleCalls() == 2U,
            "force-close boundary did not attempt one bounded watchdog arm");
    require(failFast->calls.load(std::memory_order_relaxed) == 1U &&
                failFast->reentryStarted.load(std::memory_order_acquire) &&
                failFast->reentryCompleted.load(std::memory_order_acquire),
            "schedule-failure fail-fast could not re-enter after gate release");
    const auto failed = generation->snapshot();
    require(failed.lifecycle() == Lifecycle::Fatal &&
                failed.failFastCount() == 1U &&
                failed.cancellationReapDeadline() == nullptr,
            "schedule failure did not retain one fatal boundary disposition");
    require(acceptView->forceCloseCalls == 2U,
            "re-entrant shutdown did not reach the idempotent native owner");
}

void testTerminalPumpRunsAfterGenerationTransitionRelease()
{
    const auto runCase = [](const bool consumeFailure) {
        RuntimeFixture fixture;
        const auto generationId = consumeFailure ? 48U : 49U;
        const auto completionKey = CompletionKey{
            consumeFailure ? 0x4800U : 0x4900U};
        auto accept = std::make_unique<FakeAcceptOwner>(
            RuntimeIdentity{generationId, completionKey});
        auto* const acceptView = accept.get();
        acceptView->failConsume = consumeFailure;
        acceptView->drainOnConsume = false;
        auto connections = std::make_shared<FakeConnectionControl>();
        connections->set(generationId, 1U);
        auto overload = std::make_shared<FakeOverloadResponder>();
        overload->stageTerminalOnCancel = true;
        auto gate = std::make_shared<TransitionGate>();
        auto generation = take(Generation::create(
            RuntimeIdentity{generationId, completionKey},
            std::move(accept),
            *fixture.scheduler,
            *fixture.runtime,
            connections,
            overload,
            gate));

        auto terminalCalls = std::make_shared<std::atomic_size_t>(0U);
        auto reentryCompleted =
            std::make_shared<std::atomic_bool>(false);
        auto callbackSourceLockAvailable =
            std::make_shared<std::atomic_bool>(false);
        overload->setTerminalCallback(
            [generation = std::weak_ptr<Generation>{generation},
             overload,
             terminalCalls,
             reentryCompleted,
             callbackSourceLockAvailable]() noexcept {
                terminalCalls->fetch_add(
                    1U, std::memory_order_relaxed);
                callbackSourceLockAvailable->store(
                    overload->callbackSourceLockAvailable(),
                    std::memory_order_release);
                if (const auto pinned = generation.lock();
                    pinned != nullptr) {
                    pinned->beginShutdown();
                    reentryCompleted->store(
                        true, std::memory_order_release);
                }
            });

        {
            auto transition = gate->enter();
            takeVoid(generation->startAdmission(transition));
        }
        if (consumeFailure) {
            generation->consume(
                Detail::DashboardIoCompletionPacket{
                    0U, completionKey, nullptr},
                ERROR_GEN_FAILURE);
        } else {
            generation->beginShutdown();
        }

        require(terminalCalls->load(std::memory_order_relaxed) == 1U &&
                    reentryCompleted->load(std::memory_order_acquire),
                "terminal pump could not re-enter generation shutdown");
        require(callbackSourceLockAvailable->load(
                    std::memory_order_acquire),
                "terminal pump invoked its observer under the source lock");
        require(overload->terminalDrainCalls() >= 2U,
                "terminal pump did not run after outer and re-entrant transitions");
        require(overload->cancelledIds() ==
                    std::vector<std::uint64_t>{
                        generationId, generationId},
                "terminal re-entry repeated or skipped exact cancellation");
        require(connections->shutdownCalls() == 2U,
                "terminal re-entry repeated or skipped connection shutdown");

        acceptView->drained.store(true, std::memory_order_release);
        generation->ownershipMayHaveDrained();
        require(generation->fullyDrained(),
                "terminal pump test retained fake accept ownership");
    };

    // Covers both generation.consume's terminal cancel path and a direct
    // beginShutdown/cancelGeneration transition. In each case, the callback
    // re-enters the same nonrecursive gate and therefore proves pump timing.
    runCase(true);
    runCase(false);
}

void testGenerationRejectsAcceptOwnerIdentityMismatch()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{440U, CompletionKey{0x4400U}});
    auto connections = std::make_shared<FakeConnectionControl>();
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    const auto result = Generation::create(
        RuntimeIdentity{44U, CompletionKey{0x4400U}},
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate);

    requireError(
        result,
        Domain::ErrorCodes::InvalidRequest,
        false,
        "listener generation accepted a foreign accept-owner identity");
    require(connections->shutdownCalls() == 0U &&
                overload->cancelledIds().empty(),
            "identity rejection entered listener shutdown or native work");
}

enum class EventKind : std::uint8_t {
    Prepare,
    RegisterCompletion,
    RegisterDeadline,
    Start,
    Retire,
    GracefulShutdown,
    Shutdown,
    Fatal,
    UnregisterDeadline,
    UnregisterCompletion,
};

struct Event final {
    EventKind kind{};
    std::uint64_t generationId{};
};

class Trace final {
public:
    void record(const EventKind kind, const std::uint64_t generationId)
    {
        const std::scoped_lock lock{mutex_};
        events_.push_back(Event{kind, generationId});
    }

    [[nodiscard]] std::size_t indexOf(
        const EventKind kind,
        const std::uint64_t generationId) const
    {
        const std::scoped_lock lock{mutex_};
        for (std::size_t index{}; index < events_.size(); ++index) {
            if (events_[index].kind == kind &&
                events_[index].generationId == generationId) {
                return index;
            }
        }
        fail("expected trace event was absent");
    }

    [[nodiscard]] std::size_t count(const EventKind kind) const noexcept
    {
        const std::scoped_lock lock{mutex_};
        std::size_t count{};
        for (const auto& event : events_) {
            if (event.kind == kind) {
                ++count;
            }
        }
        return count;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Event> events_;
};

class FakeGeneration final : public GenerationInterface {
public:
    FakeGeneration(
        const std::uint64_t id,
        std::shared_ptr<TransitionGate> gate,
        std::shared_ptr<Trace> trace) noexcept
        : id_{id},
          key_{static_cast<std::uintptr_t>(0x5000U + id)},
          gate_{std::move(gate)},
          trace_{std::move(trace)}
    {
    }

    [[nodiscard]] CompletionKey completionKey() const noexcept override
    {
        return key_;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return id_;
    }

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<Detail::IDashboardListenerGenerationDrainObserver>
            observer) noexcept override
    {
        observer_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> startAdmission(
        TransitionGate::Guard& transition) noexcept override
    {
        if (!transition.belongsTo(*gate_)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "wrong transition gate"));
        }
        trace_->record(EventKind::Start, id_);
        started = true;
        if (startEdge) {
            startEdge();
        }
        if (failStart) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "fake start failed"));
        }
        if (drainDuringStart) {
            drained.store(true, std::memory_order_release);
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> beginGracefulShutdown(
        TransitionGate::Guard& transition) noexcept override
    {
        if (!transition.belongsTo(*gate_)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "wrong transition gate"));
        }
        trace_->record(EventKind::GracefulShutdown, id_);
        gracefulShutdownCalls.fetch_add(1U, std::memory_order_relaxed);
        gracefulShutdownCalled = true;
        if (gracefulShutdownEdge) {
            gracefulShutdownEdge(id_);
        }
        if (drainOnGracefulShutdown) {
            setDrained();
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> beginRetirement(
        TransitionGate::Guard& transition) noexcept override
    {
        if (!transition.belongsTo(*gate_)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "wrong transition gate"));
        }
        trace_->record(EventKind::Retire, id_);
        retired = true;
        if (failRetire) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "fake retirement arm failed", true));
        }
        return Domain::Result<void>::success();
    }

    void consume(Detail::DashboardIoCompletionPacket, DWORD) noexcept override
    {
    }

    void dispatchDeadline(Deadline) noexcept override {}

    void fatal(DWORD) noexcept override
    {
        trace_->record(EventKind::Fatal, id_);
        fatalCalls.fetch_add(1U, std::memory_order_relaxed);
        fatalCalled = true;
        if (drainOnShutdown) {
            setDrained();
        }
    }

    void beginShutdown() noexcept override
    {
        trace_->record(EventKind::Shutdown, id_);
        shutdownCalls.fetch_add(1U, std::memory_order_relaxed);
        shutdownCalled = true;
        if (shutdownEdge) {
            shutdownEdge(id_);
        }
        if (drainOnShutdown) {
            setDrained();
        }
    }

    [[nodiscard]] bool fullyDrained() const noexcept override
    {
        return drained.load(std::memory_order_acquire);
    }

    void ownershipMayHaveDrained() noexcept override
    {
        ownershipRechecks.fetch_add(1U, std::memory_order_relaxed);
        if (drainWhenOwnershipRechecked) {
            setDrained();
        }
    }

    void setDrained() noexcept
    {
        drained.store(true, std::memory_order_release);
        if (const auto observer = observer_.lock(); observer != nullptr) {
            observer->generationMayHaveDrained(id_);
        }
    }

    bool failStart{};
    bool failRetire{};
    bool drainDuringStart{};
    bool drainOnShutdown{true};
    bool drainOnGracefulShutdown{true};
    bool started{};
    bool retired{};
    bool gracefulShutdownCalled{};
    bool shutdownCalled{};
    bool fatalCalled{};
    bool drainWhenOwnershipRechecked{};
    std::function<void(std::uint64_t)> shutdownEdge;
    std::function<void(std::uint64_t)> gracefulShutdownEdge;
    std::function<void()> startEdge;
    std::atomic_bool drained{};
    std::atomic_size_t ownershipRechecks{};
    std::atomic_size_t shutdownCalls{};
    std::atomic_size_t gracefulShutdownCalls{};
    std::atomic_size_t fatalCalls{};

private:
    std::uint64_t id_{};
    CompletionKey key_{0U};
    std::shared_ptr<TransitionGate> gate_;
    std::shared_ptr<Trace> trace_;
    std::weak_ptr<Detail::IDashboardListenerGenerationDrainObserver>
        observer_;
};

class FakeFactory final : public Factory {
public:
    explicit FakeFactory(std::shared_ptr<Trace> trace) noexcept
        : trace_{std::move(trace)}
    {
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<GenerationInterface>>
    prepareGeneration(std::shared_ptr<TransitionGate> gate) noexcept override
    {
        ++prepareCalls;
        trace_->record(EventKind::Prepare, nextId);
        if (failNext) {
            failNext = false;
            return Domain::Result<std::shared_ptr<
                GenerationInterface>>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "fake prepare failed", true));
        }
        transitionGate = gate;
        auto generation = std::make_shared<FakeGeneration>(
            nextId++, std::move(gate), trace_);
        generation->drainOnShutdown = drainOnShutdownForNext;
        drainOnShutdownForNext = true;
        generation->drainOnGracefulShutdown =
            drainOnGracefulShutdownForNext;
        drainOnGracefulShutdownForNext = true;
        generation->drainDuringStart = drainDuringStartForNext;
        drainDuringStartForNext = false;
        generation->drainWhenOwnershipRechecked =
            drainWhenOwnershipRecheckedForNext;
        drainWhenOwnershipRecheckedForNext = false;
        generation->shutdownEdge = std::move(shutdownEdgeForNext);
        shutdownEdgeForNext = {};
        generation->gracefulShutdownEdge =
            std::move(gracefulShutdownEdgeForNext);
        gracefulShutdownEdgeForNext = {};
        generation->startEdge = std::move(startEdgeForNext);
        startEdgeForNext = {};
        generations.push_back(generation);
        return Domain::Result<std::shared_ptr<
            GenerationInterface>>::success(generation);
    }

    [[nodiscard]] std::shared_ptr<FakeGeneration> latest() const
    {
        return generations.back();
    }

    void releaseGeneration(const std::uint64_t generationId)
    {
        for (auto iterator = generations.begin();
             iterator != generations.end(); ++iterator) {
            if ((*iterator)->registrationId() == generationId) {
                generations.erase(iterator);
                return;
            }
        }
    }

    std::shared_ptr<Trace> trace_;
    std::vector<std::shared_ptr<FakeGeneration>> generations;
    std::shared_ptr<TransitionGate> transitionGate;
    std::uint64_t nextId{1U};
    std::size_t prepareCalls{};
    bool failNext{};
    bool drainOnShutdownForNext{true};
    bool drainOnGracefulShutdownForNext{true};
    bool drainDuringStartForNext{};
    bool drainWhenOwnershipRecheckedForNext{};
    std::function<void(std::uint64_t)> shutdownEdgeForNext;
    std::function<void(std::uint64_t)> gracefulShutdownEdgeForNext;
    std::function<void()> startEdgeForNext;
};

class FakeRegistrationHost final : public RegistrationHost {
public:
    explicit FakeRegistrationHost(std::shared_ptr<Trace> trace) noexcept
        : trace_{std::move(trace)}
    {
    }

    [[nodiscard]] Domain::Result<void> bindConnectionDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionGenerationDrainObserver>
            observer) noexcept override
    {
        connectionObserver_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> registerCompletionTarget(
        std::shared_ptr<GenerationInterface> generation) noexcept override
    {
        trace_->record(
            EventKind::RegisterCompletion, generation->registrationId());
        if (failCompletion) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "fake completion registration failed", true));
        }
        completionTargets.push_back(std::move(generation));
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> registerDeadlineTarget(
        std::shared_ptr<GenerationInterface> generation) noexcept override
    {
        trace_->record(
            EventKind::RegisterDeadline, generation->registrationId());
        if (failDeadline) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "fake deadline registration failed", true));
        }
        deadlineTargets.push_back(std::move(generation));
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool unregisterDeadlineTarget(
        const std::shared_ptr<GenerationInterface>& generation)
        noexcept override
    {
        trace_->record(
            EventKind::UnregisterDeadline, generation->registrationId());
        bool fail{};
        {
            std::unique_lock lock{unregisterMutex_};
            if (blockDeadlineUnregister_) {
                deadlineUnregisterEntered_ = true;
                unregisterChanged_.notify_all();
                unregisterChanged_.wait(
                    lock, [this] { return releaseDeadlineUnregister_; });
                blockDeadlineUnregister_ = false;
            }
            fail = failDeadlineUnregister;
        }
        if (fail) {
            return false;
        }
        return erase(deadlineTargets, generation);
    }

    [[nodiscard]] bool unregisterCompletionTarget(
        const std::shared_ptr<GenerationInterface>& generation)
        noexcept override
    {
        trace_->record(
            EventKind::UnregisterCompletion, generation->registrationId());
        if (failCompletionUnregister) {
            return false;
        }
        return erase(completionTargets, generation);
    }

    void blockAndFailNextDeadlineUnregister() noexcept
    {
        const std::scoped_lock lock{unregisterMutex_};
        blockDeadlineUnregister_ = true;
        deadlineUnregisterEntered_ = false;
        releaseDeadlineUnregister_ = false;
        failDeadlineUnregister = true;
    }

    void blockNextDeadlineUnregister() noexcept
    {
        const std::scoped_lock lock{unregisterMutex_};
        blockDeadlineUnregister_ = true;
        deadlineUnregisterEntered_ = false;
        releaseDeadlineUnregister_ = false;
        failDeadlineUnregister = false;
    }

    [[nodiscard]] bool waitForDeadlineUnregister() noexcept
    {
        std::unique_lock lock{unregisterMutex_};
        return unregisterChanged_.wait_for(
            lock,
            5s,
            [this] { return deadlineUnregisterEntered_; });
    }

    void releaseDeadlineUnregister() noexcept
    {
        const std::scoped_lock lock{unregisterMutex_};
        releaseDeadlineUnregister_ = true;
        unregisterChanged_.notify_all();
    }

    void signalConnectionZero(const std::uint64_t generationId) noexcept
    {
        if (const auto observer = connectionObserver_.lock();
            observer != nullptr) {
            observer->generationConnectionsMayHaveDrained(generationId);
        }
    }

    static bool erase(
        std::vector<std::shared_ptr<GenerationInterface>>& values,
        const std::shared_ptr<GenerationInterface>& searched) noexcept
    {
        for (auto iterator = values.begin(); iterator != values.end();
             ++iterator) {
            if (iterator->get() == searched.get()) {
                values.erase(iterator);
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<Trace> trace_;
    std::vector<std::shared_ptr<GenerationInterface>> completionTargets;
    std::vector<std::shared_ptr<GenerationInterface>> deadlineTargets;
    std::weak_ptr<Detail::IDashboardConnectionGenerationDrainObserver>
        connectionObserver_;
    bool failCompletion{};
    bool failDeadline{};
    bool failDeadlineUnregister{};
    bool failCompletionUnregister{};

private:
    std::mutex unregisterMutex_;
    std::condition_variable unregisterChanged_;
    bool blockDeadlineUnregister_{};
    bool deadlineUnregisterEntered_{};
    bool releaseDeadlineUnregister_{};
};

class FakeOverloadDrainSource final
    : public Detail::IDashboardListenerGenerationOverloadDrainSource {
public:
    [[nodiscard]] Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
            observer) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        observer_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    void signal(const std::uint64_t generationId) noexcept
    {
        std::shared_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            observer = observer_.lock();
        }
        if (observer != nullptr) {
            observer->overloadGenerationMayHaveDrained(generationId);
        }
    }

    void signalOwnerShutdown() noexcept
    {
        std::shared_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            observer = observer_.lock();
        }
        if (observer != nullptr) {
            observer->overloadOwnerBeganShutdown();
        }
    }

    void signalOwnerTerminal() noexcept
    {
        std::shared_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            observer = observer_.lock();
        }
        if (observer != nullptr) {
            observer->overloadOwnerBecameTerminal();
        }
    }

    void signalTerminal(const std::uint64_t generationId) noexcept
    {
        stageTerminal(generationId);
        drainTerminalNotifications();
    }

    void runTerminalCompletionBarrier(
        const std::uint64_t generationId) noexcept
    {
        std::shared_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            observer = observer_.lock();
        }
        if (observer == nullptr) {
            return;
        }

        observer->overloadGenerationCompletionPending(generationId);
        {
            std::unique_lock lock{mutex_};
            completionPendingDelivered_ = true;
            completionChanged_.notify_all();
            completionChanged_.wait(lock, [this]() noexcept {
                return releaseCompletionResult_;
            });
        }
        observer->overloadGenerationTerminalPending(generationId);
        observer->overloadGenerationCompletionSettled(generationId);
    }

    [[nodiscard]] bool waitForCompletionPending() noexcept
    {
        std::unique_lock lock{mutex_};
        return completionChanged_.wait_for(lock, 5s, [this]() noexcept {
            return completionPendingDelivered_;
        });
    }

    void releaseTerminalCompletionResult() noexcept
    {
        {
            const std::scoped_lock lock{mutex_};
            releaseCompletionResult_ = true;
        }
        completionChanged_.notify_all();
    }

    void stageTerminal(const std::uint64_t generationId) noexcept
    {
        const std::scoped_lock lock{mutex_};
        stagedTerminalId_.emplace(generationId);
    }

    void drainTerminalNotifications() noexcept
    {
        std::shared_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
            observer;
        std::optional<std::uint64_t> terminalId;
        {
            const std::scoped_lock lock{mutex_};
            observer = observer_.lock();
            terminalId = stagedTerminalId_;
            stagedTerminalId_.reset();
        }
        if (observer != nullptr && terminalId.has_value()) {
            observer->overloadGenerationBecameTerminal(*terminalId);
        }
    }

    [[nodiscard]] bool callbackSourceLockAvailable() noexcept
    {
        if (!mutex_.try_lock()) {
            return false;
        }
        mutex_.unlock();
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable completionChanged_;
    std::weak_ptr<Detail::IDashboardAdmissionOverloadDrainObserver>
        observer_;
    std::optional<std::uint64_t> stagedTerminalId_;
    bool completionPendingDelivered_{};
    bool releaseCompletionResult_{};
};

class CoordinatorDrainObserver final
    : public Detail::
          IDashboardListenerGenerationCoordinatorDrainObserver {
public:
    explicit CoordinatorDrainObserver(
        std::function<void()> callback = {})
        : callback_{std::move(callback)}
    {
    }

    void listenerGenerationsMayHaveDrained() noexcept override
    {
        calls.fetch_add(1U, std::memory_order_relaxed);
        if (callback_) {
            callback_();
        }
    }

    std::atomic_size_t calls{};

private:
    std::function<void()> callback_;
};

struct CoordinatorFixture final {
    CoordinatorFixture()
        : trace{std::make_shared<Trace>()},
          host{std::make_shared<FakeRegistrationHost>(trace)},
          overloadDrain{std::make_shared<FakeOverloadDrainSource>()},
          factory{std::make_shared<FakeFactory>(trace)},
          coordinator{take(Coordinator::create(
              host, overloadDrain, factory))}
    {
    }

    std::shared_ptr<Trace> trace;
    std::shared_ptr<FakeRegistrationHost> host;
    std::shared_ptr<FakeOverloadDrainSource> overloadDrain;
    std::shared_ptr<FakeFactory> factory;
    std::shared_ptr<Coordinator> coordinator;
};

void testInitialPublishAndPrepareFailurePreservesActive()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    const auto initial = fixture.coordinator->snapshot();
    require(initial.activeRegistrationId() == 1U,
            "initial listener was not published");
    require(!initial.retiringRegistrationId().has_value(),
            "initial publish unexpectedly retained a generation");
    require(
        fixture.trace->indexOf(EventKind::RegisterCompletion, 1U) <
            fixture.trace->indexOf(EventKind::RegisterDeadline, 1U),
        "completion registration did not precede deadline registration");
    require(
        fixture.trace->indexOf(EventKind::RegisterDeadline, 1U) <
            fixture.trace->indexOf(EventKind::Start, 1U),
        "listener started before exact routing registration");

    fixture.factory->failNext = true;
    const auto failed = fixture.coordinator->rebind();
    requireError(
        failed,
        Domain::ErrorCodes::Conflict,
        true,
        "prepare failure unexpectedly succeeded");
    require(fixture.coordinator->snapshot().activeRegistrationId() == 1U,
            "prepare failure replaced the active listener");
}

void testSuccessfulRebindOrderingConflictAndDrain()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    auto old = fixture.factory->latest();
    old->drainOnShutdown = false;

    takeVoid(fixture.coordinator->rebind());
    auto replacement = fixture.factory->latest();
    const auto rebound = fixture.coordinator->snapshot();
    require(rebound.activeRegistrationId() == 2U,
            "replacement listener was not published");
    require(rebound.retiringRegistrationId() == 1U,
            "old listener was not retained for exact drain");
    require(
        fixture.trace->indexOf(EventKind::Start, 2U) <
            fixture.trace->indexOf(EventKind::Retire, 1U),
        "old admission closed before new admission started");

    const auto prepareCalls = fixture.factory->prepareCalls;
    const auto conflict = fixture.coordinator->rebind();
    requireError(
        conflict,
        Domain::ErrorCodes::Conflict,
        true,
        "rebind during retirement unexpectedly succeeded");
    require(fixture.factory->prepareCalls == prepareCalls,
            "conflicting rebind prepared a third generation");

    std::weak_ptr<FakeGeneration> oldLifetime = old;
    fixture.factory->releaseGeneration(1U);
    old.reset();
    require(!oldLifetime.expired(),
            "retiring owner was released before exact drain");
    if (const auto pinned = oldLifetime.lock(); pinned != nullptr) {
        pinned->setDrained();
    }
    require(!fixture.coordinator->snapshot().retiringRegistrationId()
                 .has_value(),
            "drained old generation remained registered");
    require(
        fixture.trace->indexOf(EventKind::UnregisterDeadline, 1U) <
            fixture.trace->indexOf(EventKind::UnregisterCompletion, 1U),
        "deadline ownership was not retired before completion routing");
    require(replacement->started,
            "active replacement lost admission during old drain");
    require(oldLifetime.expired(),
            "drained old generation remained pinned after unregister");
}

void testRetirementAcceptCancellationPreservesConnectionGrace()
{
    RuntimeFixture fixture;
    auto accept = std::make_unique<FakeAcceptOwner>(
        RuntimeIdentity{43U, CompletionKey{0x4300U}});
    auto* const acceptView = accept.get();
    acceptView->consumeCallsRequiredForDrain = 4U;
    auto connections = std::make_shared<FakeConnectionControl>();
    connections->set(43U, 1U);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto gate = std::make_shared<TransitionGate>();
    auto generation = take(Generation::create(
        RuntimeIdentity{43U, CompletionKey{0x4300U}},
        std::move(accept),
        *fixture.scheduler,
        *fixture.runtime,
        connections,
        overload,
        gate));

    {
        auto transition = gate->enter();
        takeVoid(generation->startAdmission(transition));
        takeVoid(generation->beginRetirement(transition));
    }
    const auto exactDeadline = *generation->snapshot().retirementDeadline();
    for (std::size_t index{}; index < 4U; ++index) {
        generation->consume(
            Detail::DashboardIoCompletionPacket{
                0U, CompletionKey{0x4300U}, nullptr},
            ERROR_OPERATION_ABORTED);
        require(connections->shutdownCalls() == 0U,
                "expected retirement accept cancel shortened grace");
    }
    require(generation->snapshot().lifecycle() == Lifecycle::Retiring,
            "cancelled accepts changed retirement lifecycle");
    require(connections->connectionCountForGeneration(43U) == 1U,
            "cancelled accepts force-closed existing connection");
    require(overload->cancelledIds().empty(),
            "graceful accept drain cancelled overload work before deadline");

    generation->dispatchDeadline(exactDeadline);
    require(connections->shutdownCalls() == 1U,
            "retirement deadline did not end connection grace");
    require(generation->fullyDrained(),
            "deadline did not complete fully drained retirement owner");
}

void testStartToPublishDrainCannotPublishDeadGeneration()
{
    {
        CoordinatorFixture fixture;
        fixture.factory->drainDuringStartForNext = true;
        const auto result = fixture.coordinator->startInitial();
        requireError(
            result,
            Domain::ErrorCodes::TransportClosed,
            false,
            "drained initial listener unexpectedly published");
        require(!fixture.coordinator->snapshot().activeRegistrationId()
                     .has_value(),
                "initial start-to-publish failure left dead active owner");
        require(fixture.host->completionTargets.empty(),
                "dead initial owner retained completion registration");
    }
    {
        CoordinatorFixture fixture;
        takeVoid(fixture.coordinator->startInitial());
        auto original = fixture.factory->latest();
        fixture.factory->drainDuringStartForNext = true;
        const auto result = fixture.coordinator->rebind();
        requireError(
            result,
            Domain::ErrorCodes::TransportClosed,
            false,
            "drained replacement unexpectedly published");
        require(fixture.coordinator->snapshot().activeRegistrationId() == 1U,
                "dead replacement displaced healthy active listener");
        require(!original->retired,
                "dead replacement closed old admission before validation");
    }
}

void testRegistrationRollback()
{
    CoordinatorFixture fixture;
    fixture.host->failDeadline = true;
    const auto result = fixture.coordinator->startInitial();
    requireError(
        result,
        Domain::ErrorCodes::LimitExceeded,
        true,
        "deadline registration failure unexpectedly published");
    require(!fixture.coordinator->snapshot().activeRegistrationId()
                 .has_value(),
            "registration failure published a listener");
    require(fixture.host->completionTargets.empty(),
            "registration rollback retained completion target");
    require(
        fixture.trace->indexOf(EventKind::Shutdown, 1U) <
            fixture.trace->indexOf(EventKind::UnregisterCompletion, 1U),
        "registration rollback unregistered before owner drain");
}

void testPartialRegistrationRollbackRetainsUntilDrain()
{
    CoordinatorFixture fixture;
    fixture.host->failDeadline = true;
    fixture.factory->drainOnShutdownForNext = false;
    const auto result = fixture.coordinator->startInitial();
    requireError(
        result,
        Domain::ErrorCodes::LimitExceeded,
        true,
        "partial registration failure unexpectedly published");
    require(fixture.coordinator->snapshot().retiringRegistrationId() == 1U,
            "partial registration rollback dropped its native owner");
    require(fixture.host->completionTargets.size() == 1U,
            "partial rollback unregistered before accept drain");

    fixture.factory->latest()->setDrained();
    require(!fixture.coordinator->snapshot().retiringRegistrationId()
                 .has_value(),
            "partial rollback remained after exact accept drain");
    require(fixture.host->completionTargets.empty(),
            "partial rollback retained fixed routing after drain");
}

void testRollbackPublishesBeforeSynchronousDrainEdge()
{
    CoordinatorFixture fixture;
    fixture.host->failDeadline = true;
    fixture.factory->drainOnShutdownForNext = false;
    fixture.factory->drainWhenOwnershipRecheckedForNext = true;
    fixture.factory->shutdownEdgeForNext =
        [host = fixture.host](const std::uint64_t generationId) noexcept {
            host->signalConnectionZero(generationId);
        };

    const auto result = fixture.coordinator->startInitial();
    requireError(
        result,
        Domain::ErrorCodes::LimitExceeded,
        true,
        "synchronous rollback edge unexpectedly published");
    require(!fixture.coordinator->snapshot().retiringRegistrationId()
                 .has_value(),
            "rollback lost synchronous last-owner drain edge");
    require(fixture.host->completionTargets.empty(),
            "synchronous rollback edge retained fixed route");
    require(
        fixture.factory->latest()->ownershipRechecks.load(
            std::memory_order_relaxed) == 2U,
        "rollback did not re-drive exact ownership around publication");
}

void testUnregisterFailureBecomesRetainedFatal()
{
    const auto runCase = [](const bool failCompletion) {
        CoordinatorFixture fixture;
        takeVoid(fixture.coordinator->startInitial());
        auto generation = fixture.factory->latest();
        if (failCompletion) {
            fixture.host->failCompletionUnregister = true;
        } else {
            fixture.host->failDeadlineUnregister = true;
        }

        generation->setDrained();

        const auto snapshot = fixture.coordinator->snapshot();
        require(snapshot.fatal() && snapshot.shutdownRequested() &&
                    snapshot.hasFailure(),
                "unregister failure did not become retained fatal state");
        require(!snapshot.collectionInProgress() &&
                    !snapshot.preparationInProgress(),
                "unregister failure remained pending after collection");
        require(snapshot.activeRegistrationId() == 1U,
                "unregister failure lost the fully drained owner");
        const auto failure = fixture.coordinator->fullFailure();
        require(failure.has_value() &&
                    failure->code == Domain::ErrorCodes::IntegrityFailure &&
                    !failure->retryable,
                "unregister failure retained the wrong fatal diagnostic");
        require(failure->message.find(
                    failCompletion ? "completion" : "deadline") !=
                    std::string::npos,
                "unregister failure did not identify the failed route");

        require(fixture.host->completionTargets.size() == 1U,
                "unregister failure released host completion ownership");
        require(
            fixture.host->deadlineTargets.size() ==
                (failCompletion ? 0U : 1U),
            "unregister failure corrupted exact deadline ownership");
        require(
            fixture.trace->count(EventKind::UnregisterDeadline) == 1U &&
                fixture.trace->count(EventKind::UnregisterCompletion) ==
                    (failCompletion ? 1U : 0U),
            "fatal unregister failure spun a pending collection loop");

        const auto prepareCalls = fixture.factory->prepareCalls;
        const auto blocked = fixture.coordinator->rebind();
        requireError(
            blocked,
            Domain::ErrorCodes::IntegrityFailure,
            false,
            "fatal unregister failure allowed another preparation");
        require(fixture.factory->prepareCalls == prepareCalls,
                "fatal unregister failure reached the generation factory");
        require(blocked.error() == *failure,
                "preparation did not return the retained fatal error");
    };

    runCase(false);
    runCase(true);
}

void testCollectionBarrierFailureIsFatalWithoutOwnerLoss()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    auto old = fixture.factory->latest();
    old->drainOnShutdown = false;
    takeVoid(fixture.coordinator->rebind());
    auto replacement = fixture.factory->latest();
    fixture.host->blockAndFailNextDeadlineUnregister();

    std::thread collector{[old] { old->setDrained(); }};
    const bool unregisterEntered =
        fixture.host->waitForDeadlineUnregister();
    const auto prepareCalls = fixture.factory->prepareCalls;
    const auto duringCollection = fixture.coordinator->rebind();
    fixture.host->releaseDeadlineUnregister();
    collector.join();

    require(unregisterEntered,
            "drained-generation unregister did not enter the barrier");
    requireError(
        duringCollection,
        Domain::ErrorCodes::Conflict,
        true,
        "rebind crossed a live unregister collection");
    require(fixture.factory->prepareCalls == prepareCalls,
            "collection conflict prepared a third listener generation");

    const auto failed = fixture.coordinator->snapshot();
    require(failed.fatal() && failed.hasFailure() &&
                failed.shutdownRequested(),
            "unregister barrier failure did not become fatal");
    require(!failed.collectionInProgress(),
            "unregister barrier failure remained collection-pending");
    require(failed.activeRegistrationId() == 2U &&
                failed.retiringRegistrationId() == 1U,
            "unregister barrier failure lost active or retiring ownership");
    require(old->fatalCalled && replacement->fatalCalled,
            "fatal unregister failure did not fail closed both generations");

    const auto retainedFailure = fixture.coordinator->fullFailure();
    require(retainedFailure.has_value() &&
                retainedFailure->code ==
                    Domain::ErrorCodes::IntegrityFailure &&
                !retainedFailure->retryable,
            "barrier failure retained the wrong diagnostic");
    const auto afterFailure = fixture.coordinator->rebind();
    requireError(
        afterFailure,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "fatal barrier failure allowed a later rebind");
    require(afterFailure.error() == *retainedFailure,
            "later rebind did not return the retained barrier failure");
    require(fixture.factory->prepareCalls == prepareCalls,
            "fatal barrier failure reached preparation after collection");

    const auto* const oldView = old.get();
    const auto hostRetainsOld = [oldView](
        const std::vector<std::shared_ptr<GenerationInterface>>& values) {
        for (const auto& value : values) {
            if (value.get() == oldView) {
                return true;
            }
        }
        return false;
    };
    require(hostRetainsOld(fixture.host->deadlineTargets) &&
                hostRetainsOld(fixture.host->completionTargets),
            "failed exact unregister released the old host-held owner");
    require(fixture.trace->count(EventKind::UnregisterDeadline) == 1U,
            "failed exact unregister retried in a pending loop");

    std::weak_ptr<FakeGeneration> oldLifetime = old;
    fixture.factory->releaseGeneration(1U);
    old.reset();
    require(!oldLifetime.expired(),
            "fatal collection failure dropped the retained old owner");
}

void testGracefulAndFatalShutdown()
{
    {
        CoordinatorFixture fixture;
        takeVoid(fixture.coordinator->startInitial());
        auto active = fixture.factory->latest();
        fixture.coordinator->beginGracefulShutdown();
        require(active->gracefulShutdownCalled &&
                    !active->shutdownCalled,
                "graceful shutdown did not notify active generation");
        require(
            fixture.coordinator->snapshot().gracefulShutdownRequested(),
                "graceful shutdown was not latched");
        require(fixture.host->completionTargets.empty(),
                "graceful drain did not unregister completion ownership");
    }
    {
        CoordinatorFixture fixture;
        takeVoid(fixture.coordinator->startInitial());
        auto active = fixture.factory->latest();
        fixture.coordinator->fatal(ERROR_GEN_FAILURE);
        require(active->fatalCalled,
                "fatal shutdown did not notify active generation");
        require(fixture.coordinator->snapshot().fatal(),
                "fatal shutdown was not latched");
        require(fixture.host->deadlineTargets.empty(),
                "fatal drain did not unregister deadline ownership");
    }
}

void testCoordinatorGracefulShutdownIsIdempotentAndHardEscalates()
{
    CoordinatorFixture fixture;
    fixture.factory->drainOnGracefulShutdownForNext = false;
    fixture.factory->drainOnShutdownForNext = false;
    takeVoid(fixture.coordinator->startInitial());
    auto generation = fixture.factory->latest();

    fixture.coordinator->beginGracefulShutdown();
    fixture.coordinator->beginGracefulShutdown();
    auto graceful = fixture.coordinator->snapshot();
    require(graceful.gracefulShutdownRequested() &&
                !graceful.hardShutdownRequested() &&
                generation->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U &&
                generation->shutdownCalls.load(
                    std::memory_order_relaxed) == 0U,
            "coordinator graceful shutdown did not use one independent fanout latch");

    fixture.coordinator->beginShutdown();
    fixture.coordinator->beginShutdown();
    const auto escalated = fixture.coordinator->snapshot();
    require(escalated.gracefulShutdownRequested() &&
                escalated.hardShutdownRequested() &&
                generation->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U &&
                generation->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "coordinator hard escalation was merged with or repeated graceful fanout");

    generation->setDrained();
}

void testCoordinatorHardShutdownSuppressesAndSerializesGracefulFanout()
{
    {
        CoordinatorFixture fixture;
        fixture.factory->drainOnShutdownForNext = false;
        fixture.factory->drainOnGracefulShutdownForNext = false;
        takeVoid(fixture.coordinator->startInitial());
        auto generation = fixture.factory->latest();

        fixture.coordinator->beginShutdown();
        fixture.coordinator->beginGracefulShutdown();
        const auto snapshot = fixture.coordinator->snapshot();
        require(snapshot.hardShutdownRequested() &&
                    !snapshot.gracefulShutdownRequested() &&
                    generation->shutdownCalls.load(
                        std::memory_order_relaxed) == 1U &&
                    generation->gracefulShutdownCalls.load(
                        std::memory_order_relaxed) == 0U,
                "hard-first coordinator shutdown allowed a late graceful fanout");
        generation->setDrained();
    }

    CoordinatorFixture fixture;
    struct Barrier final {
        std::mutex mutex;
        std::condition_variable changed;
        bool gracefulEntered{};
        bool releaseGraceful{};
        bool hardStarted{};
    };
    auto barrier = std::make_shared<Barrier>();
    fixture.factory->drainOnGracefulShutdownForNext = false;
    fixture.factory->drainOnShutdownForNext = false;
    takeVoid(fixture.coordinator->startInitial());
    auto initial = fixture.factory->latest();

    fixture.factory->drainOnGracefulShutdownForNext = false;
    fixture.factory->drainOnShutdownForNext = false;
    fixture.factory->gracefulShutdownEdgeForNext = [barrier](std::uint64_t) {
        std::unique_lock lock{barrier->mutex};
        barrier->gracefulEntered = true;
        barrier->changed.notify_all();
        barrier->changed.wait(lock, [barrier] {
            return barrier->releaseGraceful;
        });
    };
    takeVoid(fixture.coordinator->rebind());
    auto replacement = fixture.factory->latest();

    std::thread graceful{[coordinator = fixture.coordinator] {
        coordinator->beginGracefulShutdown();
    }};
    bool gracefulEntered{};
    {
        std::unique_lock lock{barrier->mutex};
        gracefulEntered = barrier->changed.wait_for(
            lock, 5s, [barrier] { return barrier->gracefulEntered; });
        if (!gracefulEntered) {
            barrier->releaseGraceful = true;
        }
    }
    if (!gracefulEntered) {
        barrier->changed.notify_all();
        graceful.join();
        fail("concurrent graceful coordinator fanout did not enter");
    }

    std::thread hard{[coordinator = fixture.coordinator, barrier] {
        {
            const std::scoped_lock lock{barrier->mutex};
            barrier->hardStarted = true;
        }
        barrier->changed.notify_all();
        coordinator->beginShutdown();
    }};
    bool hardStarted{};
    bool hardCallbackBeforeGracefulRelease{};
    {
        std::unique_lock lock{barrier->mutex};
        hardStarted = barrier->changed.wait_for(
            lock, 5s, [barrier] { return barrier->hardStarted; });
        static_cast<void>(barrier->changed.wait_for(lock, 100ms));
        hardCallbackBeforeGracefulRelease =
            initial->shutdownCalls.load(std::memory_order_relaxed) != 0U ||
            replacement->shutdownCalls.load(
                std::memory_order_relaxed) != 0U;
        barrier->releaseGraceful = true;
    }
    barrier->changed.notify_all();
    graceful.join();
    hard.join();

    const auto snapshot = fixture.coordinator->snapshot();
    require(hardStarted && !hardCallbackBeforeGracefulRelease,
            "hard coordinator shutdown overtook active graceful fanout");
    require(snapshot.gracefulShutdownRequested() &&
                snapshot.hardShutdownRequested() &&
                initial->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U &&
                replacement->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U &&
                initial->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U &&
                replacement->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "concurrent coordinator shutdown violated graceful-to-hard ordering");
    require(fixture.trace->indexOf(
                EventKind::GracefulShutdown,
                initial->registrationId()) <
                fixture.trace->indexOf(
                    EventKind::Shutdown,
                    replacement->registrationId()) &&
                fixture.trace->indexOf(
                    EventKind::GracefulShutdown,
                    replacement->registrationId()) <
                fixture.trace->indexOf(
                    EventKind::Shutdown,
                    replacement->registrationId()),
            "hard fanout began before the complete graceful generation snapshot");
    fixture.coordinator->beginGracefulShutdown();
    require(initial->gracefulShutdownCalls.load(
                std::memory_order_relaxed) == 1U &&
                replacement->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "hard coordinator shutdown permitted a later graceful callback");
    initial->setDrained();
    replacement->setDrained();
}

void testGracefulCutoffSerializesAgainstRebindPublication()
{
    CoordinatorFixture fixture;
    fixture.factory->drainOnGracefulShutdownForNext = false;
    takeVoid(fixture.coordinator->startInitial());
    auto initial = fixture.factory->latest();

    struct Barrier final {
        std::mutex mutex;
        std::condition_variable changed;
        bool replacementStarted{};
        bool releaseReplacement{};
        bool gracefulStarted{};
        bool gracefulCompleted{};
    };
    auto barrier = std::make_shared<Barrier>();
    fixture.factory->drainOnGracefulShutdownForNext = false;
    fixture.factory->startEdgeForNext = [barrier] {
        std::unique_lock lock{barrier->mutex};
        barrier->replacementStarted = true;
        barrier->changed.notify_all();
        barrier->changed.wait(lock, [barrier] {
            return barrier->releaseReplacement;
        });
    };

    std::atomic_bool rebindSucceeded{};
    std::thread rebind{[&fixture, &rebindSucceeded] {
        rebindSucceeded.store(
            static_cast<bool>(fixture.coordinator->rebind()),
            std::memory_order_release);
    }};
    bool replacementStarted{};
    {
        std::unique_lock lock{barrier->mutex};
        replacementStarted = barrier->changed.wait_for(
            lock, 5s, [barrier] { return barrier->replacementStarted; });
        if (!replacementStarted) {
            barrier->releaseReplacement = true;
        }
    }
    if (!replacementStarted) {
        barrier->changed.notify_all();
        rebind.join();
        fail("replacement did not enter transition-gated start");
    }

    std::thread graceful{[&fixture, barrier] {
        {
            const std::scoped_lock lock{barrier->mutex};
            barrier->gracefulStarted = true;
        }
        barrier->changed.notify_all();
        fixture.coordinator->beginGracefulShutdown();
        {
            const std::scoped_lock lock{barrier->mutex};
            barrier->gracefulCompleted = true;
        }
        barrier->changed.notify_all();
    }};
    bool gracefulStarted{};
    bool gracefulCompletedBeforePublication{};
    {
        std::unique_lock lock{barrier->mutex};
        gracefulStarted = barrier->changed.wait_for(
            lock, 5s, [barrier] { return barrier->gracefulStarted; });
        if (gracefulStarted) {
            gracefulCompletedBeforePublication =
                barrier->changed.wait_for(lock, 100ms, [barrier] {
                    return barrier->gracefulCompleted;
                });
        }
        barrier->releaseReplacement = true;
    }
    barrier->changed.notify_all();
    rebind.join();
    graceful.join();

    require(gracefulStarted,
            "graceful shutdown race did not start");
    require(!gracefulCompletedBeforePublication,
            "graceful cutoff crossed a live rebind transition");
    auto replacement = fixture.factory->latest();
    const auto snapshot = fixture.coordinator->snapshot();
    require(rebindSucceeded.load(std::memory_order_acquire) &&
                snapshot.activeRegistrationId() == 2U &&
                snapshot.retiringRegistrationId() == 1U,
            "transition-gated graceful cutoff split rebind publication");
    require(initial->gracefulShutdownCalls.load(
                std::memory_order_relaxed) == 1U &&
                replacement->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "graceful cutoff did not fan out with a fresh guard per published generation");

    initial->setDrained();
    replacement->setDrained();
}

void testCoordinatorDrainObserverInitialZeroAndOneShotBinding()
{
    CoordinatorFixture fixture;
    std::weak_ptr<CoordinatorDrainObserver> expired;
    requireError(
        fixture.coordinator->bindShutdownDrainObserver(expired),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "coordinator accepted an expired process drain observer");

    auto reentered = std::make_shared<std::atomic_bool>(false);
    auto observer = std::make_shared<CoordinatorDrainObserver>(
        [coordinator = std::weak_ptr<Coordinator>{fixture.coordinator},
         reentered] {
            if (const auto pinned = coordinator.lock(); pinned != nullptr) {
                const auto snapshot = pinned->snapshot();
                reentered->store(
                    snapshot.shutdownRequested(),
                    std::memory_order_release);
                pinned->beginShutdown();
            }
        });
    takeVoid(fixture.coordinator->bindShutdownDrainObserver(observer));
    requireError(
        fixture.coordinator->bindShutdownDrainObserver(observer),
        Domain::ErrorCodes::Conflict,
        false,
        "coordinator accepted a second process drain observer");

    fixture.coordinator->beginGracefulShutdown();
    fixture.coordinator->beginGracefulShutdown();
    require(observer->calls.load(std::memory_order_relaxed) == 1U &&
                reentered->load(std::memory_order_acquire),
            "initial-zero coordinator drain did not notify once outside its mutex");
    require(fixture.coordinator->snapshot().hardShutdownRequested(),
            "reentrant drain observer could not request hard shutdown");

    auto lateObserver = std::make_shared<CoordinatorDrainObserver>();
    requireError(
        fixture.coordinator->bindShutdownDrainObserver(lateObserver),
        Domain::ErrorCodes::TransportClosed,
        false,
        "coordinator accepted process drain binding after shutdown cutoff");
}

void testCoordinatorDrainObserverWaitsForPendingGracefulFanout()
{
    CoordinatorFixture fixture;
    fixture.factory->drainOnGracefulShutdownForNext = false;
    takeVoid(fixture.coordinator->startInitial());
    auto initial = fixture.factory->latest();

    struct Barrier final {
        std::mutex mutex;
        std::condition_variable changed;
        bool gracefulEntered{};
        bool releaseGraceful{};
    };
    auto barrier = std::make_shared<Barrier>();
    fixture.factory->drainOnGracefulShutdownForNext = false;
    fixture.factory->gracefulShutdownEdgeForNext = [barrier](std::uint64_t) {
        std::unique_lock lock{barrier->mutex};
        barrier->gracefulEntered = true;
        barrier->changed.notify_all();
        barrier->changed.wait(lock, [barrier] {
            return barrier->releaseGraceful;
        });
    };
    takeVoid(fixture.coordinator->rebind());
    auto replacement = fixture.factory->latest();

    auto observer = std::make_shared<CoordinatorDrainObserver>();
    takeVoid(fixture.coordinator->bindShutdownDrainObserver(observer));
    std::thread graceful{[coordinator = fixture.coordinator] {
        coordinator->beginGracefulShutdown();
    }};

    bool gracefulEntered{};
    {
        std::unique_lock lock{barrier->mutex};
        gracefulEntered = barrier->changed.wait_for(
            lock, 5s, [barrier] { return barrier->gracefulEntered; });
        if (!gracefulEntered) {
            barrier->releaseGraceful = true;
        }
    }
    if (!gracefulEntered) {
        barrier->changed.notify_all();
        graceful.join();
        fail("graceful observer barrier did not enter");
    }

    replacement->setDrained();
    initial->setDrained();
    const auto callsBeforeFanoutCompleted =
        observer->calls.load(std::memory_order_relaxed);
    {
        const std::scoped_lock lock{barrier->mutex};
        barrier->releaseGraceful = true;
    }
    barrier->changed.notify_all();
    graceful.join();

    require(callsBeforeFanoutCompleted == 0U &&
                observer->calls.load(std::memory_order_relaxed) == 1U,
            "coordinator drain edge crossed a pending graceful callback snapshot");
    require(initial->gracefulShutdownCalls.load(
                std::memory_order_relaxed) == 1U &&
                replacement->gracefulShutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "drained listener snapshot did not complete graceful fanout");
    fixture.coordinator->beginShutdown();
    require(observer->calls.load(std::memory_order_relaxed) == 1U,
            "late hard shutdown repeated the coordinator drain edge");
}

void testCoordinatorDrainObserverWaitsForCollectionAndFiresOnce()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    fixture.host->blockNextDeadlineUnregister();

    auto callbackReentered = std::make_shared<std::atomic_bool>(false);
    auto observer = std::make_shared<CoordinatorDrainObserver>(
        [coordinator = std::weak_ptr<Coordinator>{fixture.coordinator},
         callbackReentered] {
            if (const auto pinned = coordinator.lock(); pinned != nullptr) {
                const auto snapshot = pinned->snapshot();
                callbackReentered->store(
                    !snapshot.collectionInProgress() &&
                        !snapshot.activeRegistrationId().has_value() &&
                        !snapshot.retiringRegistrationId().has_value(),
                    std::memory_order_release);
            }
        });
    takeVoid(fixture.coordinator->bindShutdownDrainObserver(observer));

    std::thread shutdown{[coordinator = fixture.coordinator] {
        coordinator->beginGracefulShutdown();
    }};
    const auto unregisterEntered =
        fixture.host->waitForDeadlineUnregister();
    const auto callsDuringCollection =
        observer->calls.load(std::memory_order_relaxed);
    fixture.host->releaseDeadlineUnregister();
    shutdown.join();

    require(unregisterEntered,
            "graceful generation drain did not enter unregister collection");
    require(callsDuringCollection == 0U,
            "coordinator process drain observer fired during collection");
    fixture.coordinator->beginGracefulShutdown();
    fixture.coordinator->beginShutdown();
    require(observer->calls.load(std::memory_order_relaxed) == 1U &&
                callbackReentered->load(std::memory_order_acquire),
            "coordinator process drain observer was early, repeated, or invoked under its mutex");
}

void testConnectionZeroObserverCompletesCollection()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    auto generation = fixture.factory->latest();
    generation->drainWhenOwnershipRechecked = true;
    fixture.host->signalConnectionZero(1U);
    require(!fixture.coordinator->snapshot().activeRegistrationId()
                 .has_value(),
            "registry generation-zero edge did not collect owner");
}

void testOverloadDrainEdgeCompletesCollectionWithoutPolling()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    auto generation = fixture.factory->latest();
    generation->drainWhenOwnershipRechecked = true;
    require(fixture.coordinator->snapshot().activeRegistrationId() == 1U,
            "generation collected before overload drain edge");

    fixture.overloadDrain->signal(99U);
    require(fixture.coordinator->snapshot().activeRegistrationId() == 1U,
            "stale overload edge collected active generation");
    require(generation->ownershipRechecks.load(std::memory_order_relaxed) ==
                0U,
            "stale overload edge rechecked wrong generation");

    fixture.overloadDrain->signal(1U);
    require(!fixture.coordinator->snapshot().activeRegistrationId()
                 .has_value(),
            "exact overload drain edge did not collect generation");
    require(generation->ownershipRechecks.load(std::memory_order_relaxed) ==
                1U,
            "exact overload edge did not run one ownership recheck");
}

void testOverloadTerminalEdgeFailsClosedExactlyOnce()
{
    const auto runCase = [](const bool retiringOrigin) {
        CoordinatorFixture fixture;
        auto callbackSourceLockAvailable =
            std::make_shared<std::atomic_bool>(true);
        auto shutdownEdges = std::make_shared<std::atomic_size_t>(0U);
        const auto shutdownEdge =
            [source = fixture.overloadDrain,
             callbackSourceLockAvailable,
             shutdownEdges](const std::uint64_t) noexcept {
                shutdownEdges->fetch_add(1U, std::memory_order_relaxed);
                if (!source->callbackSourceLockAvailable()) {
                    callbackSourceLockAvailable->store(
                        false, std::memory_order_release);
                }
            };

        fixture.factory->drainOnShutdownForNext = false;
        fixture.factory->shutdownEdgeForNext = shutdownEdge;
        takeVoid(fixture.coordinator->startInitial());
        auto initial = fixture.factory->latest();
        std::shared_ptr<FakeGeneration> replacement;
        if (retiringOrigin) {
            fixture.factory->drainOnShutdownForNext = false;
            fixture.factory->shutdownEdgeForNext = shutdownEdge;
            takeVoid(fixture.coordinator->rebind());
            replacement = fixture.factory->latest();
        }

        const auto originId = initial->registrationId();
        fixture.overloadDrain->signalTerminal(99U);
        require(!fixture.coordinator->snapshot().fatal() &&
                    initial->shutdownCalls.load(
                        std::memory_order_relaxed) == 0U,
                "stale overload terminal edge shut down a generation");

        {
            auto transition = fixture.factory->transitionGate->enter();
            fixture.overloadDrain->stageTerminal(originId);
            require(!fixture.coordinator->snapshot().fatal() &&
                        initial->shutdownCalls.load(
                            std::memory_order_relaxed) == 0U &&
                        (!retiringOrigin ||
                         replacement->shutdownCalls.load(
                             std::memory_order_relaxed) == 0U),
                    "staged terminal edge re-entered a live transition");
        }
        fixture.overloadDrain->drainTerminalNotifications();
        const auto terminal = fixture.coordinator->snapshot();
        require(terminal.fatal() && terminal.shutdownRequested(),
                "exact overload terminal edge was not coordinated fatal");
        require(initial->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
                "terminal origin did not begin shutdown exactly once");
        if (retiringOrigin) {
            require(replacement != nullptr &&
                        replacement->shutdownCalls.load(
                            std::memory_order_relaxed) == 1U,
                    "retiring terminal edge did not fail closed the active generation");
            require(terminal.activeRegistrationId() == 2U &&
                        terminal.retiringRegistrationId() == 1U,
                    "terminal shutdown lost undrained generation ownership");
        } else {
            require(terminal.activeRegistrationId() == 1U &&
                        !terminal.retiringRegistrationId().has_value(),
                    "active terminal shutdown lost undrained owner identity");
        }
        require(callbackSourceLockAvailable->load(
                    std::memory_order_acquire) &&
                    shutdownEdges->load(std::memory_order_relaxed) ==
                        (retiringOrigin ? 2U : 1U),
                "generation shutdown ran under the overload callback source lock");

        const auto prepareCalls = fixture.factory->prepareCalls;
        const auto rejected = fixture.coordinator->rebind();
        require(!rejected && !rejected.error().retryable,
                "fatal overload terminal state allowed a retryable rebind");
        require(fixture.factory->prepareCalls == prepareCalls,
                "terminal overload state reached generation preparation");

        fixture.overloadDrain->signalTerminal(originId);
        require(initial->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U &&
                    (!retiringOrigin ||
                     replacement->shutdownCalls.load(
                         std::memory_order_relaxed) == 1U) &&
                    shutdownEdges->load(std::memory_order_relaxed) ==
                        (retiringOrigin ? 2U : 1U),
                "repeated overload terminal edge repeated coordinated shutdown");
    };

    runCase(false);
    runCase(true);
}

void testOverloadOwnerShutdownCoordinatesNormalOnce()
{
    const auto runCase = [](const bool coordinatorFirst) {
        CoordinatorFixture fixture;
        fixture.factory->drainOnShutdownForNext = false;
        takeVoid(fixture.coordinator->startInitial());
        auto generation = fixture.factory->latest();

        if (coordinatorFirst) {
            fixture.coordinator->beginGracefulShutdown();
            fixture.overloadDrain->signalOwnerShutdown();
        } else {
            fixture.overloadDrain->signalOwnerShutdown();
            fixture.coordinator->beginGracefulShutdown();
        }

        const auto snapshot = fixture.coordinator->snapshot();
        require(snapshot.shutdownRequested() &&
                    snapshot.gracefulShutdownRequested() &&
                    !snapshot.hardShutdownRequested() &&
                    !snapshot.fatal() &&
                    generation->gracefulShutdownCalls.load(
                        std::memory_order_relaxed) == 1U,
                "normal overload/coordinator shutdown ordering duplicated fanout or became fatal");
        const auto prepareCalls = fixture.factory->prepareCalls;
        const auto rejected = fixture.coordinator->rebind();
        require(!rejected &&
                    rejected.error().code ==
                        Domain::ErrorCodes::TransportClosed &&
                    fixture.factory->prepareCalls == prepareCalls,
                "normal overload shutdown left listener rebind open");
    };

    runCase(false);
    runCase(true);
}

void testEmptyOverloadOwnerTerminalFailsClosed()
{
    CoordinatorFixture fixture;
    fixture.factory->drainOnShutdownForNext = false;
    takeVoid(fixture.coordinator->startInitial());
    auto initial = fixture.factory->latest();
    fixture.factory->drainOnShutdownForNext = false;
    takeVoid(fixture.coordinator->rebind());
    auto replacement = fixture.factory->latest();

    fixture.overloadDrain->signalOwnerTerminal();
    const auto terminal = fixture.coordinator->snapshot();
    require(terminal.fatal() && terminal.shutdownRequested() &&
                initial->shutdownCalls.load(std::memory_order_relaxed) == 1U &&
                replacement->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "generation-independent overload terminal edge did not fail closed both listener owners");
    const auto prepareCalls = fixture.factory->prepareCalls;
    const auto rejected = fixture.coordinator->rebind();
    require(!rejected && !rejected.error().retryable &&
                fixture.factory->prepareCalls == prepareCalls,
            "generation-independent overload terminal edge left rebind open");

    fixture.overloadDrain->signalOwnerTerminal();
    require(initial->shutdownCalls.load(std::memory_order_relaxed) == 1U &&
                replacement->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "repeated generation-independent terminal edge duplicated shutdown fanout");
}

void testCompletionPendingHoldPreventsTerminalCollectionRace()
{
    CoordinatorFixture fixture;
    takeVoid(fixture.coordinator->startInitial());
    auto generation = fixture.factory->latest();

    // Model the resume-token return exposing zero accept ownership before
    // the overload owner knows whether resume succeeded. Do not emit the
    // generation drain edge until the provisional hold is already latched.
    generation->drained.store(true, std::memory_order_release);
    std::thread completionResult{
        [source = fixture.overloadDrain]() noexcept {
            source->runTerminalCompletionBarrier(1U);
        }};
    const auto pendingDelivered =
        fixture.overloadDrain->waitForCompletionPending();
    if (!pendingDelivered) {
        fixture.overloadDrain->releaseTerminalCompletionResult();
        completionResult.join();
        fail("overload completion-pending barrier was not reached");
    }

    // A registry zero edge races the still-unresolved resume result. It may
    // recheck exact ownership but must not move the active entry into router
    // unregister while the provisional hold exists.
    std::thread collector{[host = fixture.host]() noexcept {
        host->signalConnectionZero(1U);
    }};
    collector.join();
    const auto pendingSnapshot = fixture.coordinator->snapshot();
    const auto pendingPrepareCalls = fixture.factory->prepareCalls;
    const auto pendingRebind = fixture.coordinator->rebind();
    const auto pendingCompletionTargets =
        fixture.host->completionTargets.size();
    const auto pendingDeadlineTargets =
        fixture.host->deadlineTargets.size();
    const auto pendingUnregisterDeadline =
        fixture.trace->count(EventKind::UnregisterDeadline);
    const auto pendingUnregisterCompletion =
        fixture.trace->count(EventKind::UnregisterCompletion);

    // Resume now fails: terminal-pending must be latched before the
    // provisional completion hold is settled.
    fixture.overloadDrain->releaseTerminalCompletionResult();
    completionResult.join();

    require(pendingSnapshot.activeRegistrationId() == 1U &&
                !pendingSnapshot.fatal(),
            "completion-pending collection lost or failed the active owner");
    require(pendingCompletionTargets == 1U &&
                pendingDeadlineTargets == 1U &&
                pendingUnregisterDeadline == 0U &&
                pendingUnregisterCompletion == 0U,
            "completion-pending hold allowed exact router unregister");
    require(generation->ownershipRechecks.load(
                std::memory_order_relaxed) == 1U,
            "zero-connection race did not perform one ownership recheck");
    require(!pendingRebind && pendingRebind.error().retryable &&
                pendingRebind.error().code == Domain::ErrorCodes::Conflict &&
                fixture.factory->prepareCalls == pendingPrepareCalls,
            "completion-pending hold allowed listener preparation");

    // Completion is settled, but the promoted terminal-pending hold must
    // preserve the exact same owner until became-terminal is pumped.
    fixture.host->signalConnectionZero(1U);
    const auto terminalPendingSnapshot = fixture.coordinator->snapshot();
    const auto terminalPendingRebind = fixture.coordinator->rebind();
    require(terminalPendingSnapshot.activeRegistrationId() == 1U &&
                !terminalPendingSnapshot.fatal(),
            "terminal-pending hold did not retain the exact active owner");
    require(fixture.trace->count(EventKind::UnregisterDeadline) == 0U &&
                fixture.trace->count(EventKind::UnregisterCompletion) == 0U,
            "completion settlement opened a terminal collection gap");
    require(generation->ownershipRechecks.load(
                std::memory_order_relaxed) == 2U,
            "terminal-pending collection phase skipped exact recheck");
    require(!terminalPendingRebind &&
                !terminalPendingRebind.error().retryable &&
                fixture.factory->prepareCalls == pendingPrepareCalls,
            "terminal-pending hold allowed listener preparation");

    fixture.overloadDrain->stageTerminal(1U);
    fixture.overloadDrain->drainTerminalNotifications();
    const auto terminal = fixture.coordinator->snapshot();
    require(terminal.fatal() && terminal.shutdownRequested() &&
                generation->shutdownCalls.load(
                    std::memory_order_relaxed) == 1U,
            "terminal pump did not begin coordinated fatal shutdown");
    require(!terminal.activeRegistrationId().has_value() &&
                fixture.host->completionTargets.empty() &&
                fixture.host->deadlineTargets.empty(),
            "terminal pump failed to release the now-latched drained owner");
    require(fixture.trace->count(EventKind::UnregisterDeadline) == 1U &&
                fixture.trace->count(EventKind::UnregisterCompletion) == 1U,
            "terminal pump did not unregister exact routes once");

    const auto failure = fixture.coordinator->fullFailure();
    const auto rejected = fixture.coordinator->rebind();
    require(failure.has_value() &&
                failure->code == Domain::ErrorCodes::IntegrityFailure &&
                !rejected && rejected.error() == *failure,
            "latched terminal state did not reject future rebind exactly");
}

constexpr CompletionKey IntegrationListenerKey{0x7100U};
constexpr CompletionKey IntegrationDeadlineKey{0x7200U};
constexpr std::uint64_t IntegrationGenerationId = 73U;

class IntegrationWinsockApi final : public Detail::IDashboardWinsockApi {
public:
    [[nodiscard]] int startup(WORD, WSADATA& data) noexcept override
    {
        data.wVersion = MAKEWORD(2, 2);
        return 0;
    }

    [[nodiscard]] int cleanup() noexcept override { return 0; }

    [[nodiscard]] SOCKET createSocket(int, int, int, DWORD) noexcept override
    {
        return nextSocket_++;
    }

    [[nodiscard]] int closeSocket(SOCKET) noexcept override { return 0; }
    [[nodiscard]] int lastError() noexcept override { return WSAENOBUFS; }

private:
    SOCKET nextSocket_{static_cast<SOCKET>(0x7300U)};
};

class IntegrationListenerApi final
    : public Detail::IDashboardListeningSocketApi {
public:
    explicit IntegrationListenerApi(
        const Detail::DashboardLoopbackEndpoint& endpoint) noexcept
        : addressLength_{endpoint.nativeAddressLength()}
    {
        std::memcpy(
            std::addressof(address_),
            endpoint.nativeAddress(),
            static_cast<std::size_t>(addressLength_));
    }

    [[nodiscard]] int setSocketOption(
        SOCKET,
        int,
        int,
        const char*,
        int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int bindSocket(SOCKET, const sockaddr*, int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int getSocketName(
        SOCKET,
        sockaddr* const address,
        int& addressLength) noexcept override
    {
        if (address == nullptr || addressLength < addressLength_) {
            return SOCKET_ERROR;
        }
        std::memcpy(
            address,
            std::addressof(address_),
            static_cast<std::size_t>(addressLength_));
        addressLength = addressLength_;
        return 0;
    }

    [[nodiscard]] int listenSocket(SOCKET, int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override { return WSAEINVAL; }

private:
    sockaddr_storage address_{};
    int addressLength_{};
};

struct IntegrationExtensionState final {
    std::uint16_t listenerPort{};
    std::array<OVERLAPPED*, Detail::DashboardAcceptSlotSet::SlotCount>
        operations{};
    std::size_t operationCount{};
    std::size_t issueCalls{};
    std::size_t failIssueCall{};
    int lastError{WSA_IO_PENDING};

    void remember(OVERLAPPED* const operation) noexcept
    {
        for (std::size_t index{}; index < operationCount; ++index) {
            if (operations[index] == operation) {
                return;
            }
        }
        if (operationCount < operations.size()) {
            operations[operationCount++] = operation;
        }
    }
};

IntegrationExtensionState* activeIntegrationExtensionState{};

class IntegrationExtensionScope final {
public:
    explicit IntegrationExtensionScope(
        IntegrationExtensionState& state) noexcept
        : state_{std::addressof(state)}
    {
        activeIntegrationExtensionState = state_;
    }

    ~IntegrationExtensionScope() noexcept
    {
        if (activeIntegrationExtensionState == state_) {
            activeIntegrationExtensionState = nullptr;
        }
    }

    IntegrationExtensionScope(const IntegrationExtensionScope&) = delete;
    IntegrationExtensionScope& operator=(
        const IntegrationExtensionScope&) = delete;

private:
    IntegrationExtensionState* state_{};
};

BOOL PASCAL integrationAcceptEx(
    SOCKET,
    SOCKET,
    PVOID,
    DWORD,
    DWORD,
    DWORD,
    LPDWORD const received,
    LPOVERLAPPED const operation)
{
    if (activeIntegrationExtensionState == nullptr) {
        return FALSE;
    }
    auto& state = *activeIntegrationExtensionState;
    ++state.issueCalls;
    state.remember(operation);
    if (received != nullptr) {
        *received = 0U;
    }
    state.lastError = state.issueCalls == state.failIssueCall
        ? WSAENOBUFS
        : WSA_IO_PENDING;
    WSASetLastError(state.lastError);
    return FALSE;
}

void populateIntegrationAddress(
    void* const destination,
    const std::uint16_t port) noexcept
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::memcpy(destination, std::addressof(address), sizeof(address));
}

VOID PASCAL integrationGetAcceptExSockaddrs(
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    DWORD,
    LPSOCKADDR* const localAddress,
    LPINT const localLength,
    LPSOCKADDR* const remoteAddress,
    LPINT const remoteLength)
{
    auto* const bytes = static_cast<std::byte*>(outputBuffer);
    auto* const local = reinterpret_cast<sockaddr*>(bytes + 16U);
    auto* const remote = reinterpret_cast<sockaddr*>(
        bytes + receiveDataLength + localAddressLength + 16U);
    populateIntegrationAddress(
        local, activeIntegrationExtensionState->listenerPort);
    populateIntegrationAddress(remote, 49123U);
    *localAddress = local;
    *remoteAddress = remote;
    *localLength = static_cast<int>(sizeof(sockaddr_in));
    *remoteLength = static_cast<int>(sizeof(sockaddr_in));
}

[[nodiscard]] bool sameIntegrationGuid(
    const GUID& left,
    const GUID& right) noexcept
{
    return std::memcmp(
               std::addressof(left),
               std::addressof(right),
               sizeof(GUID)) == 0;
}

class IntegrationExtensionApi final
    : public Detail::IDashboardWinsockExtensionApi {
public:
    [[nodiscard]] int ioctl(
        SOCKET,
        DWORD,
        const void* const inputBuffer,
        DWORD,
        void* const outputBuffer,
        const DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept override
    {
        const auto& identifier = *static_cast<const GUID*>(inputBuffer);
        if (sameIntegrationGuid(identifier, WSAID_ACCEPTEX) &&
            outputBufferLength == sizeof(LPFN_ACCEPTEX)) {
            const LPFN_ACCEPTEX function = &integrationAcceptEx;
            std::memcpy(outputBuffer, std::addressof(function), sizeof(function));
            bytesReturned = static_cast<DWORD>(sizeof(function));
            return 0;
        }
        if (sameIntegrationGuid(identifier, WSAID_GETACCEPTEXSOCKADDRS) &&
            outputBufferLength == sizeof(LPFN_GETACCEPTEXSOCKADDRS)) {
            const LPFN_GETACCEPTEXSOCKADDRS function =
                &integrationGetAcceptExSockaddrs;
            std::memcpy(outputBuffer, std::addressof(function), sizeof(function));
            bytesReturned = static_cast<DWORD>(sizeof(function));
            return 0;
        }
        return SOCKET_ERROR;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        return activeIntegrationExtensionState == nullptr
            ? WSAEINVAL
            : activeIntegrationExtensionState->lastError;
    }
};

class IntegrationAcceptApi final : public Detail::IDashboardAcceptSlotApi {
public:
    [[nodiscard]] int updateAcceptContext(SOCKET, SOCKET) noexcept override
    {
        return 0;
    }

    [[nodiscard]] BOOL cancelAccept(SOCKET, OVERLAPPED*) noexcept override
    {
        ++cancelCalls;
        return cancelCalls == failCancelCall ||
                cancelCalls == secondFailCancelCall
            ? FALSE
            : TRUE;
    }

    [[nodiscard]] int lastSocketError() noexcept override
    {
        return WSAECONNRESET;
    }

    [[nodiscard]] DWORD lastSystemError() noexcept override
    {
        return cancelError;
    }

    std::size_t cancelCalls{};
    std::size_t failCancelCall{};
    std::size_t secondFailCancelCall{};
    DWORD cancelError{ERROR_ACCESS_DENIED};
};

class IntegrationCompletionPortApi final
    : public Detail::IDashboardIoCompletionPortApi {
public:
    struct Completion final {
        DWORD bytes{};
        ULONG_PTR key{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE file,
        const HANDLE existing,
        ULONG_PTR,
        DWORD) noexcept override
    {
        return file == INVALID_HANDLE_VALUE ? portHandle() : existing;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        HANDLE,
        const DWORD bytes,
        const ULONG_PTR key,
        OVERLAPPED* const operation) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (closed_ || queueCount_ == queue_.size()) {
            threadError_ = ERROR_NOT_ENOUGH_MEMORY;
            return FALSE;
        }
        const auto tail = (queueHead_ + queueCount_) % queue_.size();
        queue_[tail] = Completion{bytes, key, operation};
        ++queueCount_;
        changed_.notify_one();
        return TRUE;
    }

    [[nodiscard]] BOOL getQueuedCompletionStatus(
        HANDLE,
        DWORD& bytes,
        ULONG_PTR& key,
        OVERLAPPED*& operation,
        const DWORD timeoutMilliseconds) noexcept override
    {
        std::unique_lock lock{mutex_};
        const auto ready = [this]() noexcept {
            return queueCount_ != 0U || closed_;
        };
        if (!ready()) {
            static_cast<void>(changed_.wait_for(
                lock,
                std::chrono::milliseconds{timeoutMilliseconds},
                ready));
        }
        if (queueCount_ == 0U) {
            bytes = 0U;
            key = 0U;
            operation = nullptr;
            threadError_ = closed_ ? ERROR_INVALID_HANDLE : WAIT_TIMEOUT;
            return FALSE;
        }
        const auto completion = queue_[queueHead_];
        queueHead_ = (queueHead_ + 1U) % queue_.size();
        --queueCount_;
        bytes = completion.bytes;
        key = completion.key;
        operation = completion.operation;
        threadError_ = ERROR_SUCCESS;
        return TRUE;
    }

    [[nodiscard]] BOOL closeHandle(HANDLE) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        closed_ = true;
        changed_.notify_all();
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override
    {
        return threadError_;
    }

private:
    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(0x7400U));
    }

    inline static thread_local DWORD threadError_{ERROR_SUCCESS};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Completion, 32U> queue_{};
    std::size_t queueHead_{};
    std::size_t queueCount_{};
    bool closed_{};
};

class IntegrationApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange>
    prepare(
        Dashboard::DashboardHttpRequest,
        bool,
        Domain::OperationContext) noexcept override
    {
        return Domain::Result<Dashboard::DashboardPreparedExchange>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "integration application must not be invoked"));
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "integration application must not be invoked"));
    }
};

class IntegrationConnectionTarget final
    : public Detail::IDashboardConnectionDispatchTarget {
public:
    IntegrationConnectionTarget(
        const std::uint64_t generationId,
        const RuntimeIdentity identity) noexcept
        : generationId_{generationId}, identity_{identity}
    {
    }

    IntegrationConnectionTarget(
        const std::uint64_t generationId,
        const RuntimeIdentity identity,
        Detail::DashboardAdmissionController::Lease admissionLease,
        Detail::DashboardAcceptedConnection acceptedConnection) noexcept
        : generationId_{generationId},
          identity_{identity},
          admissionLease_{std::in_place, std::move(admissionLease)},
          acceptedConnection_{
              std::in_place, std::move(acceptedConnection)}
    {
    }

    ~IntegrationConnectionTarget() noexcept override
    {
        releaseOwnedConnection();
    }

    [[nodiscard]] CompletionKey completionKey() const noexcept override
    {
        return identity_.completionKey;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return identity_.registrationId;
    }

    [[nodiscard]] std::uint64_t generationId() const noexcept override
    {
        return generationId_;
    }

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionDrainObserver> observer)
        noexcept override
    {
        if (observer.expired()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "integration target requires a live drain observer"));
        }
        const std::scoped_lock lock{mutex_};
        if (observerBound_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "integration target drain observer is one-shot"));
        }
        observerBound_ = true;
        drainObserver_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        started_ = true;
        return Domain::Result<void>::success();
    }

    void dispatchIocp(DWORD, OVERLAPPED*, DWORD) noexcept override {}

    void dispatchDeadline(Deadline) noexcept override {}

    void beginShutdown() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (!shutdownRequested_) {
            shutdownRequested_ = true;
            ++shutdownCalls_;
        }
    }

    [[nodiscard]] bool isDrained() const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return drained_;
    }

    [[nodiscard]] Detail::DashboardConnectionStateSnapshot snapshot()
        const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return {
            drained_
                ? Detail::DashboardConnectionLifecycleState::Drained
                : (shutdownRequested_
                       ? Detail::DashboardConnectionLifecycleState::Closing
                       : Detail::DashboardConnectionLifecycleState::Created),
            identity_.registrationId,
            generationId_,
            identity_.completionKey,
            !drained_,
            false,
            false,
            shutdownRequested_,
            false};
    }

    void completeExactNativeReap() noexcept
    {
        std::shared_ptr<Detail::IDashboardConnectionDrainObserver> observer;
        {
            const std::scoped_lock lock{mutex_};
            if (drained_) {
                return;
            }
            acceptedConnection_.reset();
            if (admissionLease_.has_value()) {
                admissionLease_->release();
                admissionLease_.reset();
            }
            drained_ = true;
            if (!drainNotificationSent_) {
                drainNotificationSent_ = true;
                observer = drainObserver_.lock();
            }
        }
        if (observer != nullptr) {
            observer->connectionMayHaveDrained(
                identity_.completionKey,
                identity_.registrationId,
                generationId_);
        }
    }

    [[nodiscard]] bool started() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return started_;
    }

    [[nodiscard]] bool shutdownRequested() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return shutdownRequested_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return shutdownCalls_;
    }

private:
    void releaseOwnedConnection() noexcept
    {
        const std::scoped_lock lock{mutex_};
        acceptedConnection_.reset();
        if (admissionLease_.has_value()) {
            admissionLease_->release();
            admissionLease_.reset();
        }
    }

    const std::uint64_t generationId_{};
    const RuntimeIdentity identity_;
    mutable std::mutex mutex_;
    std::optional<Detail::DashboardAdmissionController::Lease>
        admissionLease_;
    std::optional<Detail::DashboardAcceptedConnection> acceptedConnection_;
    std::weak_ptr<Detail::IDashboardConnectionDrainObserver> drainObserver_;
    bool observerBound_{};
    bool started_{};
    bool shutdownRequested_{};
    bool drained_{};
    bool drainNotificationSent_{};
    std::size_t shutdownCalls_{};
};

class IntegrationOwnerFactory final
    : public Detail::IDashboardAcceptedConnectionOwnerFactory {
public:
    explicit IntegrationOwnerFactory(const void* const policyIdentity)
        noexcept
        : policyIdentity_{policyIdentity}
    {
    }

    [[nodiscard]] const void* applicationPolicyIdentity()
        const noexcept override
    {
        return policyIdentity_;
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<
        Detail::IDashboardConnectionDispatchTarget>>
    createOwner(
        const std::uint64_t generationId,
        const RuntimeIdentity identity,
        Domain::MonotonicTimePoint,
        Detail::DashboardAdmissionController::Lease admissionLease,
        Detail::DashboardAcceptedConnection acceptedConnection)
        noexcept override
    {
        auto target = std::make_shared<IntegrationConnectionTarget>(
            generationId,
            identity,
            std::move(admissionLease),
            std::move(acceptedConnection));
        {
            const std::scoped_lock lock{mutex_};
            created_.push_back(target);
        }
        return Domain::Result<std::shared_ptr<
            Detail::IDashboardConnectionDispatchTarget>>::success(
            std::move(target));
    }

    [[nodiscard]] std::shared_ptr<IntegrationConnectionTarget> latest()
        const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return created_.empty() ? nullptr : created_.back();
    }

private:
    const void* const policyIdentity_{};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IntegrationConnectionTarget>> created_;
};

void testConcreteAcceptOwnerRejectsImmutableCompositionMismatch()
{
    RuntimeFixture time;
    const auto endpoint = take(Detail::DashboardLoopbackEndpoint::create(
        "127.0.0.1", 18472U));
    IntegrationExtensionState extensionState{endpoint.port()};
    IntegrationExtensionScope extensionScope{extensionState};
    auto winsockApi = std::make_shared<IntegrationWinsockApi>();
    auto winsock = take(Detail::DashboardWinsockRuntime::create(winsockApi));
    auto extensionApi = std::make_shared<IntegrationExtensionApi>();
    auto acceptApi = std::make_shared<IntegrationAcceptApi>();

    auto completionApi =
        std::make_shared<IntegrationCompletionPortApi>();
    auto registry = take(Detail::DashboardConnectionRegistry::create(
        IntegrationDeadlineKey));
    auto completionPort = take(Detail::DashboardIoCompletionPort::create(
        completionApi));
    auto kernel = take(Detail::DashboardIocpWorkerKernel::create(
        std::move(completionPort), registry));
    auto deadlineBridge = take(Detail::DashboardDeadlineIocpBridge::create(
        *kernel, IntegrationDeadlineKey));
    takeVoid(registry->bindDeadlineBridge(deadlineBridge));

    auto admission = take(Detail::DashboardAdmissionController::create(
        Dashboard::DashboardTransportLimits{2U, 2U, 4U}));
    auto application = std::make_shared<IntegrationApplication>();
    auto factory = std::make_shared<IntegrationOwnerFactory>(
        application.get());
    auto registrar =
        std::make_shared<Detail::DashboardConnectionRegistryRegistrar>(
            *registry);
    auto overload = std::make_shared<FakeOverloadResponder>();

    const auto makeAcceptSlots = [&](const CompletionKey key) {
        auto listenerApi =
            std::make_shared<IntegrationListenerApi>(endpoint);
        auto listener = take(Detail::DashboardListeningSocket::create(
            *winsock, endpoint, std::move(listenerApi)));
        return take(Detail::DashboardAcceptSlotSet::create(
            *winsock,
            std::move(listener),
            key,
            extensionApi,
            acceptApi));
    };
    const auto makeHandoff = [&admission,
                              &time,
                              &registrar,
                              &overload](
                                 const std::uint64_t generationId,
                                 Detail::DashboardAcceptSlotSet& slots,
                                 std::shared_ptr<
                                     IntegrationOwnerFactory>
                                     ownerFactory) {
        return take(Detail::DashboardAcceptedConnectionHandoff::create(
            generationId,
            slots,
            *admission,
            *time.runtime,
            std::move(ownerFactory),
            registrar,
            overload));
    };
    const auto requireRejectedBeforeOwnership = [&extensionState,
                                                  &registry](
        const Domain::Result<std::unique_ptr<
            Detail::DashboardListenerGenerationAcceptOwner>>& result,
        const std::string_view message) {
        requireError(
            result,
            Domain::ErrorCodes::InvalidRequest,
            false,
            message);
        const auto registrySnapshot = registry->snapshot();
        require(extensionState.issueCalls == 0U &&
                    extensionState.operationCount == 0U,
                "immutable composition rejection issued native acceptance");
        require(registrySnapshot.registeredConnectionCount() == 0U &&
                    registrySnapshot
                            .registeredAuxiliaryDeadlineTargetCount() ==
                        0U,
                "immutable composition rejection mutated registry ownership");
    };

    {
        auto slots = makeAcceptSlots(IntegrationListenerKey);
        auto handoff = makeHandoff(
            IntegrationGenerationId, *slots, factory);
        const auto result =
            Detail::DashboardListenerGenerationAcceptOwner::create(
                RuntimeIdentity{
                    IntegrationGenerationId + 1U,
                    IntegrationListenerKey},
                std::move(slots),
                std::move(handoff),
                application,
                *kernel);
        requireRejectedBeforeOwnership(
            result,
            "accept owner accepted a foreign generation identity");
    }

    {
        auto slots = makeAcceptSlots(IntegrationListenerKey);
        auto handoff = makeHandoff(
            IntegrationGenerationId, *slots, factory);
        const auto result =
            Detail::DashboardListenerGenerationAcceptOwner::create(
                RuntimeIdentity{
                    IntegrationGenerationId,
                    CompletionKey{IntegrationListenerKey.value() + 1U}},
                std::move(slots),
                std::move(handoff),
                application,
                *kernel);
        requireRejectedBeforeOwnership(
            result,
            "accept owner accepted a foreign completion key");
    }

    {
        auto slots = makeAcceptSlots(IntegrationListenerKey);
        auto handoffSlots = makeAcceptSlots(IntegrationListenerKey);
        auto handoff = makeHandoff(
            IntegrationGenerationId, *handoffSlots, factory);
        const auto result =
            Detail::DashboardListenerGenerationAcceptOwner::create(
                RuntimeIdentity{
                    IntegrationGenerationId, IntegrationListenerKey},
                std::move(slots),
                std::move(handoff),
                application,
                *kernel);
        requireRejectedBeforeOwnership(
            result,
            "accept owner accepted a foreign exact accept-slot set");
        require(!handoffSlots->snapshot().startAttempted(),
                "foreign handoff accept-slot set was started");
    }

    {
        auto handoffApplication =
            std::make_shared<IntegrationApplication>();
        auto handoffFactory = std::make_shared<IntegrationOwnerFactory>(
            handoffApplication.get());
        auto slots = makeAcceptSlots(IntegrationListenerKey);
        auto handoff = makeHandoff(
            IntegrationGenerationId, *slots, handoffFactory);
        const auto result =
            Detail::DashboardListenerGenerationAcceptOwner::create(
                RuntimeIdentity{
                    IntegrationGenerationId, IntegrationListenerKey},
                std::move(slots),
                std::move(handoff),
                application,
                *kernel);
        requireRejectedBeforeOwnership(
            result,
            "accept owner accepted a foreign application policy");
    }

    const auto makeAcceptOwner = [&]() {
        auto slots = makeAcceptSlots(IntegrationListenerKey);
        auto handoff = makeHandoff(
            IntegrationGenerationId, *slots, factory);
        return take(
            Detail::DashboardListenerGenerationAcceptOwner::create(
                RuntimeIdentity{
                    IntegrationGenerationId, IntegrationListenerKey},
                std::move(slots),
                std::move(handoff),
                application,
                *kernel));
    };
    const auto resetAcceptState = [&]() noexcept {
        extensionState.operations.fill(nullptr);
        extensionState.operationCount = 0U;
        extensionState.issueCalls = 0U;
        extensionState.failIssueCall = 0U;
        acceptApi->cancelCalls = 0U;
        acceptApi->failCancelCall = 0U;
        acceptApi->secondFailCancelCall = 0U;
        acceptApi->cancelError = ERROR_ACCESS_DENIED;
    };

    {
        resetAcceptState();
        auto owner = makeAcceptOwner();
        takeVoid(owner->start());
        acceptApi->failCancelCall = 1U;
        const auto closed = owner->closeAdmission();
        requireError(
            closed,
            Domain::ErrorCodes::Unauthorized,
            false,
            "concrete accept owner discarded a CancelIoEx failure");
        require(acceptApi->cancelCalls ==
                    Detail::DashboardAcceptSlotSet::SlotCount,
                "concrete close did not continue its bounded cancel pass");

        const auto unsolicited = owner->consume(
            Detail::DashboardIoCompletionPacket{
                0U,
                IntegrationListenerKey,
                extensionState.operations[0U]},
            ERROR_OPERATION_ABORTED);
        requireError(
            unsolicited,
            Domain::ErrorCodes::IntegrityFailure,
            false,
            "concrete accept owner accepted an unrequested abort");

        takeVoid(owner->forceCloseListener());
        for (std::size_t index = 1U;
             index < Detail::DashboardAcceptSlotSet::SlotCount;
             ++index) {
            takeVoid(owner->consume(
                Detail::DashboardIoCompletionPacket{
                    0U,
                    IntegrationListenerKey,
                    extensionState.operations[index]},
                ERROR_OPERATION_ABORTED));
        }
        require(owner->fullyDrained(),
                "concrete owner did not drain after unsolicited abort handling");
    }

    {
        resetAcceptState();
        auto owner = makeAcceptOwner();
        takeVoid(owner->start());
        acceptApi->failCancelCall = 1U;
        acceptApi->secondFailCancelCall = 5U;
        const auto closed = owner->closeAdmission();
        requireError(
            closed,
            Domain::ErrorCodes::Unauthorized,
            false,
            "concrete owner discarded its first cancellation failure");
        const auto forced = owner->forceCloseListener();
        requireError(
            forced,
            Domain::ErrorCodes::Unauthorized,
            false,
            "concrete owner discarded its repeated cancellation failure");
        require(acceptApi->cancelCalls == 5U,
                "concrete owner exceeded one cancellation retry");
        for (std::size_t index{};
             index < Detail::DashboardAcceptSlotSet::SlotCount;
             ++index) {
            takeVoid(owner->consume(
                Detail::DashboardIoCompletionPacket{
                    0U,
                    IntegrationListenerKey,
                    extensionState.operations[index]},
                ERROR_OPERATION_ABORTED));
        }
        require(owner->fullyDrained(),
                "force-closed concrete owner released storage before reaps");
    }

    registry->beginShutdown();
    kernel->shutdown();
}

void testRealHandoffResumeFailureDrainsExactRegistryGeneration()
{
    RuntimeFixture time;
    const auto endpoint = take(Detail::DashboardLoopbackEndpoint::create(
        "127.0.0.1", 18473U));
    IntegrationExtensionState extensionState{endpoint.port()};
    IntegrationExtensionScope extensionScope{extensionState};
    auto winsockApi = std::make_shared<IntegrationWinsockApi>();
    auto winsock = take(Detail::DashboardWinsockRuntime::create(winsockApi));
    auto listenerApi = std::make_shared<IntegrationListenerApi>(endpoint);
    auto listener = take(Detail::DashboardListeningSocket::create(
        *winsock, endpoint, listenerApi));
    auto extensionApi = std::make_shared<IntegrationExtensionApi>();
    auto acceptApi = std::make_shared<IntegrationAcceptApi>();

    auto completionApi =
        std::make_shared<IntegrationCompletionPortApi>();
    auto registry = take(Detail::DashboardConnectionRegistry::create(
        IntegrationDeadlineKey));
    auto completionPort = take(Detail::DashboardIoCompletionPort::create(
        completionApi));
    auto kernel = take(Detail::DashboardIocpWorkerKernel::create(
        std::move(completionPort), registry));
    auto deadlineBridge = take(Detail::DashboardDeadlineIocpBridge::create(
        *kernel, IntegrationDeadlineKey));
    takeVoid(registry->bindDeadlineBridge(deadlineBridge));

    auto acceptSlots = take(Detail::DashboardAcceptSlotSet::create(
        *winsock,
        std::move(listener),
        IntegrationListenerKey,
        extensionApi,
        acceptApi));
    auto* const acceptSlotsView = acceptSlots.get();
    auto admission = take(Detail::DashboardAdmissionController::create(
        Dashboard::DashboardTransportLimits{2U, 2U, 4U}));
    auto application = std::make_shared<IntegrationApplication>();
    auto factory = std::make_shared<IntegrationOwnerFactory>(
        application.get());
    auto registrar =
        std::make_shared<Detail::DashboardConnectionRegistryRegistrar>(
            *registry);
    auto overload = std::make_shared<FakeOverloadResponder>();
    auto handoff = take(Detail::DashboardAcceptedConnectionHandoff::create(
        IntegrationGenerationId,
        *acceptSlots,
        *admission,
        *time.runtime,
        factory,
        registrar,
        overload));
    auto acceptOwner = take(
        Detail::DashboardListenerGenerationAcceptOwner::create(
            RuntimeIdentity{
                IntegrationGenerationId, IntegrationListenerKey},
            std::move(acceptSlots),
            std::move(handoff),
            application,
            *kernel));
    auto connectionControl = std::make_shared<
        Detail::DashboardConnectionRegistryGenerationControl>(*registry);
    auto transitionGate = std::make_shared<TransitionGate>();
    auto generation = take(Generation::create(
        RuntimeIdentity{IntegrationGenerationId, IntegrationListenerKey},
        std::move(acceptOwner),
        *time.scheduler,
        *time.runtime,
        connectionControl,
        overload,
        transitionGate));

    auto existing = std::make_shared<IntegrationConnectionTarget>(
        IntegrationGenerationId,
        RuntimeIdentity{901U, CompletionKey{0x7500U}});
    takeVoid(registry->registerConnection(existing));
    require(existing->started(),
            "real registry did not start the pre-existing owner");
    require(
        registry->connectionCountForGeneration(
            IntegrationGenerationId) == 1U,
        "real registry did not retain the pre-existing generation owner");

    {
        auto transition = transitionGate->enter();
        takeVoid(generation->startAdmission(transition));
    }
    require(extensionState.issueCalls ==
                Detail::DashboardAcceptSlotSet::SlotCount &&
                extensionState.operationCount ==
                    Detail::DashboardAcceptSlotSet::SlotCount,
            "real accept owner did not issue its exact four slots");

    extensionState.failIssueCall =
        Detail::DashboardAcceptSlotSet::SlotCount + 1U;
    generation->consume(
        Detail::DashboardIoCompletionPacket{
            0U, IntegrationListenerKey, extensionState.operations[0]},
        ERROR_SUCCESS);

    const auto created = factory->latest();
    require(created != nullptr && created->started(),
            "handoff resume failure happened before registry start");
    require(
        registry->connectionCountForGeneration(
            IntegrationGenerationId) == 2U,
        "post-registration resume failure lost a generation owner");
    require(existing->shutdownRequested() &&
                existing->shutdownCalls() == 1U &&
                created->shutdownRequested() &&
                created->shutdownCalls() == 1U,
            "generation shutdown did not cover existing and new owners");
    require(generation->snapshot().hasFailure(),
            "real resume-reissue failure was not retained by generation");
    require(overload->cancelledIds() ==
                std::vector<std::uint64_t>{IntegrationGenerationId},
            "real resume failure cancelled the wrong overload generation");
    require(acceptSlotsView->snapshot().drainedCount() == 1U &&
                acceptSlotsView->snapshot().cancellationRequestedCount() ==
                    3U,
            "failed exact reissue did not retain three accept reaps");

    existing->completeExactNativeReap();
    require(
        registry->connectionCountForGeneration(
            IntegrationGenerationId) == 1U,
        "first exact connection reap removed the wrong registry set");
    require(take(admission->snapshot()).shortConnectionCount() == 1U,
            "first exact reap released the new owner's admission");

    created->completeExactNativeReap();
    require(
        registry->connectionCountForGeneration(
            IntegrationGenerationId) == 0U,
        "last exact connection reap did not empty the registry generation");
    require(take(admission->snapshot()).totalConnectionCount() == 0U,
            "last exact connection reap retained admission ownership");
    require(registry->snapshot().removedConnectionCount() == 2U,
            "registry did not remove both exact generation identities");

    for (std::size_t index = 1U;
         index < Detail::DashboardAcceptSlotSet::SlotCount;
         ++index) {
        generation->consume(
            Detail::DashboardIoCompletionPacket{
                0U,
                IntegrationListenerKey,
                extensionState.operations[index]},
            ERROR_OPERATION_ABORTED);
    }
    require(overload->cancelledIds() ==
                std::vector<std::uint64_t>{IntegrationGenerationId},
            "expected accept cancellation reaps repeated overload shutdown");
    require(existing->shutdownCalls() == 1U &&
                created->shutdownCalls() == 1U,
            "expected accept cancellation reaps repeated connection shutdown");
    require(acceptSlotsView->snapshot().fullyDrained(),
            "real accept owner retained a cancelled native obligation");
    require(generation->fullyDrained(),
            "real generation did not drain after registry and accept reaps");

    generation.reset();
    registry->beginShutdown();
    kernel->shutdown();
}

} // namespace

int main()
{
    try {
        testModeledSharedCallbackPinRetainsListenerKeyLease();
        testConcreteGracefulShutdownPreservesConnectionsUntilHardEscalation();
        testExactRetirementDeadlineAndPartialAcceptDrain();
        testPostRegistrationResumeFailureForceClosesGeneration();
        testCancellationFailureForceCloseAndBoundedReapWatchdog();
        testCancellationWatchdogPublicationRaceCancelsUncommittedArm();
        testWatchdogScheduleFailureInvokesFailFastAfterTransitionRelease();
        testTerminalPumpRunsAfterGenerationTransitionRelease();
        testGenerationRejectsAcceptOwnerIdentityMismatch();
        testRetirementAcceptCancellationPreservesConnectionGrace();
        testInitialPublishAndPrepareFailurePreservesActive();
        testStartToPublishDrainCannotPublishDeadGeneration();
        testSuccessfulRebindOrderingConflictAndDrain();
        testRegistrationRollback();
        testPartialRegistrationRollbackRetainsUntilDrain();
        testRollbackPublishesBeforeSynchronousDrainEdge();
        testUnregisterFailureBecomesRetainedFatal();
        testCollectionBarrierFailureIsFatalWithoutOwnerLoss();
        testGracefulAndFatalShutdown();
        testCoordinatorGracefulShutdownIsIdempotentAndHardEscalates();
        testCoordinatorHardShutdownSuppressesAndSerializesGracefulFanout();
        testGracefulCutoffSerializesAgainstRebindPublication();
        testCoordinatorDrainObserverInitialZeroAndOneShotBinding();
        testCoordinatorDrainObserverWaitsForPendingGracefulFanout();
        testCoordinatorDrainObserverWaitsForCollectionAndFiresOnce();
        testConnectionZeroObserverCompletesCollection();
        testOverloadDrainEdgeCompletesCollectionWithoutPolling();
        testOverloadTerminalEdgeFailsClosedExactlyOnce();
        testOverloadOwnerShutdownCoordinatesNormalOnce();
        testEmptyOverloadOwnerTerminalFailsClosed();
        testCompletionPendingHoldPreventsTerminalCollectionRace();
        testConcreteAcceptOwnerRejectsImmutableCompositionMismatch();
        testRealHandoffResumeFailureDrainsExactRegistryGeneration();
        std::cout << "Dashboard listener generation tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard listener generation tests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    }
}
