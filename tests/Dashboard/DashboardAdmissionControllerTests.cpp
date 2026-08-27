#include "Infrastructure/Windows/Detail/DashboardAdmissionController.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using Controller = Detail::DashboardAdmissionController;
using Kind = Detail::DashboardAdmissionKind;
using Lease = Controller::Lease;

std::size_t assertionCount{};

// Owns test worker teardown from before the first thread is constructed. This
// keeps partial construction and normal completion on the same gate protocol.
class WorkerGateGuard final {
public:
    WorkerGateGuard(
        std::latch& start,
        std::vector<std::jthread>& workers,
        std::atomic_bool* release = nullptr) noexcept
        : start_{start}, workers_{workers}, release_{release}
    {
    }

    ~WorkerGateGuard() noexcept
    {
        join();
    }

    WorkerGateGuard(const WorkerGateGuard&) = delete;
    WorkerGateGuard& operator=(const WorkerGateGuard&) = delete;
    WorkerGateGuard(WorkerGateGuard&&) = delete;
    WorkerGateGuard& operator=(WorkerGateGuard&&) = delete;

    void open() noexcept
    {
        if (!startOpened_) {
            start_.count_down();
            startOpened_ = true;
        }
    }

    void join() noexcept
    {
        open();
        if (release_ != nullptr) {
            release_->store(true, std::memory_order_release);
            release_->notify_all();
        }
        workers_.clear();
    }

private:
    std::latch& start_;
    std::vector<std::jthread>& workers_;
    std::atomic_bool* release_{};
    bool startOpened_{};
};

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
void requireRetryableLimit(
    const Domain::Result<Value>& result,
    const std::string_view message)
{
    require(!result, message);
    require(
        result.error().code == Domain::ErrorCodes::LimitExceeded,
        "capacity failure did not use limit_exceeded");
    require(result.error().retryable, "capacity failure was not retryable");
}

[[nodiscard]] std::unique_ptr<Controller> controller(
    const Dashboard::DashboardTransportLimits limits = {})
{
    return take(Controller::create(limits));
}

[[nodiscard]] Lease accept(Controller& owner)
{
    return take(owner.tryAccept());
}

[[nodiscard]] Lease acceptSse(Controller& owner)
{
    auto lease = accept(owner);
    takeVoid(lease.convertToSse());
    return lease;
}

void requireEmpty(Controller& owner)
{
    const auto snapshot = take(owner.snapshot());
    require(snapshot.shortConnectionCount() == 0U, "short count did not drain");
    require(snapshot.sseConnectionCount() == 0U, "SSE count did not drain");
    require(snapshot.totalConnectionCount() == 0U, "total count did not drain");
    require(snapshot.withinLimits(), "empty snapshot violated its limits");
}

void exactDefaultsAndReducedLimitsAreValidated()
{
    const Dashboard::DashboardTransportLimits defaults;
    require(
        defaults.maximumShortConnections == 8U,
        "default short ceiling changed");
    require(
        defaults.maximumSseConnections == 32U,
        "default SSE ceiling changed");
    require(
        defaults.maximumTotalConnections == 40U,
        "default total ceiling changed");
    require(static_cast<bool>(defaults.validate()), "default limits were rejected");

    const Dashboard::DashboardTransportLimits reduced{2U, 3U, 5U};
    require(static_cast<bool>(reduced.validate()), "reduced limits were rejected");
    const auto reducedOwner = controller(reduced);
    require(reducedOwner->limits() == reduced, "controller changed reduced limits");

    const std::vector<Dashboard::DashboardTransportLimits> invalid{
        {0U, 1U, 1U},
        {1U, 0U, 1U},
        {1U, 1U, 0U},
        {9U, 1U, 10U},
        {1U, 33U, 34U},
        {8U, 32U, 41U},
        {2U, 3U, 4U},
        {2U, 3U, 6U},
    };
    for (const auto limits : invalid) {
        const auto validation = limits.validate();
        require(!validation, "invalid dashboard limits were accepted");
        require(
            validation.error().code == Domain::ErrorCodes::InvalidRequest,
            "invalid limits returned the wrong error code");
        require(
            !Controller::create(limits),
            "controller accepted invalid dashboard limits");
    }
}

