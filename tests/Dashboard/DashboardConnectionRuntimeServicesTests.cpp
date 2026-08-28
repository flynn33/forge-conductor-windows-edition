#include "Infrastructure/Windows/Detail/DashboardConnectionRuntimeServices.h"

#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using Key = Detail::DashboardIoCompletionKey;
using RuntimeIdentity = Detail::DashboardConnectionRuntimeIdentity;
using RuntimeServices = Detail::DashboardConnectionRuntimeServices;

using namespace std::chrono_literals;

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

static_assert(std::is_final_v<RuntimeServices>);
static_assert(std::is_abstract_v<Detail::IDashboardOperationalStateSource>);
static_assert(std::has_virtual_destructor_v<
              Detail::IDashboardOperationalStateSource>);
static_assert(std::is_aggregate_v<RuntimeIdentity>);
static_assert(!std::is_copy_constructible_v<RuntimeServices>);
static_assert(!std::is_move_constructible_v<RuntimeServices>);
static_assert(RuntimeServices::MaximumFixedCompletionKeyCount == 32U);
static_assert(RuntimeServices::MaximumOperationLifetime == 5s);
static_assert(noexcept(RuntimeServices::create({}, {}, {}, {})));
static_assert(noexcept(
    std::declval<RuntimeServices&>().allocateConnectionIdentity()));
static_assert(noexcept(
    std::declval<const RuntimeServices&>().monotonicNow()));
static_assert(noexcept(
    std::declval<const RuntimeServices&>().operationalServiceActive()));
static_assert(noexcept(
    std::declval<RuntimeServices&>().createOperationContext({})));

class MutableClock final : public Contracts::IClock {
public:
    explicit MutableClock(
        const Domain::MonotonicTimePoint monotonic) noexcept
        : monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return monotonic_;
    }

    void set(const Domain::MonotonicTimePoint value) noexcept
    {
        const std::lock_guard lock{mutex_};
        monotonic_ = value;
    }

private:
    mutable std::mutex mutex_;
    Domain::MonotonicTimePoint monotonic_;
};

class SequenceUuidGenerator final : public Contracts::IUuidGenerator {
public:
    explicit SequenceUuidGenerator(
        const std::size_t failAtCall = 0U,
        std::function<void(std::size_t)> onCall = {}) noexcept
        : failAtCall_{failAtCall}, onCall_{std::move(onCall)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto call = calls_.fetch_add(1U, std::memory_order_relaxed) +
                1U;
            if (onCall_) {
                onCall_(call);
            }
            if (failAtCall_ != 0U && call == failAtCall_) {
                return Domain::Result<Domain::Uuid>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::HostCapabilityUnavailable,
                        "Injected UUID failure."));
            }
            std::ostringstream text;
            text << "10000000-0000-4000-8000-" << std::setfill('0')
                 << std::setw(12) << call;
            return Domain::Uuid::parse(text.str());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Test UUID generation failed."));
        }
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t failAtCall_{};
    const std::function<void(std::size_t)> onCall_;
    std::atomic_size_t calls_{};
};

class BlockingFirstUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto invocation =
                invocations_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            if (invocation == 1U) {
                std::unique_lock lock{mutex_};
                firstCallEntered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this]() noexcept {
                    return firstCallReleased_;
                });
            }
            return sequence_.next();
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Blocking test UUID generation failed."));
        }
    }

    [[nodiscard]] bool waitForFirstCall(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this]() noexcept {
            return firstCallEntered_;
        });
    }

    void releaseFirstCall()
    {
        {
            const std::lock_guard lock{mutex_};
            firstCallReleased_ = true;
        }
        condition_.notify_all();
    }

private:
    SequenceUuidGenerator sequence_;
    std::atomic_size_t invocations_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool firstCallEntered_{};
    bool firstCallReleased_{};
};

class MutableOperationalState final
    : public Detail::IDashboardOperationalStateSource {
public:
    explicit MutableOperationalState(const bool active) noexcept
        : active_{active}
    {
    }

    [[nodiscard]] bool operationalServiceActive() const noexcept override
    {
        return active_.load(std::memory_order_acquire);
    }

    void set(const bool active) noexcept
    {
        active_.store(active, std::memory_order_release);
    }

private:
    std::atomic_bool active_{};
};

