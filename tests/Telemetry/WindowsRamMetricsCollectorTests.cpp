#include "ForgeConductor/Telemetry/Windows/WindowsRamMetricsCollector.h"
#include "Infrastructure/TestSupport.h"
#include "Telemetry/Windows/Detail/IRamMetricsPlatform.h"
#include "Telemetry/Windows/Detail/SynchronousCollectorGate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace ForgeConductor::Telemetry::Windows::Detail {

struct WindowsRamMetricsCollectorTestAccess final {
    [[nodiscard]] static std::unique_ptr<WindowsRamMetricsCollector> create(
        Contracts::IClock& clock,
        std::shared_ptr<IRamMetricsPlatform> platform)
    {
        return std::unique_ptr<WindowsRamMetricsCollector>{
            new WindowsRamMetricsCollector{clock, std::move(platform)}};
    }
};

} // namespace ForgeConductor::Telemetry::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

namespace TelemetryWindows = Telemetry::Windows;
namespace Detail = Telemetry::Windows::Detail;

using Collector = TelemetryWindows::WindowsRamMetricsCollector;
using Interface = Contracts::IRamMetricsCollector;
using Availability = Domain::TelemetryMetricAvailability;
using LifecycleGate = Detail::SynchronousCollectorGate;
using namespace std::chrono_literals;

using CollectMember = Domain::Result<Domain::RamMetrics> (
    Collector::*)(const Domain::OperationContext&) noexcept;
using ShutdownMember = void (Collector::*)() noexcept;

static_assert(std::is_final_v<Collector>);
static_assert(std::is_base_of_v<Interface, Collector>);
static_assert(std::has_virtual_destructor_v<Interface>);
static_assert(!std::is_copy_constructible_v<Collector>);
static_assert(!std::is_copy_assignable_v<Collector>);
static_assert(!std::is_move_constructible_v<Collector>);
static_assert(!std::is_move_assignable_v<Collector>);
static_assert(std::is_same_v<decltype(&Collector::collect), CollectMember>);
static_assert(std::is_same_v<decltype(&Collector::shutdown), ShutdownMember>);
static_assert(std::is_final_v<LifecycleGate>);
static_assert(std::is_move_constructible_v<LifecycleGate::Lease>);
static_assert(std::is_move_assignable_v<LifecycleGate::Lease>);
static_assert(!std::is_copy_constructible_v<LifecycleGate::Lease>);
static_assert(!std::is_copy_assignable_v<LifecycleGate::Lease>);

class TestClock final : public Contracts::IClock {
public:
    TestClock() noexcept
        : utcNanoseconds_{
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::hours{24}).count()},
          monotonicNanoseconds_{
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::hours{1}).count()}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{
            std::chrono::duration_cast<Domain::UtcTimePoint::duration>(
                std::chrono::nanoseconds{
                    utcNanoseconds_.load(std::memory_order_relaxed)})};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        try {
            std::unique_lock lock{monotonicBlockMutex_};
            if (blockNextMonotonic_) {
                blockNextMonotonic_ = false;
                monotonicBlockEntered_ = true;
                monotonicBlockChanged_.notify_all();
                monotonicBlockChanged_.wait(lock, [this]() noexcept {
                    return monotonicBlockReleased_;
                });
            }
        } catch (...) {
        }
        return Domain::MonotonicTimePoint{
            std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(
                std::chrono::nanoseconds{
                    monotonicNanoseconds_.load(std::memory_order_relaxed)})};
    }

    void advanceUtc(const std::chrono::nanoseconds amount) noexcept
    {
        utcNanoseconds_.fetch_add(amount.count(), std::memory_order_relaxed);
    }

    void advanceMonotonic(const std::chrono::nanoseconds amount) noexcept
    {
        monotonicNanoseconds_.fetch_add(
            amount.count(), std::memory_order_relaxed);
    }

    void blockNextMonotonicNow()
    {
        const std::lock_guard lock{monotonicBlockMutex_};
        blockNextMonotonic_ = true;
        monotonicBlockEntered_ = false;
        monotonicBlockReleased_ = false;
    }

    [[nodiscard]] bool waitForMonotonicBlockEntry(
        const std::chrono::milliseconds timeout) const noexcept
    {
        try {
            std::unique_lock lock{monotonicBlockMutex_};
            return monotonicBlockChanged_.wait_for(
                lock,
                timeout,
                [this]() noexcept { return monotonicBlockEntered_; });
        } catch (...) {
            return false;
        }
    }

    void releaseBlockedMonotonicNow()
    {
        const std::lock_guard lock{monotonicBlockMutex_};
        monotonicBlockReleased_ = true;
        monotonicBlockChanged_.notify_all();
    }

