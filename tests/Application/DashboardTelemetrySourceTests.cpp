#include "ForgeConductor/Application/DashboardTelemetrySource.h"

#include "ForgeConductor/Domain/Error.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
using namespace std::chrono_literals;

std::size_t assertions{};

[[noreturn]] void fail(
    const std::string_view expression,
    const std::size_t line)
{
    throw std::runtime_error{
        "requirement failed at line " + std::to_string(line) + ": " +
        std::string{expression}};
}

void require(
    const bool condition,
    const std::string_view expression,
    const std::size_t line)
{
    ++assertions;
    if (!condition) fail(expression, line);
}

#define REQUIRE(condition) \
    require(static_cast<bool>(condition), #condition, __LINE__)

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::UtcTimePoint utc(
    const std::int64_t milliseconds = 1'704'164'645'000LL)
{
    return Domain::UtcTimePoint{std::chrono::milliseconds{milliseconds}};
}

[[nodiscard]] Domain::TelemetrySnapshot snapshot(
    const double cpuPercent = 25.0,
    const std::size_t historyCount = 1U)
{
    Domain::CpuMetrics cpu;
    cpu.percent = cpuPercent;
    cpu.userPercent = 10.0;
    cpu.systemPercent = 15.0;
    cpu.idlePercent = 75.0;
    cpu.logicalProcessorCount = 1U;
    cpu.physicalCoreCount = 1U;
    cpu.perLogicalProcessor = {cpuPercent};

    Domain::RamMetrics ram;
    ram.percent = 40.0;
    ram.pressurePercent = 35.0;

    Domain::SystemMetrics system{
        utc(),
        "dashboard-host",
        "windows",
        "x64",
        std::move(cpu),
        std::move(ram),
        {},
        Domain::DiskIoMetrics{},
        {},
        {},
        Domain::PowerMetrics{}};
    Domain::ForgeSnapshot forge{
        utc(),
        path("C:\\Forge"),
        "windows-native",
        0U,
        0U,
        {},
        {},
        0U,
        Domain::TelemetryHealth::Ok};

    std::vector<Domain::HistoryPoint> history;
    history.reserve(historyCount);
    for (std::size_t index{}; index < historyCount; ++index) {
        history.push_back(Domain::HistoryPoint{
            utc() + std::chrono::seconds{static_cast<std::int64_t>(index)},
            cpuPercent,
            40.0,
            std::nullopt,
            0.0,
            index,
            Domain::TelemetryHealth::Ok});
    }
    return Domain::TelemetrySnapshot{
        std::move(system),
        std::move(forge),
        utc() + 1s,
        std::move(history),
        "windows-native"};
}

[[nodiscard]] Domain::TelemetryHealthReport healthReport()
{
    return Domain::TelemetryHealthReport{
        true,
        "forge-telemetry",
        "windows-native",
        false,
        "continuous-native",
        "Windows native collectors",
        "WinUI 3 + SSE",
        false};
}

[[nodiscard]] Domain::RuntimeDiagnosticSnapshot diagnostics()
{
    return Domain::RuntimeDiagnosticSnapshot{
        utc(), 1U, 2U, 3U, 4U, 1U, Domain::ResourcePressureLevel::Nominal,
        5U, 6U, 7U, 8U};
}

class Clock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utcValue{utc()};
    Domain::MonotonicTimePoint monotonicValue{10s};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utcValue;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonicValue;
    }
};

class TelemetryService final : public Contracts::ITelemetryService {
public:
    Snapshot latestValue;
    Domain::TelemetryHealthReport report{healthReport()};
    std::optional<Domain::Error> healthFailure;
    std::size_t startCalls{};
    std::size_t sampleCalls{};
    std::size_t healthCalls{};
    std::size_t setConsumerCalls{};
    mutable std::size_t latestCalls{};
    std::size_t stopCalls{};
    std::optional<Domain::OperationId> lastHealthOperation;

