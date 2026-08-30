#include "UnavailableTelemetryService.h"

#include "ForgeConductor/Application/DashboardTelemetrySource.h"
#include "ForgeConductor/Dashboard/DashboardApplicationJsonCodec.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Application = ForgeConductor::Application;
namespace Composition = ForgeConductor::Composition::Windows;
namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Composition::UnavailableTelemetryService>);
static_assert(std::is_base_of_v<
              Contracts::ITelemetryService,
              Composition::UnavailableTelemetryService>);
static_assert(!std::is_copy_constructible_v<
              Composition::UnavailableTelemetryService>);
static_assert(!std::is_move_constructible_v<
              Composition::UnavailableTelemetryService>);

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
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

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

class Clock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utcValue{Domain::UtcTimePoint{} + 100s};
    Domain::MonotonicTimePoint monotonicValue{
        Domain::MonotonicTimePoint{} + 10s};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utcValue;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonicValue;
    }
};

class RuntimeDiagnostics final : public Contracts::IRuntimeDiagnostics {
public:
    [[nodiscard]] Domain::Result<Contracts::RuntimeOwnershipLease> acquire(
        Contracts::RuntimeOwnerKind,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Ownership acquisition is not expected in this test."));
    }

    [[nodiscard]] Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        ++snapshotCalls;
        return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::success(
            Domain::RuntimeDiagnosticSnapshot{
                Domain::UtcTimePoint{} + 100s,
                0U,
                0U,
                0U,
                0U,
                0U,
                Domain::ResourcePressureLevel::Nominal,
                0U,
                0U,
                0U,
                0U});
    }

    void shutdown() noexcept override { ++shutdownCalls; }

    std::size_t snapshotCalls{};
    std::size_t shutdownCalls{};
};

[[nodiscard]] Domain::OperationContext context(
    const Domain::MonotonicTimePoint deadline =
        Domain::MonotonicTimePoint{} + 20s,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        deadline,
        cancellation,
        parse<Domain::CorrelationId>("telemetry-unavailable")};
}

[[nodiscard]] Application::DashboardTelemetrySourceConfiguration
configuration()
{
    return Application::DashboardTelemetrySourceConfiguration{
        Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB),
        4U,
        Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes,
        false,
        true,
        false};
}

[[nodiscard]] Dashboard::DashboardStreamRateSelection rate()
{
    return Dashboard::DashboardStreamRateSelection{
        20.0,
        2.0,
        Dashboard::DashboardStreamRateSource::ProfileDefault,
        false,
        true};
}

void reportsUnavailableHealthWithoutFabricatingObservations()
{
    Clock clock;
    Composition::UnavailableTelemetryService service{clock};

    take(service.start(context()));
    const auto report = take(service.health(context()));
    require(!report.ok, "unavailable telemetry reported healthy");
    require(report.service == "forge-telemetry", "service identity changed");
    require(report.runtime == "windows-native", "runtime identity changed");
    require(!report.interferesWithMcp, "telemetry claimed MCP interference");
    require(report.mode == "unavailable", "unavailable mode was not explicit");
    require(
        report.collectors ==
            "Windows native telemetry collectors unavailable",
        "collector unavailability was not explicit");
    require(report.ui == "WinUI 3 + SSE", "UI projection changed");
    require(!report.nodeRequired, "native telemetry claimed a Node dependency");

    const auto sampled = service.sample(true, context());
    requireError(
        sampled,
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "unavailable telemetry fabricated a sample");
    require(sampled.error().retryable, "unavailable sample was not retryable");
    require(service.latest() == nullptr, "unavailable telemetry retained a snapshot");
    require(service.pendingCount() == 0U, "unavailable telemetry retained pending work");
    service.stop();
    service.stop();
}

void cancellationAndDeadlinePrecedeCapabilityFailure()
{
    Clock clock;
    Composition::UnavailableTelemetryService service{clock};

    std::stop_source cancellation;
    cancellation.request_stop();
    requireError(
        service.sample(false, context(
            Domain::MonotonicTimePoint{} + 20s,
            cancellation.get_token())),
        Domain::ErrorCodes::Cancelled,
        "cancellation did not precede capability unavailability");
    requireError(
        service.start(context(Domain::MonotonicTimePoint{} + 10s)),
        Domain::ErrorCodes::DeadlineExceeded,
        "start ignored an expired deadline");
    requireError(
        service.health(context(Domain::MonotonicTimePoint{} + 10s)),
        Domain::ErrorCodes::DeadlineExceeded,
        "health ignored an expired deadline");
}

void discardsConsumerOwnershipWithoutInvokingCallbacks()
{
    Clock clock;
    Composition::UnavailableTelemetryService service{clock};
    auto captured = std::make_shared<std::size_t>(0U);
    std::weak_ptr<std::size_t> weakCaptured{captured};

    take(service.setConsumer(
        [captured](Contracts::ITelemetryService::Snapshot) noexcept {
            ++*captured;
        }));
    require(*captured == 0U, "consumer was invoked without a producer");
    captured.reset();
    require(weakCaptured.expired(), "consumer ownership was retained");
}

void projectsDegradationThroughTheRealDashboardSource()
{
    Clock clock;
    RuntimeDiagnostics runtime;
    Composition::UnavailableTelemetryService service{clock};
    auto source = take(Application::DashboardTelemetrySource::create(
        service, runtime, clock, configuration()));

    const auto health = take(source->health(context()));
    require(!health.report.ok, "dashboard changed unavailable health to success");
    require(health.report.mode == "unavailable", "dashboard changed health mode");
    require(health.targetSampleHz == 2.0, "dashboard target rate changed");
    require(health.measuredSampleHz == 0.0, "dashboard fabricated a measured rate");
    require(!health.streamRunning, "dashboard fabricated a running producer");
    require(runtime.snapshotCalls == 1U, "runtime diagnostics were not projected");

    requireError(
        source->latest(context()),
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "dashboard fabricated a latest telemetry observation");
    requireError(
        source->subscribe(rate(), context()),
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "dashboard opened an SSE stream without telemetry");

    source->shutdown();
    source.reset();
    require(runtime.shutdownCalls == 0U, "dashboard source owned runtime diagnostics");
    require(
        !take(service.health(context())).ok,
        "dashboard source owned the telemetry service lifecycle");
}

} // namespace

int main()
{
    try {
        reportsUnavailableHealthWithoutFabricatingObservations();
        cancellationAndDeadlinePrecedeCapabilityFailure();
        discardsConsumerOwnershipWithoutInvokingCallbacks();
        projectsDegradationThroughTheRealDashboardSource();
        std::cout << "Unavailable telemetry service tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unavailable telemetry service tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unavailable telemetry service tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