private:
    std::atomic<std::int64_t> utcNanoseconds_;
    std::atomic<std::int64_t> monotonicNanoseconds_;
    mutable std::mutex monotonicBlockMutex_;
    mutable std::condition_variable monotonicBlockChanged_;
    mutable bool blockNextMonotonic_{};
    mutable bool monotonicBlockEntered_{};
    mutable bool monotonicBlockReleased_{};
};

class FakeRamMetricsPlatform final : public Detail::IRamMetricsPlatform {
public:
    FakeRamMetricsPlatform()
    {
        physical.totalBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        physical.availableBytes = 6ULL * 1024ULL * 1024ULL * 1024ULL;
        performance.pageSizeBytes = 4096U;
        performance.committedPages = 1'048'576U;
        performance.commitLimitPages = 2'097'152U;
        performance.pagedPoolPages = 65'536U;
    }

    [[nodiscard]] Domain::Result<Detail::PhysicalMemoryObservation>
    queryPhysicalMemory() noexcept override
    {
        try {
            physicalCalls.fetch_add(1U, std::memory_order_relaxed);
            {
                std::unique_lock lock{blockMutex_};
                physicalEntered_ = true;
                blockChanged_.notify_all();
                blockChanged_.wait(lock, [this]() noexcept {
                    return !blockPhysical_ || physicalReleased_;
                });
            }
            if (afterPhysical) {
                afterPhysical();
            }
            if (physicalFailure) {
                return Domain::Result<Detail::PhysicalMemoryObservation>::failure(
                    *physicalFailure);
            }
            return Domain::Result<Detail::PhysicalMemoryObservation>::success(
                physical);
        } catch (const std::exception& exception) {
            return Domain::Result<Detail::PhysicalMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<Detail::PhysicalMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake physical-memory probe failed."));
        }
    }

    [[nodiscard]] Domain::Result<Detail::PerformanceMemoryObservation>
    queryPerformanceMemory() noexcept override
    {
        try {
            performanceCalls.fetch_add(1U, std::memory_order_relaxed);
            {
                std::unique_lock lock{blockMutex_};
                performanceEntered_ = true;
                blockChanged_.notify_all();
                blockChanged_.wait(lock, [this]() noexcept {
                    return !blockPerformance_ || performanceReleased_;
                });
            }
            if (afterPerformance) {
                afterPerformance();
            }
            if (performanceFailure) {
                return Domain::Result<
                    Detail::PerformanceMemoryObservation>::failure(
                    *performanceFailure);
            }
            return Domain::Result<
                Detail::PerformanceMemoryObservation>::success(performance);
        } catch (const std::exception& exception) {
            return Domain::Result<
                Detail::PerformanceMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<
                Detail::PerformanceMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake performance-memory probe failed."));
        }
    }

    void blockPhysical() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        blockPhysical_ = true;
        physicalReleased_ = false;
        physicalEntered_ = false;
    }

    [[nodiscard]] bool waitForPhysicalEntry(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{blockMutex_};
            return blockChanged_.wait_for(
                lock,
                timeout,
                [this]() noexcept { return physicalEntered_; });
        } catch (...) {
            return false;
        }
    }

    void releasePhysical() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        physicalReleased_ = true;
        blockChanged_.notify_all();
    }

    void blockPerformance() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        blockPerformance_ = true;
        performanceReleased_ = false;
        performanceEntered_ = false;
    }

    [[nodiscard]] bool waitForPerformanceEntry(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{blockMutex_};
            return blockChanged_.wait_for(
                lock,
                timeout,
                [this]() noexcept { return performanceEntered_; });
        } catch (...) {
            return false;
        }
    }

    void releasePerformance() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        performanceReleased_ = true;
        blockChanged_.notify_all();
    }

    Detail::PhysicalMemoryObservation physical;
    Detail::PerformanceMemoryObservation performance;
    std::optional<Domain::Error> physicalFailure;
    std::optional<Domain::Error> performanceFailure;
    std::function<void()> afterPhysical;
    std::function<void()> afterPerformance;
    std::atomic<std::size_t> physicalCalls{};
    std::atomic<std::size_t> performanceCalls{};