    [[nodiscard]] Domain::Result<void> start(
        const Domain::OperationContext&) noexcept override
    {
        ++startCalls;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<Snapshot> sample(
        bool,
        const Domain::OperationContext&) noexcept override
    {
        ++sampleCalls;
        if (latestValue != nullptr) {
            return Domain::Result<Snapshot>::success(latestValue);
        }
        return Domain::Result<Snapshot>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "No telemetry sample."));
    }

    [[nodiscard]] Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext& context) noexcept override
    {
        ++healthCalls;
        lastHealthOperation = context.operationId;
        if (healthFailure) {
            return Domain::Result<Domain::TelemetryHealthReport>::failure(
                *healthFailure);
        }
        return Domain::Result<Domain::TelemetryHealthReport>::success(report);
    }

    [[nodiscard]] Domain::Result<void> setConsumer(
        Consumer) noexcept override
    {
        ++setConsumerCalls;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Snapshot latest() const noexcept override
    {
        ++latestCalls;
        return latestValue;
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return 0U;
    }

    void stop() noexcept override { ++stopCalls; }
};

class RuntimeDiagnostics final : public Contracts::IRuntimeDiagnostics {
public:
    Domain::RuntimeDiagnosticSnapshot value{diagnostics()};
    std::optional<Domain::Error> failure;
    std::size_t acquireCalls{};
    std::size_t snapshotCalls{};
    std::size_t shutdownCalls{};
    std::optional<Domain::OperationId> lastSnapshotOperation;

    [[nodiscard]] Domain::Result<Contracts::RuntimeOwnershipLease> acquire(
        Contracts::RuntimeOwnerKind,
        const Domain::OperationContext&) noexcept override
    {
        ++acquireCalls;
        return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Ownership acquisition is not used by this test."));
    }

    [[nodiscard]] Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept override
    {
        ++snapshotCalls;
        lastSnapshotOperation = context.operationId;
        if (failure) {
            return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::failure(
                *failure);
        }
        return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::success(value);
    }

    void shutdown() noexcept override { ++shutdownCalls; }
};

[[nodiscard]] Domain::OperationContext context(
    const Domain::MonotonicTimePoint deadline =
        Domain::MonotonicTimePoint{} + 20s,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        take(Domain::OperationId::parse(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        deadline,
        cancellation,
        take(Domain::CorrelationId::parse("dashboard-telemetry-source"))};
}

[[nodiscard]] Application::DashboardTelemetrySourceConfiguration configuration(
    const std::size_t maximumSubscriptions = 32U)
{
    return Application::DashboardTelemetrySourceConfiguration{
        Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB),
        maximumSubscriptions,
        Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes,
        true,
        true,
        false};
}

[[nodiscard]] Dashboard::DashboardStreamRateSelection rate(
    const double deliveryHz)
{
    return Dashboard::DashboardStreamRateSelection{
        20.0,
        deliveryHz,
        Dashboard::DashboardStreamRateSource::ProfileDefault,
        false,
        true};
}

[[nodiscard]] std::unique_ptr<Application::DashboardTelemetrySource> source(
    TelemetryService& telemetry,
    RuntimeDiagnostics& runtime,
    Clock& clock,
    Application::DashboardTelemetrySourceConfiguration config =
        configuration())
{
    return take(Application::DashboardTelemetrySource::create(
        telemetry, runtime, clock, std::move(config)));
}

