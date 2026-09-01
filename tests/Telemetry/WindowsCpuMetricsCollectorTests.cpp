#include "ForgeConductor/Telemetry/Windows/WindowsCpuMetricsCollector.h"
#include "Infrastructure/TestSupport.h"
#include "Telemetry/Windows/Detail/ICpuMetricsPlatform.h"

// This contract scaffold remains out of the CMake test target until the native
// Windows CPU collector implementation is added in the next P17 slice.

#include <atomic>
#include <chrono>
#include <cmath>
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
#include <vector>

namespace ForgeConductor::Telemetry::Windows::Detail {

struct WindowsCpuMetricsCollectorTestAccess final {
    [[nodiscard]] static std::unique_ptr<WindowsCpuMetricsCollector> create(
        Contracts::IClock& clock,
        std::shared_ptr<ICpuMetricsPlatform> platform)
    {
        return std::unique_ptr<WindowsCpuMetricsCollector>{
            new WindowsCpuMetricsCollector{clock, std::move(platform)}};
    }
};

} // namespace ForgeConductor::Telemetry::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

namespace TelemetryWindows = Telemetry::Windows;
namespace Detail = Telemetry::Windows::Detail;

using Collector = TelemetryWindows::WindowsCpuMetricsCollector;
using Interface = Contracts::ICpuMetricsCollector;
using Availability = Domain::TelemetryMetricAvailability;
using namespace std::chrono_literals;