void eightShortAdmissionsRejectTheNinthWithoutQueueing()
{
    auto owner = controller();
    std::vector<Lease> leases;
    leases.reserve(8U);
    for (std::size_t index{}; index < 8U; ++index) {
        leases.push_back(accept(*owner));
        require(leases.back().ownsAdmission(), "short lease did not own admission");
        require(leases.back().kind() == Kind::Short, "new lease was not short");
    }

    const auto ninth = owner->tryAccept();
    requireRetryableLimit(ninth, "ninth short admission unexpectedly succeeded");
    const auto full = take(owner->snapshot());
    require(full.shortConnectionCount() == 8U, "short ceiling was not retained");
    require(full.sseConnectionCount() == 0U, "short saturation created SSE state");
    require(full.totalConnectionCount() == 8U, "short total was incorrect");
    require(full.withinLimits(), "short saturation violated limits");

    leases.clear();
    requireEmpty(*owner);
}

void thirtyTwoSseAdmissionsRejectTheThirtyThirdConversion()
{
    auto owner = controller();
    std::vector<Lease> streams;
    streams.reserve(32U);
    for (std::size_t index{}; index < 32U; ++index) {
        streams.push_back(acceptSse(*owner));
        require(
            streams.back().kind() == Kind::ServerSentEvents,
            "converted lease was not SSE");
    }

    auto candidate = accept(*owner);
    const auto conversion = candidate.convertToSse();
    requireRetryableLimit(
        conversion,
        "thirty-third SSE conversion unexpectedly succeeded");
    require(candidate.ownsAdmission(), "failed conversion released its lease");
    require(candidate.kind() == Kind::Short, "failed conversion changed lease kind");

    const auto snapshot = take(owner->snapshot());
    require(snapshot.shortConnectionCount() == 1U, "failed conversion lost short count");
    require(snapshot.sseConnectionCount() == 32U, "SSE ceiling was not retained");
    require(snapshot.totalConnectionCount() == 33U, "failed conversion changed total");
    require(snapshot.withinLimits(), "SSE saturation violated limits");

    candidate.release();
    streams.clear();
    requireEmpty(*owner);
}

void fortyTotalAdmissionsRejectTheFortyFirst()
{
    auto owner = controller();
    std::vector<Lease> streams;
    streams.reserve(32U);
    for (std::size_t index{}; index < 32U; ++index) {
        streams.push_back(acceptSse(*owner));
    }
    std::vector<Lease> shortRequests;
    shortRequests.reserve(8U);
    for (std::size_t index{}; index < 8U; ++index) {
        shortRequests.push_back(accept(*owner));
    }

    const auto fortyFirst = owner->tryAccept();
    requireRetryableLimit(
        fortyFirst,
        "forty-first dashboard connection unexpectedly succeeded");
    const auto snapshot = take(owner->snapshot());
    require(snapshot.shortConnectionCount() == 8U, "full short count was incorrect");
    require(snapshot.sseConnectionCount() == 32U, "full SSE count was incorrect");
    require(snapshot.totalConnectionCount() == 40U, "total ceiling was not exact");
    require(snapshot.withinLimits(), "forty admitted connections violated limits");

    shortRequests.clear();
    streams.clear();
    requireEmpty(*owner);
}

void failedConversionRetainsTheOriginalShortLease()
{
    auto owner = controller({2U, 1U, 3U});
    auto stream = acceptSse(*owner);
    auto shortRequest = accept(*owner);

    const auto conversion = shortRequest.convertToSse();
    requireRetryableLimit(conversion, "reduced SSE ceiling was exceeded");
    const auto retained = take(owner->snapshot());
    require(retained.shortConnectionCount() == 1U, "failed conversion lost short lease");
    require(retained.sseConnectionCount() == 1U, "failed conversion changed SSE count");
    require(retained.totalConnectionCount() == 2U, "failed conversion changed total count");
    require(shortRequest.ownsAdmission(), "failed conversion cleared ownership");
    require(shortRequest.kind() == Kind::Short, "failed conversion changed kind");

    stream.release();

    takeVoid(shortRequest.convertToSse());
    require(
        shortRequest.kind() == Kind::ServerSentEvents,
        "conversion retry did not change the lease kind");
    const auto retried = take(owner->snapshot());
    require(retried.shortConnectionCount() == 0U, "retry retained a short count");
    require(retried.sseConnectionCount() == 1U, "retry did not reserve SSE capacity");
    require(retried.totalConnectionCount() == 1U, "retry changed the total count");

    shortRequest.release();
    requireEmpty(*owner);
}