private:
    std::mutex blockMutex_;
    std::condition_variable blockChanged_;
    bool blockPhysical_{};
    bool physicalEntered_{};
    bool physicalReleased_{};
    bool blockPerformance_{};
    bool performanceEntered_{};
    bool performanceReleased_{};
};

[[nodiscard]] Domain::OperationContext activeContext(
    const TestClock& clock,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "71000000-0000-4000-8000-000000000001"),
        clock.monotonicNow() + 5min,
        cancellation,
        parse<Domain::CorrelationId>("p17-ram-collector-test")};
}

[[nodiscard]] Domain::OperationContext expiringContext(
    const TestClock& clock,
    const std::chrono::nanoseconds lifetime,
    const std::stop_token cancellation = {})
{
    auto context = activeContext(clock, cancellation);
    context.deadline = clock.monotonicNow() + lifetime;
    return context;
}

[[nodiscard]] std::unique_ptr<Collector> createCollector(
    TestClock& clock,
    const std::shared_ptr<Detail::IRamMetricsPlatform>& platform)
{
    return Detail::WindowsRamMetricsCollectorTestAccess::create(
        clock, platform);
}

[[nodiscard]] bool waitForClosedAdmission(
    Collector& collector,
    const Domain::OperationContext& context,
    const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto probe = collector.collect(context);
        if (!probe && probe.error().code == Domain::ErrorCodes::TransportClosed) {
            return true;
        }
        requireError(
            probe,
            Domain::ErrorCodes::LimitExceeded,
            "shutdown-drain observation admitted a second collection");
        std::this_thread::yield();
    }
    return false;
}

template <typename T>
void requireAvailable(
    const Domain::TelemetryMetric<T>& metric,
    const T& expected,
    const std::string_view message)
{
    require(
        metric.availability == Availability::Available &&
            !metric.stale &&
            metric.value.has_value() &&
            metric.value.value() == expected &&
            metric.capturedAt.has_value() &&
            metric.observedAt.has_value() &&
            metric.capturedAt == metric.observedAt &&
            !metric.source.empty() &&
            !metric.unavailableReason.has_value(),
        message);
}

template <typename T>
void requireUnsupported(
    const Domain::TelemetryMetric<T>& metric,
    const std::string_view message)
{
    require(
        metric.availability == Availability::Unsupported &&
            !metric.stale &&
            !metric.value.has_value() &&
            !metric.source.empty() &&
            metric.unavailableReason.has_value(),
        message);
}