using CollectMember = Domain::Result<Domain::CpuMetrics> (
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

class FakeCpuMetricsPlatform final : public Detail::ICpuMetricsPlatform {
public:
    FakeCpuMetricsPlatform()
    {
        systemTimes = Detail::CpuTimesObservation{1'000U, 2'000U, 1'000U};
        topology = Detail::CpuTopologyObservation{
            4U,
            2U,
            {{1U, 3U}, {0U, 10U}, {0U, 2U}, {1U, 0U}}};
        brand = "AMD Ryzen 9 7950X 16-Core Processor";
        performance = readyPerformance();
    }

    [[nodiscard]] static Detail::CpuPerformanceObservation readyPerformance()
    {
        return Detail::CpuPerformanceObservation{
            true,
            {{1U, 3U, 44.0},
             {0U, 10U, 22.0},
             {1U, 0U, 33.0},
             {0U, 2U, 11.0}},
            std::nullopt,
            4'250.0,
            std::nullopt,
            {{1U, 3U, 4'400.0},
             {0U, 10U, 4'200.0},
             {1U, 0U, 4'300.0},
             {0U, 2U, 4'100.0}},
            std::nullopt};
    }

    [[nodiscard]] Domain::Result<Detail::CpuTimesObservation>
    querySystemTimes() noexcept override
    {
        try {
            systemTimesCalls.fetch_add(1U, std::memory_order_relaxed);
            {
                std::unique_lock lock{blockMutex_};
                systemTimesEntered_ = true;
                blockChanged_.notify_all();
                blockChanged_.wait(lock, [this]() noexcept {
                    return !blockSystemTimes_ || systemTimesReleased_;
                });
            }
            if (afterSystemTimes) {
                afterSystemTimes();
            }
            if (systemTimesFailure) {
                return Domain::Result<Detail::CpuTimesObservation>::failure(
                    *systemTimesFailure);
            }
            return Domain::Result<Detail::CpuTimesObservation>::success(
                systemTimes);
        } catch (const std::exception& exception) {
            return Domain::Result<Detail::CpuTimesObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<Detail::CpuTimesObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake system-times probe failed."));
        }
    }

    [[nodiscard]] Domain::Result<Detail::CpuTopologyObservation>
    queryTopology() noexcept override
    {
        try {
            topologyCalls.fetch_add(1U, std::memory_order_relaxed);
            if (afterTopology) {
                afterTopology();
            }
            if (topologyFailure) {
                return Domain::Result<Detail::CpuTopologyObservation>::failure(
                    *topologyFailure);
            }
            return Domain::Result<Detail::CpuTopologyObservation>::success(
                topology);
        } catch (const std::exception& exception) {
            return Domain::Result<Detail::CpuTopologyObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<Detail::CpuTopologyObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake topology probe failed."));
        }
    }

    [[nodiscard]] Domain::Result<std::string> queryBrand() noexcept override
    {
        try {
            brandCalls.fetch_add(1U, std::memory_order_relaxed);
            if (afterBrand) {
                afterBrand();
            }
            if (brandFailure) {
                return Domain::Result<std::string>::failure(*brandFailure);
            }
            return Domain::Result<std::string>::success(brand);
        } catch (const std::exception& exception) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                exception.what()));
        } catch (...) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The fake processor-brand probe failed."));
        }
    }

    [[nodiscard]] Domain::Result<Detail::CpuPerformanceObservation>
    queryProcessorPerformance() noexcept override
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
                    Detail::CpuPerformanceObservation>::failure(
                    *performanceFailure);
            }
            return Domain::Result<
                Detail::CpuPerformanceObservation>::success(performance);
        } catch (const std::exception& exception) {
            return Domain::Result<
                Detail::CpuPerformanceObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<
                Detail::CpuPerformanceObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake processor-performance probe failed."));
        }
    }

    void shutdown() noexcept override
    {
        shutdownCalls.fetch_add(1U, std::memory_order_relaxed);
    }

    void blockSystemTimes() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        blockSystemTimes_ = true;
        systemTimesEntered_ = false;
        systemTimesReleased_ = false;
    }

    [[nodiscard]] bool waitForSystemTimesEntry(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{blockMutex_};
            return blockChanged_.wait_for(
                lock,
                timeout,
                [this]() noexcept { return systemTimesEntered_; });
        } catch (...) {
            return false;
        }
    }

    void releaseSystemTimes() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        systemTimesReleased_ = true;
        blockChanged_.notify_all();
    }

    void blockPerformance() noexcept
    {
        const std::lock_guard lock{blockMutex_};
        blockPerformance_ = true;
        performanceEntered_ = false;
        performanceReleased_ = false;
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

    Detail::CpuTimesObservation systemTimes;
    Detail::CpuTopologyObservation topology;
    std::string brand;
    Detail::CpuPerformanceObservation performance;
    std::optional<Domain::Error> systemTimesFailure;
    std::optional<Domain::Error> topologyFailure;
    std::optional<Domain::Error> brandFailure;
    std::optional<Domain::Error> performanceFailure;
    std::function<void()> afterSystemTimes;
    std::function<void()> afterTopology;
    std::function<void()> afterBrand;
    std::function<void()> afterPerformance;
    std::atomic<std::size_t> systemTimesCalls{};
    std::atomic<std::size_t> topologyCalls{};
    std::atomic<std::size_t> brandCalls{};
    std::atomic<std::size_t> performanceCalls{};
    std::atomic<std::size_t> shutdownCalls{};

private:
    std::mutex blockMutex_;
    std::condition_variable blockChanged_;
    bool blockSystemTimes_{};
    bool systemTimesEntered_{};
    bool systemTimesReleased_{};
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
            "72000000-0000-4000-8000-000000000001"),
        clock.monotonicNow() + 5min,
        cancellation,
        parse<Domain::CorrelationId>("p17-cpu-collector-test")};
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
    const std::shared_ptr<Detail::ICpuMetricsPlatform>& platform)
{
    return Detail::WindowsCpuMetricsCollectorTestAccess::create(
        clock, platform);
}

[[nodiscard]] bool approximatelyEqual(
    const double actual,
    const double expected,
    const double tolerance = 1.0e-9) noexcept
{
    return std::isfinite(actual) &&
        std::abs(actual - expected) <= tolerance;
}

template <typename T>
void requireAvailable(
    const Domain::TelemetryMetric<T>& metric,
    const T& expected,
    const std::string_view message)
{
    require(
        metric.availability == Availability::Available &&
            metric.value.has_value() && *metric.value == expected &&
            !metric.stale && metric.capturedAt.has_value() &&
            metric.observedAt == metric.capturedAt &&
            !metric.unavailableReason,
        message);
}

void requireAvailableNear(
    const Domain::TelemetryMetric<double>& metric,
    const double expected,
    const std::string_view message)
{
    require(
        metric.availability == Availability::Available &&
            metric.value.has_value() &&
            approximatelyEqual(*metric.value, expected) &&
            !metric.stale && metric.capturedAt.has_value() &&
            metric.observedAt == metric.capturedAt &&
            !metric.unavailableReason,
        message);
}

template <typename T>
void requireUnavailable(
    const Domain::TelemetryMetric<T>& metric,
    const Availability availability,
    const std::string_view message)
{
    require(
        metric.availability == availability && !metric.value &&
            !metric.stale && !metric.capturedAt && metric.observedAt &&
            metric.unavailableReason && !metric.unavailableReason->empty(),
        message);
}