struct Fixture final {
    explicit Fixture(
        std::span<const Key> fixedKeys = {},
        const std::size_t uuidFailureCall = 0U)
        : clock{std::make_shared<MutableClock>(
              Domain::MonotonicTimePoint{100s})},
          uuidGenerator{
              std::make_shared<SequenceUuidGenerator>(uuidFailureCall)},
          operationalState{std::make_shared<MutableOperationalState>(true)},
          services{take(RuntimeServices::create(
              clock,
              uuidGenerator,
              operationalState,
              fixedKeys))}
    {
    }

    std::shared_ptr<MutableClock> clock;
    std::shared_ptr<SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<MutableOperationalState> operationalState;
    std::unique_ptr<RuntimeServices> services;
};

void constructionRejectsMissingOrUnboundedInputs()
{
    const auto clock = std::make_shared<MutableClock>(
        Domain::MonotonicTimePoint{});
    const auto uuids = std::make_shared<SequenceUuidGenerator>();
    const auto operational = std::make_shared<MutableOperationalState>(true);

    require(!RuntimeServices::create({}, uuids, operational),
            "missing clock was accepted");
    require(!RuntimeServices::create(clock, {}, operational),
            "missing UUID generator was accepted");
    require(!RuntimeServices::create(clock, uuids, {}),
            "missing operational-state source was accepted");

    const std::array duplicates{Key{7U}, Key{7U}};
    const auto duplicateResult = RuntimeServices::create(
        clock, uuids, operational, duplicates);
    require(!duplicateResult, "duplicate fixed completion keys were accepted");
    require(duplicateResult.error().code == Domain::ErrorCodes::InvalidRequest,
            "duplicate fixed keys used the wrong error code");

    std::vector<Key> tooMany;
    tooMany.reserve(RuntimeServices::MaximumFixedCompletionKeyCount + 1U);
    for (std::size_t index{};
         index < RuntimeServices::MaximumFixedCompletionKeyCount + 1U;
         ++index) {
        tooMany.push_back(Key{100U + index});
    }
    const auto oversized = RuntimeServices::create(
        clock, uuids, operational, tooMany);
    require(!oversized, "an unbounded fixed completion-key set was accepted");
    require(oversized.error().code == Domain::ErrorCodes::InvalidRequest,
            "an oversized fixed-key set used the wrong error code");
}

void identityAllocationIsMonotonicAndSkipsReservedKeys()
{
    const std::array fixed{
        Key{1U},
        Key{3U},
        Key{4U},
        Key{Detail::DashboardIocpWorkerKernel::ShutdownKeyValue}};
    Fixture fixture{fixed};

    const auto first = take(fixture.services->allocateConnectionIdentity());
    const auto second = take(fixture.services->allocateConnectionIdentity());
    const auto third = take(fixture.services->allocateConnectionIdentity());

    require(first.registrationId == 1U && second.registrationId == 2U &&
                third.registrationId == 3U,
            "registration identifiers were not strictly monotonic");
    require(first.completionKey == Key{2U},
            "the first dynamic completion key did not skip fixed key one");
    require(second.completionKey == Key{5U} &&
                third.completionKey == Key{6U},
            "dynamic completion keys reused or failed to skip fixed keys");
    require(first.completionKey.value() != 0U &&
                first.completionKey.value() !=
                    Detail::DashboardIocpWorkerKernel::ShutdownKeyValue,
            "a dynamic completion key used a reserved value");
}