void mapsVerifiedWindowsSourcesAndUnsupportedFields()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    auto collector = createCollector(clock, platform);

    const auto metrics = take(collector->collect(activeContext(clock)));
    const auto gibibyte = 1024ULL * 1024ULL * 1024ULL;

    requireAvailable(
        metrics.totalBytes, 16ULL * gibibyte,
        "GlobalMemoryStatusEx total bytes were not mapped");
    requireAvailable(
        metrics.availableBytes, 6ULL * gibibyte,
        "GlobalMemoryStatusEx available bytes were not mapped");
    requireAvailable(
        metrics.usedBytes, 10ULL * gibibyte,
        "physical used bytes were not derived with checked subtraction");
    requireAvailable(
        metrics.percent, 62.5,
        "physical utilization was not derived from total and available bytes");
    requireAvailable(
        metrics.pressurePercent, 50.0,
        "GetPerformanceInfo commit pressure was not mapped");
    requireAvailable(
        metrics.committedBytes, 4ULL * gibibyte,
        "GetPerformanceInfo committed pages were not converted to bytes");
    requireAvailable(
        metrics.pagedPoolBytes, 256ULL * 1024ULL * 1024ULL,
        "GetPerformanceInfo paged-pool pages were not converted to bytes");
    require(
        metrics.totalBytes.source == "GlobalMemoryStatusEx.ullTotalPhys" &&
            metrics.availableBytes.source ==
                "GlobalMemoryStatusEx.ullAvailPhys" &&
            metrics.usedBytes.source ==
                "GlobalMemoryStatusEx total minus available" &&
            metrics.percent.source ==
                "GlobalMemoryStatusEx physical utilization" &&
            metrics.pressurePercent.source ==
                "GetPerformanceInfo.CommitTotal / CommitLimit" &&
            metrics.committedBytes.source ==
                "GetPerformanceInfo.CommitTotal * PageSize" &&
            metrics.pagedPoolBytes.source ==
                "GetPerformanceInfo.KernelPaged * PageSize",
        "RAM values did not identify their exact verified Windows sources");

    requireUnsupported(
        metrics.activeBytes,
        "active bytes were synthesized instead of reported unsupported");
    requireUnsupported(
        metrics.wiredBytes,
        "wired bytes were synthesized instead of reported unsupported");
    requireUnsupported(
        metrics.compressedBytes,
        "compressed bytes were synthesized instead of reported unsupported");
    require(
        metrics.compressedBytes.source ==
            "PDH \\Memory\\Compressed Page Count",
        "compressed bytes did not identify the intended absent PDH source");
    requireUnsupported(
        metrics.swapTotalBytes,
        "swap total was synthesized instead of reported unsupported");
    requireUnsupported(
        metrics.swapUsedBytes,
        "swap used was synthesized instead of reported unsupported");
    requireUnsupported(
        metrics.swapPercent,
        "swap utilization was synthesized instead of reported unsupported");
    require(
        metrics.activeBytes.source ==
                "Windows memory model: no active-byte equivalent" &&
            metrics.wiredBytes.source ==
                "Windows memory model: no wired-byte equivalent" &&
            metrics.swapTotalBytes.source ==
                "Windows memory model: no equivalent swap metric" &&
            metrics.swapUsedBytes.source ==
                "Windows memory model: no equivalent swap metric" &&
            metrics.swapPercent.source ==
                "Windows memory model: no equivalent swap metric",
        "unsupported RAM fields did not retain their exact semantic sources");
    require(
        Domain::validateRamMetrics(metrics).hasValue(),
        "the mapped fixture did not satisfy the typed RAM metric invariant");
}

void partialProbeFailurePreservesSuccessfulCategories()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    platform->physicalFailure = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "GlobalMemoryStatusEx access was denied.");
    auto collector = createCollector(clock, platform);

    const auto metrics = take(collector->collect(activeContext(clock)));

    require(
        metrics.totalBytes.availability == Availability::AccessDenied &&
            !metrics.totalBytes.value && !metrics.totalBytes.stale &&
            metrics.totalBytes.unavailableReason ==
                "GlobalMemoryStatusEx access was denied.",
        "a first physical-probe denial was not represented without a value");
    requireAvailable(
        metrics.pressurePercent, 50.0,
        "a physical-probe denial discarded successful performance metrics");
    requireAvailable(
        metrics.committedBytes, 4ULL * 1024ULL * 1024ULL * 1024ULL,
        "a physical-probe denial discarded committed bytes");
    require(
        platform->physicalCalls.load() == 1U &&
            platform->performanceCalls.load() == 1U,
        "a partial native failure prevented the independent probe from running");
}

void failedRefreshRetainsPriorValueAsStale()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    auto collector = createCollector(clock, platform);
    const auto initial = take(collector->collect(activeContext(clock)));

    platform->physicalFailure = Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "GlobalMemoryStatusEx refresh failed.",
        true);
    platform->performanceFailure = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "GetPerformanceInfo is unavailable.");
    clock.advanceUtc(30s);
    const auto failureObservedAt = clock.utcNow();

    const auto stale = take(collector->collect(activeContext(clock)));

    require(
        stale.totalBytes.value == initial.totalBytes.value &&
            stale.totalBytes.capturedAt == initial.totalBytes.capturedAt &&
            stale.totalBytes.source == initial.totalBytes.source &&
            stale.totalBytes.observedAt.has_value() &&
            *stale.totalBytes.observedAt == failureObservedAt &&
            stale.totalBytes.stale &&
            stale.totalBytes.availability ==
                Availability::TemporarilyUnavailable &&
            stale.totalBytes.unavailableReason ==
                "GlobalMemoryStatusEx refresh failed.",
        "a failed physical refresh did not retain the last successful value and provenance");
    require(
        stale.committedBytes.value == initial.committedBytes.value &&
            stale.committedBytes.capturedAt ==
                initial.committedBytes.capturedAt &&
            stale.committedBytes.source == initial.committedBytes.source &&
            stale.committedBytes.observedAt.has_value() &&
            *stale.committedBytes.observedAt == failureObservedAt &&
            stale.committedBytes.stale &&
            stale.committedBytes.availability == Availability::Unsupported &&
            stale.committedBytes.unavailableReason ==
                "GetPerformanceInfo is unavailable.",
        "a failed performance refresh did not retain its prior value as stale");
    require(
        stale.compressedBytes.availability == Availability::Unsupported &&
            !stale.compressedBytes.stale && !stale.compressedBytes.value,
        "a permanently unsupported metric incorrectly acquired stale state");
    require(
        Domain::validateRamMetrics(stale).hasValue(),
        "the retained stale fixture violated the typed RAM metric invariant");
}