void repeatedConversionReturnsTypedRejection()
{
    auto owner = controller({1U, 1U, 2U});
    auto stream = accept(*owner);
    takeVoid(stream.convertToSse());

    const auto repeated = stream.convertToSse();
    require(!repeated, "an SSE lease converted a second time");
    require(
        repeated.error().code == Domain::ErrorCodes::InvalidRequest,
        "repeated conversion returned the wrong error code");
    require(!repeated.error().retryable, "repeated conversion was retryable");
    require(stream.ownsAdmission(), "repeated conversion released the lease");
    require(
        stream.kind() == Kind::ServerSentEvents,
        "repeated conversion changed the lease kind");
    const auto snapshot = take(owner->snapshot());
    require(snapshot.shortConnectionCount() == 0U, "repeated conversion added short state");
    require(snapshot.sseConnectionCount() == 1U, "repeated conversion changed SSE state");

    stream.release();
    requireEmpty(*owner);
}

void leasesRetainStateAfterControllerDestruction()
{
    auto owner = controller({1U, 1U, 2U});
    auto occupiedStream = acceptSse(*owner);
    auto survivingShort = accept(*owner);
    owner.reset();

    const auto atCapacity = survivingShort.convertToSse();
    requireRetryableLimit(
        atCapacity,
        "lease outliving its controller ignored retained SSE capacity");
    require(
        survivingShort.kind() == Kind::Short,
        "failed post-controller conversion changed the lease kind");

    occupiedStream.release();
    takeVoid(survivingShort.convertToSse());
    require(
        survivingShort.kind() == Kind::ServerSentEvents,
        "surviving lease could not convert after capacity was released");
    survivingShort.release();
    require(
        !survivingShort.ownsAdmission(),
        "surviving lease did not release after controller destruction");
}

void moveAndDestructionReleaseExactlyOnce()
{
    auto owner = controller({2U, 1U, 3U});
    auto first = accept(*owner);
    auto second = accept(*owner);
    Lease moved{std::move(first)};
    require(!first.ownsAdmission(), "move source retained admission");
    require(moved.ownsAdmission(), "move destination lost admission");
    require(take(owner->snapshot()).shortConnectionCount() == 2U, "move changed count");

    moved = std::move(second);
    require(!second.ownsAdmission(), "move-assignment source retained admission");
    require(moved.ownsAdmission(), "move-assignment destination lost admission");
    require(
        take(owner->snapshot()).shortConnectionCount() == 1U,
        "move assignment did not release its prior lease exactly once");

    moved.release();
    moved.release();
    require(!moved.ownsAdmission(), "explicit release retained admission");
    requireEmpty(*owner);

    {
        auto scoped = acceptSse(*owner);
        require(scoped.ownsAdmission(), "scoped SSE lease was inactive");
    }
    requireEmpty(*owner);

    const auto releasedConversion = moved.convertToSse();
    require(!releasedConversion, "released lease converted to SSE");
    require(
        releasedConversion.error().code == Domain::ErrorCodes::InvalidRequest,
        "released conversion returned wrong error code");
}

void activeAndRetiringGenerationsShareOneController()
{
    auto owner = controller();
    std::vector<Lease> activeGeneration;
    std::vector<Lease> retiringGeneration;
    activeGeneration.reserve(8U);
    retiringGeneration.reserve(4U);

    for (std::size_t index{}; index < 4U; ++index) {
        retiringGeneration.push_back(accept(*owner));
        activeGeneration.push_back(accept(*owner));
    }
    requireRetryableLimit(
        owner->tryAccept(),
        "shared generations exceeded the one short ceiling");

    retiringGeneration.clear();
    for (std::size_t index{}; index < 4U; ++index) {
        activeGeneration.push_back(accept(*owner));
    }
    requireRetryableLimit(
        owner->tryAccept(),
        "active generation ignored retained shared admissions");
    const auto snapshot = take(owner->snapshot());
    require(snapshot.shortConnectionCount() == 8U, "shared short count was incorrect");
    require(snapshot.totalConnectionCount() == 8U, "shared total count was incorrect");
    require(snapshot.withinLimits(), "shared generations violated limits");

    activeGeneration.clear();
    requireEmpty(*owner);
}