void closesConfigurationAndDependencyOwnership()
{
    static_assert(std::is_final_v<Application::DashboardTelemetrySource>);
    static_assert(std::is_base_of_v<
        Dashboard::IDashboardTelemetrySource,
        Application::DashboardTelemetrySource>);
    static_assert(!std::is_copy_constructible_v<
        Application::DashboardTelemetrySource>);
    static_assert(!std::is_move_constructible_v<
        Application::DashboardTelemetrySource>);
    static_assert(noexcept(std::declval<Application::DashboardTelemetrySource&>()
                               .shutdown()));
    static_assert(
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions == 32U);

    TelemetryService telemetry;
    RuntimeDiagnostics runtime;
    Clock clock;

    auto invalid = configuration(33U);
    auto rejected = Application::DashboardTelemetrySource::create(
        telemetry, runtime, clock, invalid);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::InvalidRequest);

    invalid = configuration();
    invalid.resourceBudgets.telemetryPendingSnapshotsMaximum = 2U;
    rejected = Application::DashboardTelemetrySource::create(
        telemetry, runtime, clock, invalid);
    REQUIRE(!rejected);

    invalid = configuration();
    invalid.maximumEncodedBytes =
        Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes + 1U;
    rejected = Application::DashboardTelemetrySource::create(
        telemetry, runtime, clock, invalid);
    REQUIRE(!rejected);

    telemetry.latestValue = std::make_shared<const Domain::TelemetrySnapshot>(
        snapshot(101.0));
    rejected = Application::DashboardTelemetrySource::create(
        telemetry, runtime, clock, configuration());
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::InvalidRequest);

    telemetry.latestValue.reset();
    {
        auto owned = source(telemetry, runtime, clock);
        REQUIRE(telemetry.latestCalls == 2U);
        REQUIRE(telemetry.startCalls == 0U);
        REQUIRE(telemetry.setConsumerCalls == 0U);
    }
    REQUIRE(telemetry.stopCalls == 0U);
    REQUIRE(runtime.shutdownCalls == 0U);
}

void seedsAndPublishesOneSharedImmutableFramePair()
{
    TelemetryService telemetry;
    RuntimeDiagnostics runtime;
    Clock clock;
    const auto initial =
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot(25.0));
    telemetry.latestValue = initial;
    auto owned = source(telemetry, runtime, clock, configuration(4U));

    const auto initialLatest = take(owned->latest(context()));
    REQUIRE(initialLatest.snapshot == initial);
    REQUIRE(!initialLatest.measuredSampleHz.has_value());

    auto first = take(owned->subscribe(rate(2.0), context()));
    auto second = take(owned->subscribe(rate(1.5), context()));
    REQUIRE(first->deliveryHz() == 2.0);
    REQUIRE(second->deliveryHz() == 1.5);
    REQUIRE(first->pendingCount() == 1U);
    REQUIRE(second->pendingCount() == 1U);
    const auto firstSeed = first->takeLatest();
    const auto secondSeed = second->takeLatest();
    REQUIRE(firstSeed != nullptr);
    REQUIRE(firstSeed == secondSeed);
    REQUIRE(firstSeed->sourceSequence() == 1U);
    REQUIRE(firstSeed->compactBytes() != nullptr);
    REQUIRE(firstSeed->fullBytes() != nullptr);
    REQUIRE(!firstSeed->compactBytes()->empty());
    REQUIRE(!firstSeed->fullBytes()->empty());

    const auto next =
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot(30.0));
    const auto published = owned->publish(
        Dashboard::DashboardTelemetryObservation{next, 1.75});
    REQUIRE(published);
    REQUIRE(first->pendingCount() == 1U);
    REQUIRE(second->pendingCount() == 1U);
    const auto firstNext = first->takeLatest();
    const auto secondNext = second->takeLatest();
    REQUIRE(firstNext == secondNext);
    REQUIRE(firstNext != firstSeed);
    REQUIRE(firstNext->sourceSequence() == 2U);

    const auto latest = take(owned->latest(context()));
    REQUIRE(latest.snapshot == next);
    REQUIRE(latest.measuredSampleHz == 1.75);

    REQUIRE(owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot(35.0)),
        1.8}));
    REQUIRE(owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot(40.0)),
        1.9}));
    REQUIRE(first->pendingCount() == 1U);
    REQUIRE(first->takeLatest()->sourceSequence() == 4U);
    REQUIRE(second->takeLatest()->sourceSequence() == 4U);
    REQUIRE(telemetry.setConsumerCalls == 0U);
}