void successfulZeroesRemainAvailable()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    platform->physical.availableBytes = platform->physical.totalBytes;
    platform->performance.committedPages = 0U;
    platform->performance.pagedPoolPages = 0U;
    auto collector = createCollector(clock, platform);

    const auto metrics = take(collector->collect(activeContext(clock)));

    requireAvailable(
        metrics.usedBytes,
        std::uint64_t{0U},
        "a successful physical-memory zero was treated as unavailable");
    requireAvailable(
        metrics.percent,
        0.0,
        "a successful physical-utilization zero was treated as unavailable");
    requireAvailable(
        metrics.pressurePercent,
        0.0,
        "a successful commit-pressure zero was treated as unavailable");
    requireAvailable(
        metrics.committedBytes,
        std::uint64_t{0U},
        "a successful committed-byte zero was treated as unavailable");
    requireAvailable(
        metrics.pagedPoolBytes,
        std::uint64_t{0U},
        "a successful paged-pool zero was treated as unavailable");
}

void cancellationAndDeadlinesGateEveryNativeBlock()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    auto collector = createCollector(clock, platform);

    std::stop_source preCancelled;
    preCancelled.request_stop();
    requireError(
        collector->collect(activeContext(clock, preCancelled.get_token())),
        Domain::ErrorCodes::Cancelled,
        "a pre-cancelled RAM collection reached the platform");
    require(
        platform->physicalCalls.load() == 0U,
        "the pre-cancelled collection invoked GlobalMemoryStatusEx");

    requireError(
        collector->collect(expiringContext(clock, 0ns)),
        Domain::ErrorCodes::DeadlineExceeded,
        "an expired RAM collection reached the platform");
    require(
        platform->physicalCalls.load() == 0U,
        "the expired collection invoked GlobalMemoryStatusEx");

    platform->afterPhysical = [&clock]() noexcept {
        clock.advanceMonotonic(2min);
    };
    requireError(
        collector->collect(expiringContext(clock, 1min)),
        Domain::ErrorCodes::DeadlineExceeded,
        "a deadline crossed inside the physical API block was ignored");
    require(
        platform->physicalCalls.load() == 1U &&
            platform->performanceCalls.load() == 0U,
        "the performance probe ran after the physical block crossed its deadline");

    platform->afterPhysical = {};
    std::stop_source duringPhysical;
    platform->afterPhysical = [&duringPhysical]() noexcept {
        duringPhysical.request_stop();
    };
    requireError(
        collector->collect(
            activeContext(clock, duringPhysical.get_token())),
        Domain::ErrorCodes::Cancelled,
        "cancellation requested inside the physical API block was ignored");
    require(
        platform->physicalCalls.load() == 2U &&
            platform->performanceCalls.load() == 0U,
        "the performance probe ran after cancellation of the physical block");
}