void partialWorkerConstructionUnwindReleasesEveryGate()
{
    struct SimulatedConstructionFailure final {
    };

    std::latch start{1};
    std::atomic_bool release{};
    std::atomic_bool workerExited{};
    std::vector<std::jthread> workers;
    workers.reserve(2U);
    bool failureObserved{};

    try {
        WorkerGateGuard workerGuard{start, workers, &release};
        workers.emplace_back([&](std::stop_token) {
            start.wait();
            release.wait(false, std::memory_order_acquire);
            workerExited.store(true, std::memory_order_release);
        });
        throw SimulatedConstructionFailure{};
    } catch (const SimulatedConstructionFailure&) {
        failureObserved = true;
    }

    require(failureObserved, "partial worker construction did not unwind");
    require(release.load(), "partial construction did not open the release gate");
    require(workerExited.load(), "partial construction left its worker blocked");
    require(workers.empty(), "partial construction did not join its worker");
}

void concurrentConversionsAcrossDistinctLeases()
{
    constexpr std::size_t LeaseCount = 8U;
    auto owner = controller({LeaseCount, LeaseCount, LeaseCount * 2U});
    std::vector<Lease> leases;
    leases.reserve(LeaseCount);
    for (std::size_t index{}; index < LeaseCount; ++index) {
        leases.push_back(accept(*owner));
    }

    std::latch start{1};
    std::atomic<std::size_t> converted{};
    std::atomic_bool malformedFailure{};
    std::vector<std::jthread> workers;
    workers.reserve(LeaseCount);
    WorkerGateGuard workerGuard{start, workers};
    for (std::size_t index{}; index < LeaseCount; ++index) {
        workers.emplace_back([&, index](std::stop_token) {
            start.wait();
            const auto result = leases[index].convertToSse();
            if (result) {
                converted.fetch_add(1U, std::memory_order_relaxed);
            } else {
                malformedFailure.store(true, std::memory_order_release);
            }
        });
    }

    workerGuard.open();
    workerGuard.join();
    require(converted.load() == LeaseCount, "not every distinct lease converted");
    require(!malformedFailure.load(), "a distinct lease conversion failed");
    for (const auto& lease : leases) {
        require(
            lease.kind() == Kind::ServerSentEvents,
            "concurrent conversion left a lease classified as short");
    }
    const auto convertedSnapshot = take(owner->snapshot());
    require(
        convertedSnapshot.shortConnectionCount() == 0U,
        "concurrent conversions retained short counts");
    require(
        convertedSnapshot.sseConnectionCount() == LeaseCount,
        "concurrent conversions produced the wrong SSE count");
    require(
        convertedSnapshot.withinLimits(),
        "concurrent conversions exceeded a ceiling");

    leases.clear();
    requireEmpty(*owner);
}

void concurrentSaturationReturnsImmediatelyAndDrains()
{
    constexpr std::size_t ThreadCount = 64U;
    constexpr std::size_t InitialThreadCount = ThreadCount - 1U;
    auto owner = controller();
    std::latch start{1};
    std::atomic<std::size_t> attempted{};
    std::atomic<std::size_t> admitted{};
    std::atomic<std::size_t> rejected{};
    std::atomic_bool malformedFailure{};
    std::atomic_bool release{};
    std::vector<std::jthread> workers;
    workers.reserve(ThreadCount);
    WorkerGateGuard workerGuard{start, workers, &release};

    const auto attempt = [&](std::stop_token) {
        start.wait();
        auto result = owner->tryAccept();
        if (result) {
            auto lease = std::move(result).value();
            admitted.fetch_add(1U, std::memory_order_relaxed);
            attempted.fetch_add(1U, std::memory_order_release);
            attempted.notify_all();
            release.wait(false, std::memory_order_acquire);
        } else {
            if (result.error().code != Domain::ErrorCodes::LimitExceeded ||
                !result.error().retryable) {
                malformedFailure.store(true, std::memory_order_release);
            }
            rejected.fetch_add(1U, std::memory_order_relaxed);
            attempted.fetch_add(1U, std::memory_order_release);
            attempted.notify_all();
        }
    };

    for (std::size_t index{}; index < InitialThreadCount; ++index) {
        workers.emplace_back(attempt);
    }

    workerGuard.open();
    auto observed = attempted.load(std::memory_order_acquire);
    while (observed != InitialThreadCount) {
        attempted.wait(observed, std::memory_order_acquire);
        observed = attempted.load(std::memory_order_acquire);
    }

    // Preserve the observed value of 63 across worker creation. The wait must
    // compare against 63 even if the terminal worker publishes 64 first.
    workers.emplace_back(attempt);
    while (observed != ThreadCount) {
        attempted.wait(observed, std::memory_order_acquire);
        observed = attempted.load(std::memory_order_acquire);
    }

    const auto admittedCount = admitted.load(std::memory_order_acquire);
    const auto rejectedCount = rejected.load(std::memory_order_acquire);
    const auto malformed = malformedFailure.load(std::memory_order_acquire);
    const auto saturatedResult = owner->snapshot();

    workerGuard.join();

    require(admittedCount == 8U, "concurrent saturation admitted other than eight");
    require(rejectedCount == 56U, "concurrent saturation rejected wrong count");
    require(!malformed, "concurrent rejection was not typed retryable");
    require(static_cast<bool>(saturatedResult), "concurrent snapshot failed");
    const auto& saturated = saturatedResult.value();
    require(saturated.shortConnectionCount() == 8U, "concurrent count was not eight");
    require(saturated.withinLimits(), "concurrent saturation violated limits");
    requireEmpty(*owner);
}