template <typename T>
void requireStaleFrom(
    const Domain::TelemetryMetric<T>& current,
    const Domain::TelemetryMetric<T>& previous,
    const Availability availability,
    const Domain::UtcTimePoint failureObservedAt,
    const std::string_view message)
{
    require(
        current.availability == availability && current.stale &&
            current.value == previous.value &&
            current.capturedAt == previous.capturedAt &&
            current.observedAt == failureObservedAt &&
            current.source == previous.source &&
            current.unavailableReason &&
            !current.unavailableReason->empty(),
        message);
}

[[nodiscard]] bool allCallsEqual(
    const FakeCpuMetricsPlatform& platform,
    const std::size_t systemTimes,
    const std::size_t topology,
    const std::size_t brand,
    const std::size_t performance) noexcept
{
    return platform.systemTimesCalls.load(std::memory_order_relaxed) ==
            systemTimes &&
        platform.topologyCalls.load(std::memory_order_relaxed) == topology &&
        platform.brandCalls.load(std::memory_order_relaxed) == brand &&
        platform.performanceCalls.load(std::memory_order_relaxed) == performance;
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
            "shutdown-drain observation admitted a second CPU collection");
        std::this_thread::yield();
    }
    return false;
}

void pdhProcessorInstanceParserIsStrictAndNumeric()
{
    const auto systemTotal = take(Detail::parsePdhProcessorInstanceName(L"_Total"));
    require(
        systemTotal.kind == Detail::PdhProcessorInstanceKind::SystemTotal,
        "the PDH parser did not recognize the global total");

    const auto groupTotal = take(
        Detail::parsePdhProcessorInstanceName(L"12,_Total"));
    require(
        groupTotal.kind == Detail::PdhProcessorInstanceKind::GroupTotal &&
            groupTotal.group == 12U,
        "the PDH parser did not recognize a numeric group total");

    const auto logical = take(Detail::parsePdhProcessorInstanceName(L"2,10"));
    require(
        logical.kind == Detail::PdhProcessorInstanceKind::LogicalProcessor &&
            logical.group == 2U && logical.processor == 10U,
        "the PDH parser did not preserve numeric processor identity");

    const std::wstring_view invalid[] = {
        L"",
        L"0",
        L"0,",
        L",0",
        L"0,0,1",
        L"-1,0",
        L"0,-1",
        L"+1,0",
        L" 0,0",
        L"0,0 ",
        L"group,0",
        L"0,processor",
        L"65536,0",
        L"0,65536",
        L"_Total,0",
        L"_Total,_Total"};
    for (const auto name : invalid) {
        requireError(
            Detail::parsePdhProcessorInstanceName(name),
            Domain::ErrorCodes::InvalidRequest,
            "the PDH parser accepted a malformed processor instance");
    }
}