void overlappingCollectionIsRejectedWithoutAQueue()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    platform->blockPhysical();
    auto collector = createCollector(clock, platform);
    const auto context = activeContext(clock);

    auto first = std::async(
        std::launch::async,
        [&collector, context]() { return collector->collect(context); });
    const bool entered = platform->waitForPhysicalEntry(2s);
    if (!entered) {
        platform->releasePhysical();
        static_cast<void>(first.get());
        require(false, "the overlap fixture did not enter its physical probe");
    }
    const auto overlapping = collector->collect(context);
    platform->releasePhysical();
    const auto completed = first.get();

    requireError(
        overlapping,
        Domain::ErrorCodes::LimitExceeded,
        "the synchronous RAM collector queued an overlapping operation");
    require(
        completed.hasValue(),
        "the admitted RAM collection did not complete after release");
    require(
        platform->physicalCalls.load() == 1U &&
            platform->performanceCalls.load() == 1U,
        "the rejected overlap reached a native probe");
}

void publicationAlreadyInProgressLinearizesBeforeShutdown()
{
    TestClock clock;
    LifecycleGate lifecycle{clock, "test telemetry collection"};
    const auto context = activeContext(clock);

    auto admission = lifecycle.tryAcquire(context);
    require(
        admission.hasValue(),
        "the shared lifecycle gate rejected its first operation");
    std::optional<LifecycleGate::Lease> lease{
        std::move(admission).value()};

    std::mutex publicationMutex;
    std::condition_variable publicationChanged;
    bool publicationEntered{};
    bool publicationReleased{};
    bool publicationCompleted{};

    auto publishing = std::async(
        std::launch::async,
        [&]() {
            return lifecycle.publish(
                context,
                "publish test metrics",
                [&]() {
                    std::unique_lock lock{publicationMutex};
                    publicationEntered = true;
                    publicationChanged.notify_all();
                    publicationChanged.wait(lock, [&]() noexcept {
                        return publicationReleased;
                    });
                    publicationCompleted = true;
                });
        });

    bool entered{};
    {
        std::unique_lock lock{publicationMutex};
        entered = publicationChanged.wait_for(
            lock,
            2s,
            [&]() noexcept { return publicationEntered; });
    }
    if (!entered) {
        {
            const std::lock_guard lock{publicationMutex};
            publicationReleased = true;
        }
        publicationChanged.notify_all();
        static_cast<void>(publishing.get());
        lease.reset();
        lifecycle.shutdownAndDrain();
        require(false, "the publication fixture did not enter its callback");
    }

    std::promise<void> shutdownInvoked;
    auto shutdownStarted = shutdownInvoked.get_future();
    auto stopping = std::async(
        std::launch::async,
        [&lifecycle, signal = std::move(shutdownInvoked)]() mutable {
            signal.set_value();
            lifecycle.shutdownAndDrain();
        });
    shutdownStarted.get();
    const bool shutdownWaitedForPublication =
        stopping.wait_for(0ms) == std::future_status::timeout;

    {
        const std::lock_guard lock{publicationMutex};
        publicationReleased = true;
    }
    publicationChanged.notify_all();
    const auto published = publishing.get();
    const bool shutdownStillDrainingLease =
        stopping.wait_for(0ms) == std::future_status::timeout;
    lease.reset();
    const bool shutdownCompletedAfterRelease =
        stopping.wait_for(2s) == std::future_status::ready;
    stopping.get();

    lifecycle.shutdownAndDrain();
    requireError(
        lifecycle.tryAcquire(context),
        Domain::ErrorCodes::TransportClosed,
        "the shared lifecycle gate admitted work after shutdown");
    require(
        published.hasValue() && publicationCompleted,
        "publication did not finish after winning lifecycle linearization");
    require(
        shutdownWaitedForPublication,
        "shutdown completed while a publication callback held lifecycle ownership");
    require(
        shutdownStillDrainingLease,
        "shutdown completed before the admitted operation released its lease");
    require(
        shutdownCompletedAfterRelease,
        "shutdown did not complete after the publication and lease were released");
}