void concurrentInvariantStressNeverExceedsAnyCeiling()
{
    constexpr std::size_t ThreadCount = 16U;
    constexpr std::size_t Iterations = 5'000U;
    auto owner = controller({3U, 5U, 8U});
    std::latch start{1};
    std::atomic_bool invariantFailure{};
    std::atomic<std::size_t> observations{};
    std::vector<std::jthread> workers;
    workers.reserve(ThreadCount);
    WorkerGateGuard workerGuard{start, workers};

    for (std::size_t threadIndex{}; threadIndex < ThreadCount; ++threadIndex) {
        workers.emplace_back([&, threadIndex](std::stop_token) {
            start.wait();
            for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
                auto result = owner->tryAccept();
                if (!result) {
                    if (result.error().code != Domain::ErrorCodes::LimitExceeded ||
                        !result.error().retryable) {
                        invariantFailure.store(true, std::memory_order_release);
                    }
                    continue;
                }

                auto lease = std::move(result).value();
                if (((iteration + threadIndex) & 1U) == 0U) {
                    const auto converted = lease.convertToSse();
                    if (!converted &&
                        (converted.error().code != Domain::ErrorCodes::LimitExceeded ||
                         !converted.error().retryable ||
                         !lease.ownsAdmission() || lease.kind() != Kind::Short)) {
                        invariantFailure.store(true, std::memory_order_release);
                    }
                }

                const auto observed = owner->snapshot();
                if (!observed || !observed.value().withinLimits() ||
                    observed.value().totalConnectionCount() !=
                        observed.value().shortConnectionCount() +
                            observed.value().sseConnectionCount()) {
                    invariantFailure.store(true, std::memory_order_release);
                }
                observations.fetch_add(1U, std::memory_order_relaxed);
                if ((iteration & 7U) == 0U) {
                    std::this_thread::yield();
                }
            }
        });
    }

    workerGuard.open();
    workerGuard.join();
    require(observations.load() > 0U, "invariant stress made no observations");
    require(!invariantFailure.load(), "invariant stress observed invalid state");
    requireEmpty(*owner);
}

} // namespace

int main()
{
    try {
        exactDefaultsAndReducedLimitsAreValidated();
        eightShortAdmissionsRejectTheNinthWithoutQueueing();
        thirtyTwoSseAdmissionsRejectTheThirtyThirdConversion();
        fortyTotalAdmissionsRejectTheFortyFirst();
        failedConversionRetainsTheOriginalShortLease();
        repeatedConversionReturnsTypedRejection();
        leasesRetainStateAfterControllerDestruction();
        moveAndDestructionReleaseExactlyOnce();
        activeAndRetiringGenerationsShareOneController();
        partialWorkerConstructionUnwindReleasesEveryGate();
        concurrentConversionsAcrossDistinctLeases();
        concurrentSaturationReturnsImmediatelyAndDrains();
        concurrentInvariantStressNeverExceedsAnyCeiling();
        std::cout << "Dashboard admission controller tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard admission controller tests failed after "
                  << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard admission controller tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