void concurrentIdentityAllocationNeverReusesEitherSequence()
{
    constexpr std::size_t ThreadCount = 16U;
    constexpr std::size_t AllocationsPerThread = 256U;
    constexpr std::size_t Total = ThreadCount * AllocationsPerThread;
    const std::array fixed{Key{2U}, Key{19U}, Key{500U}, Key{2'500U}};
    Fixture fixture{fixed};
    std::mutex outputMutex;
    std::vector<RuntimeIdentity> identities;
    identities.reserve(Total);
    std::vector<std::jthread> workers;
    workers.reserve(ThreadCount);

    for (std::size_t thread{}; thread < ThreadCount; ++thread) {
        workers.emplace_back([&](std::stop_token) {
            std::vector<RuntimeIdentity> local;
            local.reserve(AllocationsPerThread);
            for (std::size_t index{}; index < AllocationsPerThread; ++index) {
                local.push_back(take(
                    fixture.services->allocateConnectionIdentity()));
            }
            const std::lock_guard lock{outputMutex};
            identities.insert(
                identities.end(), local.begin(), local.end());
        });
    }
    workers.clear();

    require(identities.size() == Total,
            "concurrent identity allocation lost a result");
    std::sort(
        identities.begin(),
        identities.end(),
        [](const RuntimeIdentity& left, const RuntimeIdentity& right) {
            return left.registrationId < right.registrationId;
        });
    for (std::size_t index{}; index < identities.size(); ++index) {
        require(identities[index].registrationId == index + 1U,
                "concurrent registration identifiers were reused or skipped");
        if (index != 0U) {
            require(
                identities[index - 1U].completionKey.value() <
                    identities[index].completionKey.value(),
                "concurrent completion keys were reused or reordered");
        }
        require(
            std::none_of(
                fixed.begin(), fixed.end(), [&](const Key key) {
                    return key == identities[index].completionKey;
                }),
            "concurrent allocation issued a fixed completion key");
    }
}

void boundedSequenceProvesTerminalAndReservedTransitions()
{
    using Sequence =
        Detail::DashboardBoundedMonotonicSequence<std::uint64_t>;
    constexpr auto Maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    const auto unreserved = [](const std::uint64_t) noexcept {
        return false;
    };

    Sequence terminal{Maximum - 1U};
    const auto maximumMinusOne = terminal.tryTake(unreserved);
    const auto maximum = terminal.tryTake(unreserved);
    const auto exhausted = terminal.tryTake(unreserved);
    require(maximumMinusOne.has_value() &&
                *maximumMinusOne == Maximum - 1U,
            "bounded sequence changed its max-minus-one transition");
    require(maximum.has_value() && *maximum == Maximum,
            "bounded sequence did not issue its terminal value");
    require(!exhausted.has_value() && terminal.exhausted(),
            "bounded sequence reused its terminal value");

    Sequence reservedTail{Maximum - 2U};
    const auto skippedToMaximum = reservedTail.tryTake(
        [](const std::uint64_t value) noexcept {
            constexpr auto Limit =
                (std::numeric_limits<std::uint64_t>::max)();
            return value == Limit - 2U || value == Limit - 1U;
        });
    require(skippedToMaximum.has_value() &&
                *skippedToMaximum == Maximum,
            "bounded sequence did not skip reserved terminal neighbors");
    require(!reservedTail.tryTake(unreserved).has_value(),
            "bounded sequence reused a value after a reserved transition");

    Sequence terminalReserved{Maximum};
    const auto noCandidate = terminalReserved.tryTake(
        [](const std::uint64_t value) noexcept {
            return value ==
                (std::numeric_limits<std::uint64_t>::max)();
        });
    require(!noCandidate.has_value() && terminalReserved.exhausted(),
            "a reserved terminal value did not exhaust the sequence");

    Sequence original{Maximum};
    auto rejectedStage = original;
    require(rejectedStage.tryTake(unreserved).has_value(),
            "staged terminal allocation unexpectedly failed");
    const auto afterRejectedStage = original.tryTake(unreserved);
    require(afterRejectedStage.has_value() &&
                *afterRejectedStage == Maximum,
            "a rejected staged allocation consumed the source sequence");
}

void timeOperationalStateAndContextsAreBoundedSnapshots()
{
    Fixture fixture;
    const auto now = Domain::MonotonicTimePoint{100s};
    require(fixture.services->monotonicNow() == now,
            "runtime time observation changed the injected clock value");
    require(fixture.services->operationalServiceActive(),
            "active operational state was not observed");
    fixture.operationalState->set(false);
    require(!fixture.services->operationalServiceActive(),
            "updated operational state was not observed");

    auto bounded = take(fixture.services->createOperationContext(now + 1h));
    require(bounded.deadline == now + 5s,
            "long operation context exceeded its five-second ceiling");
    require(
        bounded.operationId.value() == "10000000-0000-4000-8000-000000000001",
        "operation identity did not use the first generated UUID");
    require(
        bounded.correlationId.value() ==
            "10000000-0000-4000-8000-000000000002",
        "correlation identity did not use the second generated UUID");
    require(!bounded.isCancellationRequested(),
            "live operation context reported cancellation");

    auto shortContext = take(
        fixture.services->createOperationContext(now + 2s));
    require(shortContext.deadline == now + 2s,
            "absolute deadline below five seconds was not preserved");
    require(fixture.uuidGenerator->calls() == 4U,
            "successful contexts did not consume exactly two UUIDs each");
}

void cancelledExpiredAndGeneratorFailuresAreTyped()
{
    Fixture fixture;
    const auto now = Domain::MonotonicTimePoint{100s};
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled = fixture.services->createOperationContext(
        now + 1s, cancellation.get_token());
    require(!cancelled, "an already-cancelled context was created");
    require(cancelled.error().code == Domain::ErrorCodes::Cancelled,
            "cancelled context used the wrong error code");

    const auto expired = fixture.services->createOperationContext(now);
    require(!expired, "an already-expired context was created");
    require(expired.error().code == Domain::ErrorCodes::DeadlineExceeded,
            "expired context used the wrong error code");
    require(fixture.uuidGenerator->calls() == 0U,
            "rejected contexts consumed UUIDs");

    Fixture firstFailure{std::span<const Key>{}, 1U};
    const auto first = firstFailure.services->createOperationContext(now + 1s);
    require(!first, "first UUID failure produced a context");
    require(first.error().code ==
                Domain::ErrorCodes::HostCapabilityUnavailable,
            "first UUID failure was not preserved");
    require(firstFailure.uuidGenerator->calls() == 1U,
            "first UUID failure consumed the wrong number of calls");

    Fixture secondFailure{std::span<const Key>{}, 2U};
    const auto second = secondFailure.services->createOperationContext(now + 1s);
    require(!second, "correlation UUID failure produced a context");
    require(second.error().code ==
                Domain::ErrorCodes::HostCapabilityUnavailable,
            "correlation UUID failure was not preserved");
    require(secondFailure.uuidGenerator->calls() == 2U,
            "correlation UUID failure consumed the wrong number of calls");
}

void contextCreationRechecksClockAndCancellationAfterUuidWork()
{
    const auto initial = Domain::MonotonicTimePoint{100s};
    const auto operational = std::make_shared<MutableOperationalState>(true);

    const auto advancedClock = std::make_shared<MutableClock>(initial);
    const auto advancingUuids = std::make_shared<SequenceUuidGenerator>(
        0U,
        [advancedClock, initial](const std::size_t call) noexcept {
            if (call == 2U) {
                advancedClock->set(initial + 2s);
            }
        });
    auto advancedServices = take(RuntimeServices::create(
        advancedClock, advancingUuids, operational));
    const auto advanced = take(
        advancedServices->createOperationContext(initial + 20s));
    require(advanced.deadline == initial + 7s,
            "context deadline was not bounded from the post-UUID clock "
            "observation");

    const auto expiredClock = std::make_shared<MutableClock>(initial);
    const auto expiringUuids = std::make_shared<SequenceUuidGenerator>(
        0U,
        [expiredClock, initial](const std::size_t call) noexcept {
            if (call == 2U) {
                expiredClock->set(initial + 11s);
            }
        });
    auto expiredServices = take(RuntimeServices::create(
        expiredClock, expiringUuids, operational));
    const auto expired = expiredServices->createOperationContext(
        initial + 10s);
    require(!expired,
            "a deadline that expired during UUID generation was accepted");
    require(expired.error().code == Domain::ErrorCodes::DeadlineExceeded,
            "mid-creation deadline expiry used the wrong error code");
    require(expiringUuids->calls() == 2U,
            "mid-creation deadline test did not finish both UUID calls");

    std::stop_source cancellation;
    const auto cancellationClock =
        std::make_shared<MutableClock>(initial);
    const auto cancellingUuids = std::make_shared<SequenceUuidGenerator>(
        0U,
        [&cancellation](const std::size_t call) noexcept {
            if (call == 2U) {
                cancellation.request_stop();
            }
        });
    auto cancellationServices = take(RuntimeServices::create(
        cancellationClock, cancellingUuids, operational));
    const auto cancelled = cancellationServices->createOperationContext(
        initial + 10s, cancellation.get_token());
    require(!cancelled,
            "cancellation during UUID generation was not rechecked");
    require(cancelled.error().code == Domain::ErrorCodes::Cancelled,
            "mid-creation cancellation used the wrong error code");
    require(cancellingUuids->calls() == 2U,
            "mid-creation cancellation did not finish both UUID calls");

    const auto firstCallExpiredClock =
        std::make_shared<MutableClock>(initial);
    const auto firstCallExpiringUuids =
        std::make_shared<SequenceUuidGenerator>(
            0U,
            [firstCallExpiredClock, initial](
                const std::size_t call) noexcept {
                if (call == 1U) {
                    firstCallExpiredClock->set(initial + 11s);
                }
            });
    auto firstCallExpiredServices = take(RuntimeServices::create(
        firstCallExpiredClock, firstCallExpiringUuids, operational));
    const auto firstCallExpired =
        firstCallExpiredServices->createOperationContext(initial + 10s);
    require(!firstCallExpired,
            "first UUID work was allowed to cross the deadline");
    require(firstCallExpired.error().code ==
                Domain::ErrorCodes::DeadlineExceeded,
            "first-call deadline expiry used the wrong error code");
    require(firstCallExpiringUuids->calls() == 1U,
            "deadline expiry after the first UUID consumed a second UUID");

    std::stop_source firstCallCancellation;
    const auto firstCallCancellationClock =
        std::make_shared<MutableClock>(initial);
    const auto firstCallCancellingUuids =
        std::make_shared<SequenceUuidGenerator>(
            0U,
            [&firstCallCancellation](const std::size_t call) noexcept {
                if (call == 1U) {
                    firstCallCancellation.request_stop();
                }
            });
    auto firstCallCancellationServices = take(RuntimeServices::create(
        firstCallCancellationClock,
        firstCallCancellingUuids,
        operational));
    const auto firstCallCancelled =
        firstCallCancellationServices->createOperationContext(
            initial + 10s, firstCallCancellation.get_token());
    require(!firstCallCancelled,
            "cancellation after the first UUID was not observed");
    require(firstCallCancelled.error().code == Domain::ErrorCodes::Cancelled,
            "first-call cancellation used the wrong error code");
    require(firstCallCancellingUuids->calls() == 1U,
            "cancellation after the first UUID consumed a second UUID");
}

void blockedUuidWorkDoesNotSerializeUnrelatedContextAdmission()
{
    const auto initial = Domain::MonotonicTimePoint{100s};
    const auto clock = std::make_shared<MutableClock>(initial);
    const auto generator = std::make_shared<BlockingFirstUuidGenerator>();
    const auto operational = std::make_shared<MutableOperationalState>(true);
    auto services = take(RuntimeServices::create(
        clock, generator, operational));

    std::optional<Domain::Result<Domain::OperationContext>> firstResult;
    std::jthread firstWorker{[&]() {
        firstResult.emplace(
            services->createOperationContext(initial + 10s));
    }};

    const auto firstEntered = generator->waitForFirstCall(5s);
    if (!firstEntered) {
        generator->releaseFirstCall();
        firstWorker.join();
    }
    require(firstEntered, "the blocking UUID call was never entered");

    std::optional<Domain::Result<Domain::OperationContext>> secondResult;
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool secondCompleted{};
    std::jthread secondWorker{[&]() {
        secondResult.emplace(
            services->createOperationContext(initial + 10s));
        {
            const std::lock_guard lock{completionMutex};
            secondCompleted = true;
        }
        completionCondition.notify_all();
    }};

    bool completedWhileFirstBlocked{};
    {
        std::unique_lock lock{completionMutex};
        completedWhileFirstBlocked = completionCondition.wait_for(
            lock, 2s, [&secondCompleted]() noexcept {
                return secondCompleted;
            });
    }

    generator->releaseFirstCall();
    firstWorker.join();
    secondWorker.join();

    require(completedWhileFirstBlocked,
            "blocked UUID work serialized unrelated context admission");
    require(firstResult.has_value() && firstResult->hasValue(),
            "the released context request did not complete successfully");
    require(secondResult.has_value() && secondResult->hasValue(),
            "the unrelated context request did not complete successfully");
}

} // namespace

int main()
{
    try {
        constructionRejectsMissingOrUnboundedInputs();
        identityAllocationIsMonotonicAndSkipsReservedKeys();
        concurrentIdentityAllocationNeverReusesEitherSequence();
        boundedSequenceProvesTerminalAndReservedTransitions();
        timeOperationalStateAndContextsAreBoundedSnapshots();
        cancelledExpiredAndGeneratorFailuresAreTyped();
        contextCreationRechecksClockAndCancellationAfterUuidWork();
        blockedUuidWorkDoesNotSerializeUnrelatedContextAdmission();
        std::cout << "Dashboard connection runtime services tests passed ("
                  << assertionCount.load(std::memory_order_relaxed)
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard connection runtime services tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard connection runtime services tests failed with "
                     "an unknown error.\n";
        return 1;
    }
}