void shutdownIsIdempotentAndClosesAdmission()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    platform->blockPhysical();
    auto collector = createCollector(clock, platform);
    const auto context = activeContext(clock);

    auto active = std::async(
        std::launch::async,
        [&collector, context]() { return collector->collect(context); });
    const bool entered = platform->waitForPhysicalEntry(2s);

    auto stopping = std::async(
        std::launch::async,
        [&collector]() { collector->shutdown(); });
    const bool closedObserved =
        waitForClosedAdmission(*collector, context, 2s);
    const bool shutdownWaitedForActiveRelease =
        stopping.wait_for(0ms) == std::future_status::timeout;
    platform->releasePhysical();
    const auto interrupted = active.get();
    const bool shutdownCompletedAfterActiveRelease =
        stopping.wait_for(2s) == std::future_status::ready;
    stopping.get();
    collector->shutdown();
    requireError(
        collector->collect(context),
        Domain::ErrorCodes::TransportClosed,
        "the RAM collector admitted work after shutdown");

    require(entered, "the shutdown fixture did not enter its physical probe");
    require(closedObserved, "shutdown did not close admission before draining");
    require(
        shutdownWaitedForActiveRelease,
        "shutdown returned before its admitted collection released lifecycle ownership");
    require(
        shutdownCompletedAfterActiveRelease,
        "shutdown did not complete after its admitted collection released lifecycle ownership");
    requireError(
        interrupted,
        Domain::ErrorCodes::TransportClosed,
        "shutdown allowed an active collection to publish after its native block");
    require(
        platform->physicalCalls.load() == 1U &&
            platform->performanceCalls.load() == 0U,
        "shutdown did not prevent later native blocks and post-shutdown admission");
}

void shutdownLinearizesBeforeFinalPublication()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    platform->blockPerformance();
    auto collector = createCollector(clock, platform);
    const auto context = activeContext(clock);

    auto active = std::async(
        std::launch::async,
        [&collector, context]() { return collector->collect(context); });
    const bool entered = platform->waitForPerformanceEntry(2s);
    auto stopping = std::async(
        std::launch::async,
        [&collector]() { collector->shutdown(); });

    const bool closedObserved =
        waitForClosedAdmission(*collector, context, 2s);
    const bool shutdownWaitedForActiveRelease =
        stopping.wait_for(0ms) == std::future_status::timeout;

    platform->releasePerformance();
    const auto interrupted = active.get();
    const bool shutdownCompletedAfterActiveRelease =
        stopping.wait_for(2s) == std::future_status::ready;
    stopping.get();

    require(entered, "the final-publication fixture did not enter its last probe");
    require(closedObserved, "shutdown did not close admission before draining");
    require(
        shutdownWaitedForActiveRelease,
        "shutdown returned before the pre-publication collection released lifecycle ownership");
    require(
        shutdownCompletedAfterActiveRelease,
        "shutdown did not complete after the pre-publication collection released lifecycle ownership");
    requireError(
        interrupted,
        Domain::ErrorCodes::TransportClosed,
        "a collection published successfully after shutdown began draining it");
}

void shutdownLinearizesBeforeLifecycleAdmission()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    auto collector = createCollector(clock, platform);
    const auto context = activeContext(clock);
    clock.blockNextMonotonicNow();

    auto collecting = std::async(
        std::launch::async,
        [&collector, context]() { return collector->collect(context); });
    const bool pausedBeforeAdmission = clock.waitForMonotonicBlockEntry(2s);

    collector->shutdown();
    clock.releaseBlockedMonotonicNow();
    const auto rejected = collecting.get();

    require(
        pausedBeforeAdmission,
        "the pre-admission fixture did not pause inside context validation");
    requireError(
        rejected,
        Domain::ErrorCodes::TransportClosed,
        "a collection crossed lifecycle admission after shutdown returned");
    require(
        platform->physicalCalls.load() == 0U &&
            platform->performanceCalls.load() == 0U,
        "a pre-admission shutdown race reached a native memory probe");
}

void invalidAndOverflowingRawValuesAreNotSynthesized()
{
    TestClock clock;
    auto platform = std::make_shared<FakeRamMetricsPlatform>();
    platform->physical = Detail::PhysicalMemoryObservation{0U, 1U};
    platform->performance = Detail::PerformanceMemoryObservation{
        std::numeric_limits<std::uint64_t>::max(),
        2U,
        4U,
        2U};
    auto collector = createCollector(clock, platform);

    const auto metrics = take(collector->collect(activeContext(clock)));

    require(
        !metrics.totalBytes.value && !metrics.availableBytes.value &&
            !metrics.usedBytes.value && !metrics.percent.value &&
            metrics.totalBytes.availability ==
                Availability::TemporarilyUnavailable,
        "incoherent physical values were projected as measured zeroes");
    requireAvailable(
        metrics.pressurePercent, 50.0,
        "a valid commit ratio was discarded with independent byte overflows");
    require(
        !metrics.committedBytes.value &&
            metrics.committedBytes.availability ==
                Availability::TemporarilyUnavailable &&
            !metrics.pagedPoolBytes.value &&
            metrics.pagedPoolBytes.availability ==
                Availability::TemporarilyUnavailable,
        "overflowing page-to-byte products were emitted as values");
    require(
        Domain::validateRamMetrics(metrics).hasValue(),
        "invalid raw data escaped into an invalid RAM domain object");

    platform->physicalFailure = Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::string(Domain::TelemetryMetricReasonBytesMaximum + 1U, 'x'));
    const auto bounded = take(collector->collect(activeContext(clock)));
    require(
        bounded.totalBytes.unavailableReason.has_value() &&
            bounded.totalBytes.unavailableReason->size() <=
                Domain::TelemetryMetricReasonBytesMaximum,
        "an oversized platform reason crossed the telemetry domain boundary");
}