void firstSampleWarmsRatesWithoutSynthesizingZeroes()
{
    TestClock clock;
    auto platform = std::make_shared<FakeCpuMetricsPlatform>();
    platform->performance.utilizationReady = false;
    platform->performance.perLogicalUtilization.clear();
    auto collector = createCollector(clock, platform);

    const auto metrics = take(collector->collect(activeContext(clock)));

    requireUnavailable(
        metrics.percent,
        Availability::WarmingUp,
        "the first GetSystemTimes sample synthesized aggregate utilization");
    requireUnavailable(
        metrics.userPercent,
        Availability::WarmingUp,
        "the first GetSystemTimes sample synthesized user utilization");
    requireUnavailable(
        metrics.systemPercent,
        Availability::WarmingUp,
        "the first GetSystemTimes sample synthesized system utilization");
    requireUnavailable(
        metrics.idlePercent,
        Availability::WarmingUp,
        "the first GetSystemTimes sample synthesized idle utilization");
    requireUnavailable(
        metrics.perLogicalProcessor,
        Availability::WarmingUp,
        "the first PDH rate sample synthesized per-logical utilization");

    requireAvailable(
        metrics.logicalProcessorCount,
        std::uint32_t{4U},
        "the first sample discarded the logical processor count");
    requireAvailable(
        metrics.physicalCoreCount,
        std::uint32_t{2U},
        "the first sample discarded the physical core count");
    requireAvailable(
        metrics.frequencyMhz,
        std::uint32_t{4'250U},
        "instantaneous aggregate Actual Frequency did not publish during rate warm-up");
    requireAvailable(
        metrics.perCoreFrequencyMhz,
        std::vector<std::uint32_t>{4'100U, 4'200U, 4'300U, 4'400U},
        "instantaneous per-logical Actual Frequency did not sort numerically");
    requireAvailable(
        metrics.brand,
        std::string{"AMD Ryzen 9 7950X 16-Core Processor"},
        "the bounded registry processor brand did not publish during warm-up");
    requireUnavailable(
        metrics.loadAverage,
        Availability::Unsupported,
        "Windows load average was synthesized instead of explicit unsupported state");
    require(
        metrics.loadAverage.source ==
            "Windows scheduling model: no 1/5/15 load-average equivalent",
        "load-average provenance did not describe the Windows semantic gap");
    require(
        Domain::validateCpuMetrics(metrics).hasValue(),
        "the first CPU snapshot violated the typed CPU metric invariant");
    require(
        allCallsEqual(*platform, 1U, 1U, 1U, 1U),
        "the first CPU sample did not query each independent native category once");
}

void systemTimesDeltasSortNumericIdentitiesAndRebaseRegression()
{
    TestClock clock;
    auto platform = std::make_shared<FakeCpuMetricsPlatform>();
    platform->performance.utilizationReady = false;
    platform->performance.perLogicalUtilization.clear();
    auto collector = createCollector(clock, platform);
    static_cast<void>(take(collector->collect(activeContext(clock))));

    clock.advanceUtc(1s);
    platform->systemTimes = Detail::CpuTimesObservation{1'400U, 2'600U, 1'400U};
    platform->performance = FakeCpuMetricsPlatform::readyPerformance();
    const auto measured = take(collector->collect(activeContext(clock)));

    requireAvailableNear(
        measured.percent,
        60.0,
        "GetSystemTimes busy utilization did not use checked deltas");
    requireAvailableNear(
        measured.userPercent,
        40.0,
        "GetSystemTimes user utilization did not use delta user / total");
    requireAvailableNear(
        measured.systemPercent,
        20.0,
        "GetSystemTimes system utilization did not subtract idle from kernel");
    requireAvailableNear(
        measured.idlePercent,
        40.0,
        "GetSystemTimes idle utilization did not use delta idle / total");
    requireAvailable(
        measured.perLogicalProcessor,
        std::vector<double>{11.0, 22.0, 33.0, 44.0},
        "per-logical utilization used lexical or observation ordering");
    requireAvailable(
        measured.perCoreFrequencyMhz,
        std::vector<std::uint32_t>{4'100U, 4'200U, 4'300U, 4'400U},
        "per-logical frequency used lexical or observation ordering");
    require(
        measured.perLogicalProcessor.value->size() ==
                *measured.logicalProcessorCount.value &&
            measured.perCoreFrequencyMhz.value->size() ==
                *measured.logicalProcessorCount.value,
        "available per-logical vectors did not exactly match topology cardinality");

    clock.advanceUtc(1s);
    const auto regressionObservedAt = clock.utcNow();
    platform->systemTimes = Detail::CpuTimesObservation{100U, 200U, 100U};
    const auto regression = take(collector->collect(activeContext(clock)));
    requireStaleFrom(
        regression.percent,
        measured.percent,
        Availability::TemporarilyUnavailable,
        regressionObservedAt,
        "a GetSystemTimes counter regression did not retain prior utilization");
    requireStaleFrom(
        regression.userPercent,
        measured.userPercent,
        Availability::TemporarilyUnavailable,
        regressionObservedAt,
        "a GetSystemTimes counter regression did not retain prior user utilization");

    clock.advanceUtc(1s);
    platform->systemTimes = Detail::CpuTimesObservation{200U, 400U, 200U};
    const auto rebased = take(collector->collect(activeContext(clock)));
    requireAvailableNear(
        rebased.percent,
        200.0 / 3.0,
        "a GetSystemTimes regression was not adopted as the new baseline");
    requireAvailableNear(
        rebased.userPercent,
        100.0 / 3.0,
        "the rebased user delta was not calculated from the reset sample");
    requireAvailableNear(
        rebased.systemPercent,
        100.0 / 3.0,
        "the rebased system delta was not calculated from the reset sample");
    requireAvailableNear(
        rebased.idlePercent,
        100.0 / 3.0,
        "the rebased idle delta was not calculated from the reset sample");
}

void measuredZeroesRemainAvailableAndZeroLengthDeltasDoNot()
{
    TestClock clock;
    auto platform = std::make_shared<FakeCpuMetricsPlatform>();
    platform->performance.utilizationReady = false;
    platform->performance.perLogicalUtilization.clear();
    auto collector = createCollector(clock, platform);
    static_cast<void>(take(collector->collect(activeContext(clock))));

    clock.advanceUtc(1s);
    platform->systemTimes = Detail::CpuTimesObservation{1'100U, 2'100U, 1'000U};
    platform->performance = FakeCpuMetricsPlatform::readyPerformance();
    for (auto& logical : platform->performance.perLogicalUtilization) {
        logical.percent = 0.0;
    }
    const auto idle = take(collector->collect(activeContext(clock)));
    requireAvailableNear(idle.percent, 0.0, "a measured busy zero was unavailable");
    requireAvailableNear(idle.userPercent, 0.0, "a measured user zero was unavailable");
    requireAvailableNear(idle.systemPercent, 0.0, "a measured system zero was unavailable");
    requireAvailableNear(idle.idlePercent, 100.0, "a measured idle value was unavailable");
    requireAvailable(
        idle.perLogicalProcessor,
        std::vector<double>{0.0, 0.0, 0.0, 0.0},
        "measured per-logical zeroes were treated as unavailable");

    clock.advanceUtc(1s);
    const auto zeroDeltaObservedAt = clock.utcNow();
    const auto zeroDelta = take(collector->collect(activeContext(clock)));
    requireStaleFrom(
        zeroDelta.percent,
        idle.percent,
        Availability::TemporarilyUnavailable,
        zeroDeltaObservedAt,
        "a zero-length cumulative delta was emitted as a measured zero");
}

void exactTopologyIdentitySetsAreRequiredIndependently()
{
    TestClock clock;
    auto platform = std::make_shared<FakeCpuMetricsPlatform>();
    platform->performance.perLogicalUtilization.back() =
        Detail::LogicalProcessorUtilizationObservation{2U, 0U, 11.0};
    auto collector = createCollector(clock, platform);

    const auto wrongUtilization = take(
        collector->collect(activeContext(clock)));
    requireUnavailable(
        wrongUtilization.perLogicalProcessor,
        Availability::TemporarilyUnavailable,
        "same-cardinality utilization with a foreign identity was accepted");
    requireAvailable(
        wrongUtilization.perCoreFrequencyMhz,
        std::vector<std::uint32_t>{4'100U, 4'200U, 4'300U, 4'400U},
        "an invalid utilization identity discarded independent frequency data");

    TestClock duplicateClock;
    auto duplicatePlatform = std::make_shared<FakeCpuMetricsPlatform>();
    duplicatePlatform->performance.perLogicalFrequencyMhz.back() =
        duplicatePlatform->performance.perLogicalFrequencyMhz.front();
    auto duplicateCollector = createCollector(duplicateClock, duplicatePlatform);
    const auto duplicateFrequency = take(
        duplicateCollector->collect(activeContext(duplicateClock)));
    requireAvailable(
        duplicateFrequency.perLogicalProcessor,
        std::vector<double>{11.0, 22.0, 33.0, 44.0},
        "a duplicate frequency identity discarded independent utilization data");
    requireAvailable(
        duplicateFrequency.frequencyMhz,
        std::uint32_t{4'250U},
        "a duplicate per-logical frequency discarded aggregate frequency");
    requireUnavailable(
        duplicateFrequency.perCoreFrequencyMhz,
        Availability::TemporarilyUnavailable,
        "duplicate per-logical frequency identities were accepted");

    TestClock topologyClock;
    auto topologyPlatform = std::make_shared<FakeCpuMetricsPlatform>();
    topologyPlatform->topology.logicalProcessors.back() =
        topologyPlatform->topology.logicalProcessors.front();
    auto topologyCollector = createCollector(topologyClock, topologyPlatform);
    const auto invalidTopology = take(
        topologyCollector->collect(activeContext(topologyClock)));
    requireUnavailable(
        invalidTopology.logicalProcessorCount,
        Availability::TemporarilyUnavailable,
        "duplicate topology identities were published as a logical count");
    requireUnavailable(
        invalidTopology.physicalCoreCount,
        Availability::TemporarilyUnavailable,
        "invalid topology retained an independently trustworthy physical count");
    requireUnavailable(
        invalidTopology.perLogicalProcessor,
        Availability::TemporarilyUnavailable,
        "utilization was published without a trustworthy topology identity set");
    requireUnavailable(
        invalidTopology.perCoreFrequencyMhz,
        Availability::TemporarilyUnavailable,
        "frequency was published without a trustworthy topology identity set");
}

void partialFailuresAndActualFrequencyStatesRemainIndependent()
{
    TestClock clock;
    auto platform = std::make_shared<FakeCpuMetricsPlatform>();
    platform->systemTimesFailure = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "GetSystemTimes access was denied.");
    platform->brandFailure = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "ProcessorNameString is unavailable.");
    platform->performance.utilizationFailure = Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The utilization counter failed.",
        true);
    platform->performance.aggregateFrequencyMhz.reset();
    platform->performance.aggregateFrequencyFailure = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "The aggregate Actual Frequency counter is absent.");
    auto collector = createCollector(clock, platform);

    const auto partial = take(collector->collect(activeContext(clock)));
    requireUnavailable(
        partial.percent,
        Availability::AccessDenied,
        "a system-times access denial was not typed on aggregate CPU values");
    requireAvailable(
        partial.logicalProcessorCount,
        std::uint32_t{4U},
        "a system-times failure discarded successful topology");
    requireUnavailable(
        partial.brand,
        Availability::Unsupported,
        "an unavailable brand source was not explicit unsupported state");
    requireUnavailable(
        partial.perLogicalProcessor,
        Availability::TemporarilyUnavailable,
        "a utilization counter failure was not represented independently");
    requireUnavailable(
        partial.frequencyMhz,
        Availability::Unsupported,
        "missing Actual Frequency was replaced with a nominal fallback");
    requireAvailable(
        partial.perCoreFrequencyMhz,
        std::vector<std::uint32_t>{4'100U, 4'200U, 4'300U, 4'400U},
        "aggregate Actual Frequency failure discarded per-logical frequency");
    require(
        partial.frequencyMhz.source.find("Actual Frequency") !=
            std::string::npos,
        "unavailable frequency did not retain the intended Actual Frequency source");
    require(
        Domain::validateCpuMetrics(partial).hasValue(),
        "independent CPU partial failures produced an invalid domain snapshot");

    TestClock reverseClock;
    auto reversePlatform = std::make_shared<FakeCpuMetricsPlatform>();
    reversePlatform->performance.perLogicalFrequencyMhz.clear();
    reversePlatform->performance.perLogicalFrequencyFailure =
        Domain::makeError(
            Domain::ErrorCodes::Unauthorized,
            "Per-logical Actual Frequency access was denied.");
    auto reverseCollector = createCollector(reverseClock, reversePlatform);
    const auto reverse = take(
        reverseCollector->collect(activeContext(reverseClock)));
    requireAvailable(
        reverse.frequencyMhz,
        std::uint32_t{4'250U},
        "per-logical frequency denial discarded aggregate frequency");
    requireUnavailable(
        reverse.perCoreFrequencyMhz,
        Availability::AccessDenied,
        "per-logical frequency denial did not remain independent");

    TestClock missingClock;
    auto missingPlatform = std::make_shared<FakeCpuMetricsPlatform>();
    missingPlatform->performance.aggregateFrequencyMhz.reset();
    missingPlatform->performance.aggregateFrequencyFailure.reset();
    missingPlatform->performance.perLogicalFrequencyMhz.clear();
    missingPlatform->performance.perLogicalFrequencyFailure.reset();
    auto missingCollector = createCollector(missingClock, missingPlatform);
    const auto malformed = take(
        missingCollector->collect(activeContext(missingClock)));
    requireUnavailable(
        malformed.frequencyMhz,
        Availability::TemporarilyUnavailable,
        "missing Actual Frequency without a failure was treated as a measured value");
    requireUnavailable(
        malformed.perCoreFrequencyMhz,
        Availability::TemporarilyUnavailable,
        "missing per-logical frequency without a failure was treated as available empty data");
}

void failedRefreshesRetainFieldLevelValuesAndProvenance()
{
    TestClock clock;
    auto platform = std::make_shared<FakeCpuMetricsPlatform>();
    platform->performance.utilizationReady = false;
    platform->performance.perLogicalUtilization.clear();
    auto collector = createCollector(clock, platform);
    static_cast<void>(take(collector->collect(activeContext(clock))));

    clock.advanceUtc(1s);
    platform->systemTimes = Detail::CpuTimesObservation{1'400U, 2'600U, 1'400U};
    platform->performance = FakeCpuMetricsPlatform::readyPerformance();
    const auto available = take(collector->collect(activeContext(clock)));

    clock.advanceUtc(30s);
    const auto failureObservedAt = clock.utcNow();
    platform->systemTimesFailure = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "GetSystemTimes refresh was denied.");
    platform->brandFailure = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "ProcessorNameString refresh was denied.");
    platform->performance.utilizationFailure = Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "Utilization refresh failed.",
        true);
    platform->performance.aggregateFrequencyMhz.reset();
    platform->performance.aggregateFrequencyFailure = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "Aggregate Actual Frequency became unavailable.");
    platform->performance.perLogicalFrequencyMhz.clear();
    platform->performance.perLogicalFrequencyFailure = Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "Per-logical Actual Frequency refresh was denied.");

    const auto stale = take(collector->collect(activeContext(clock)));
    requireStaleFrom(
        stale.percent,
        available.percent,
        Availability::AccessDenied,
        failureObservedAt,
        "aggregate CPU refresh failure lost its prior value or provenance");
    requireStaleFrom(
        stale.userPercent,
        available.userPercent,
        Availability::AccessDenied,
        failureObservedAt,
        "user CPU refresh failure lost its prior value or provenance");
    requireStaleFrom(
        stale.perLogicalProcessor,
        available.perLogicalProcessor,
        Availability::TemporarilyUnavailable,
        failureObservedAt,
        "utilization refresh failure lost its prior vector or provenance");
    requireStaleFrom(
        stale.frequencyMhz,
        available.frequencyMhz,
        Availability::Unsupported,
        failureObservedAt,
        "aggregate frequency failure lost its independent prior value");
    requireStaleFrom(
        stale.perCoreFrequencyMhz,
        available.perCoreFrequencyMhz,
        Availability::AccessDenied,
        failureObservedAt,
        "per-logical frequency failure lost its independent prior vector");
    requireStaleFrom(
        stale.brand,
        available.brand,
        Availability::AccessDenied,
        failureObservedAt,
        "brand refresh failure lost its prior text or provenance");
    require(
        stale.loadAverage.availability == Availability::Unsupported &&
            !stale.loadAverage.stale && !stale.loadAverage.value &&
            stale.loadAverage.observedAt == failureObservedAt,
        "permanently unsupported load average incorrectly acquired stale state");

    clock.advanceUtc(1s);
    platform->topologyFailure = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "Topology refresh is unavailable.");
    const auto topologyObservedAt = clock.utcNow();
    const auto staleTopology = take(
        collector->collect(activeContext(clock)));
    requireStaleFrom(
        staleTopology.logicalProcessorCount,
        stale.logicalProcessorCount,
        Availability::Unsupported,
        topologyObservedAt,
        "logical count refresh did not preserve its prior topology value");
    requireStaleFrom(
        staleTopology.physicalCoreCount,
        stale.physicalCoreCount,
        Availability::Unsupported,
        topologyObservedAt,
        "physical count refresh did not preserve its prior topology value");
}

} // namespace

void registerWindowsCpuMetricsCollectorContractTests(TestRegistry& tests)
{
    addTest(
        tests,
        "telemetry_windows.cpu.parser-contract",
        pdhProcessorInstanceParserIsStrictAndNumeric);
    addTest(
        tests,
        "telemetry_windows.cpu.warmup-contract",
        firstSampleWarmsRatesWithoutSynthesizingZeroes);
    addTest(
        tests,
        "telemetry_windows.cpu.delta-contract",
        systemTimesDeltasSortNumericIdentitiesAndRebaseRegression);
    addTest(
        tests,
        "telemetry_windows.cpu.zero-contract",
        measuredZeroesRemainAvailableAndZeroLengthDeltasDoNot);
    addTest(
        tests,
        "telemetry_windows.cpu.topology-contract",
        exactTopologyIdentitySetsAreRequiredIndependently);
    addTest(
        tests,
        "telemetry_windows.cpu.partial-failure-contract",
        partialFailuresAndActualFrequencyStatesRemainIndependent);
    addTest(
        tests,
        "telemetry_windows.cpu.stale-contract",
        failedRefreshesRetainFieldLevelValuesAndProvenance);
}

} // namespace ForgeConductor::Tests