void rejectsInvalidObservationsWithoutPublishingPartialState()
{
    TelemetryService telemetry;
    RuntimeDiagnostics runtime;
    Clock clock;
    auto config = configuration(2U);
    config.resourceBudgets.historyPointsDefault = 1U;
    config.resourceBudgets.historyPointsHardMaximum = 1U;
    auto owned = source(telemetry, runtime, clock, config);

    auto missing = owned->latest(context());
    REQUIRE(!missing);
    REQUIRE(
        missing.error().code == Domain::ErrorCodes::HostCapabilityUnavailable);
    auto noSeed = owned->subscribe(rate(2.0), context());
    REQUIRE(!noSeed);
    REQUIRE(
        noSeed.error().code == Domain::ErrorCodes::HostCapabilityUnavailable);

    auto result = owned->publish(
        Dashboard::DashboardTelemetryObservation{nullptr, 1.0});
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::IntegrityFailure);

    result = owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot()),
        std::numeric_limits<double>::quiet_NaN()});
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);

    result = owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot(25.0, 2U)),
        1.0});
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::LimitExceeded);

    const auto accepted =
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot());
    REQUIRE(owned->publish(
        Dashboard::DashboardTelemetryObservation{accepted, 1.0}));
    auto subscription = take(owned->subscribe(rate(1.0), context()));
    const auto acceptedFrame = subscription->takeLatest();
    REQUIRE(acceptedFrame != nullptr);

    result = owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot(101.0)),
        1.0});
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(subscription->pendingCount() == 0U);
    REQUIRE(take(owned->latest(context())).snapshot == accepted);

    TelemetryService sizeTelemetry;
    auto tiny = configuration();
    tiny.maximumEncodedBytes = 64U;
    auto tinySource = source(sizeTelemetry, runtime, clock, tiny);
    result = tinySource->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot()), 1.0});
    REQUIRE(!result);
    REQUIRE(
        result.error().code == Domain::ErrorCodes::PayloadTooLarge ||
        result.error().code == Domain::ErrorCodes::LimitExceeded);
}

void composesBoundedHealthAndPropagatesDependencyFailures()
{
    TelemetryService telemetry;
    RuntimeDiagnostics runtime;
    Clock clock;
    telemetry.latestValue =
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot());
    auto owned = source(telemetry, runtime, clock);

    auto value = take(owned->health(context()));
    REQUIRE(value.report.service == "forge-telemetry");
    REQUIRE(value.targetSampleHz == 2.0);
    REQUIRE(value.measuredSampleHz == 0.0);
    REQUIRE(!value.streamRunning);
    REQUIRE(value.exportPresent);
    REQUIRE(value.staticPresent);
    REQUIRE(!value.nodeAvailable);
    REQUIRE(telemetry.healthCalls == 1U);
    REQUIRE(runtime.snapshotCalls == 1U);
    REQUIRE(telemetry.lastHealthOperation == context().operationId);
    REQUIRE(runtime.lastSnapshotOperation == context().operationId);

    REQUIRE(owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot()), 1.5}));
    value = take(owned->health(context()));
    REQUIRE(value.streamRunning);
    REQUIRE(value.measuredSampleHz == 1.5);

    const Domain::Error dependencyError = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy, "private dependency detail", true,
        "private-evidence");
    telemetry.healthFailure = dependencyError;
    auto failed = owned->health(context());
    REQUIRE(!failed);
    REQUIRE(failed.error() == dependencyError);
    REQUIRE(runtime.snapshotCalls == 2U);

    telemetry.healthFailure.reset();
    runtime.failure = dependencyError;
    failed = owned->health(context());
    REQUIRE(!failed);
    REQUIRE(failed.error() == dependencyError);

    runtime.failure.reset();
    telemetry.report.service = std::string(
        Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes, 'x');
    failed = owned->health(context());
    REQUIRE(!failed);
    REQUIRE(
        failed.error().code == Domain::ErrorCodes::PayloadTooLarge ||
        failed.error().code == Domain::ErrorCodes::LimitExceeded);
}