void productionPlatformSmokeMatchesTheQualifiedMachine()
{
    TestClock clock;
    Collector collector{clock};
    const auto metrics = take(collector.collect(activeContext(clock)));

    require(
        metrics.totalBytes.availability == Availability::Available &&
            metrics.totalBytes.value.value_or(0U) > 0U &&
            metrics.availableBytes.availability == Availability::Available &&
            metrics.usedBytes.availability == Availability::Available &&
            metrics.percent.availability == Availability::Available,
        "GlobalMemoryStatusEx did not produce physical RAM metrics on the qualified machine");
    require(
        metrics.totalBytes.source == "GlobalMemoryStatusEx.ullTotalPhys" &&
            metrics.pressurePercent.source ==
                "GetPerformanceInfo.CommitTotal / CommitLimit" &&
            metrics.pagedPoolBytes.source ==
                "GetPerformanceInfo.KernelPaged * PageSize",
        "the production RAM probes did not retain verified source identities");
    require(
        metrics.pressurePercent.availability == Availability::Available &&
            metrics.committedBytes.availability == Availability::Available &&
            metrics.pagedPoolBytes.availability == Availability::Available,
        "GetPerformanceInfo did not produce commit metrics on the qualified machine");
    requireUnsupported(
        metrics.compressedBytes,
        "the absent qualified-machine compressed-memory counter was not explicit");
    require(
        Domain::validateRamMetrics(metrics).hasValue(),
        "the production Windows probes produced an invalid RAM snapshot");
}

} // namespace

void registerWindowsRamMetricsCollectorTests(TestRegistry& tests)
{
    addTest(
        tests,
        "telemetry_windows.ram.mapping",
        mapsVerifiedWindowsSourcesAndUnsupportedFields);
    addTest(
        tests,
        "telemetry_windows.ram.partial-failure",
        partialProbeFailurePreservesSuccessfulCategories);
    addTest(
        tests,
        "telemetry_windows.ram.stale-retention",
        failedRefreshRetainsPriorValueAsStale);
    addTest(
        tests,
        "telemetry_windows.ram.successful-zero",
        successfulZeroesRemainAvailable);
    addTest(
        tests,
        "telemetry_windows.ram.context",
        cancellationAndDeadlinesGateEveryNativeBlock);
    addTest(
        tests,
        "telemetry_windows.ram.overlap",
        overlappingCollectionIsRejectedWithoutAQueue);
    addTest(
        tests,
        "telemetry_windows.ram.lifecycle-publication-wins",
        publicationAlreadyInProgressLinearizesBeforeShutdown);
    addTest(
        tests,
        "telemetry_windows.ram.shutdown",
        shutdownIsIdempotentAndClosesAdmission);
    addTest(
        tests,
        "telemetry_windows.ram.shutdown-final-publication",
        shutdownLinearizesBeforeFinalPublication);
    addTest(
        tests,
        "telemetry_windows.ram.shutdown-pre-admission",
        shutdownLinearizesBeforeLifecycleAdmission);
    addTest(
        tests,
        "telemetry_windows.ram.invalid-raw",
        invalidAndOverflowingRawValuesAreNotSynthesized);
    addTest(
        tests,
        "telemetry_windows.ram.machine-smoke",
        productionPlatformSmokeMatchesTheQualifiedMachine);
}

} // namespace ForgeConductor::Tests