void enforcesContextRateCapacityAndShutdownClosure()
{
    TelemetryService telemetry;
    RuntimeDiagnostics runtime;
    Clock clock;
    telemetry.latestValue =
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot());
    auto owned = source(telemetry, runtime, clock, configuration(32U));

    std::stop_source cancelled;
    cancelled.request_stop();
    auto result = owned->latest(context(
        Domain::MonotonicTimePoint{} + 20s,
        cancelled.get_token()));
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::Cancelled);
    result = owned->latest(context(Domain::MonotonicTimePoint{} + 10s));
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::DeadlineExceeded);

    auto badRate = owned->subscribe(
        rate(std::numeric_limits<double>::infinity()), context());
    REQUIRE(!badRate);
    REQUIRE(badRate.error().code == Domain::ErrorCodes::InvalidRequest);
    badRate = owned->subscribe(rate(0.99), context());
    REQUIRE(!badRate);
    badRate = owned->subscribe(rate(2.01), context());
    REQUIRE(!badRate);

    std::vector<std::unique_ptr<Dashboard::IDashboardSseSubscription>> streams;
    streams.reserve(32U);
    for (std::size_t index{}; index < 32U; ++index) {
        streams.push_back(take(owned->subscribe(rate(2.0), context())));
    }
    auto overflow = owned->subscribe(rate(2.0), context());
    REQUIRE(!overflow);
    REQUIRE(overflow.error().code == Domain::ErrorCodes::LimitExceeded);
    streams.front()->close();
    streams.front() = take(owned->subscribe(rate(2.0), context()));

    owned->shutdown();
    owned->shutdown();
    for (const auto& stream : streams) {
        REQUIRE(stream->pendingCount() == 0U);
        REQUIRE(stream->takeLatest() == nullptr);
    }
    const auto healthCalls = telemetry.healthCalls;
    auto closedHealth = owned->health(context());
    REQUIRE(!closedHealth);
    REQUIRE(closedHealth.error().code == Domain::ErrorCodes::Cancelled);
    REQUIRE(telemetry.healthCalls == healthCalls);
    REQUIRE(!owned->latest(context()));
    REQUIRE(!owned->subscribe(rate(2.0), context()));
    REQUIRE(!owned->publish(Dashboard::DashboardTelemetryObservation{
        std::make_shared<const Domain::TelemetrySnapshot>(snapshot()), 2.0}));
    REQUIRE(telemetry.startCalls == 0U);
    REQUIRE(telemetry.setConsumerCalls == 0U);
    REQUIRE(telemetry.stopCalls == 0U);
    REQUIRE(runtime.shutdownCalls == 0U);

    TelemetryService constrainedTelemetry;
    constrainedTelemetry.latestValue = telemetry.latestValue;
    auto constrainedConfig = configuration();
    constrainedConfig.resourceBudgets =
        Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB);
    auto constrained = source(
        constrainedTelemetry, runtime, clock, constrainedConfig);
    badRate = constrained->subscribe(rate(2.0), context());
    REQUIRE(!badRate);
    REQUIRE(badRate.error().code == Domain::ErrorCodes::InvalidRequest);
    REQUIRE(constrained->subscribe(rate(1.0), context()));
}

} // namespace

int main()
{
    try {
        closesConfigurationAndDependencyOwnership();
        seedsAndPublishesOneSharedImmutableFramePair();
        rejectsInvalidObservationsWithoutPublishingPartialState();
        composesBoundedHealthAndPropagatesDependencyFailures();
        enforcesContextRateCapacityAndShutdownClosure();
        std::cout << "Dashboard telemetry source tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard telemetry source tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
