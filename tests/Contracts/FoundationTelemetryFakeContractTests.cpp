#include "Contracts/FoundationTelemetryFakeContractTests.h"

#include "ForgeConductor/Domain/Domain.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/TelemetryFakes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using namespace std::chrono_literals;

static_assert(std::is_final_v<Fakes::FakeClock>);
static_assert(std::is_final_v<Fakes::SequenceUuidGenerator>);
static_assert(std::is_final_v<Fakes::ScriptedHasher>);
static_assert(std::is_final_v<Fakes::ScriptedRedactor>);
static_assert(std::is_final_v<Fakes::CpuMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::RamMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::DiskVolumeCollectorFake>);
static_assert(std::is_final_v<Fakes::DiskIoMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::GpuMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::ProcessMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::PowerMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::SystemMetricsCollectorFake>);
static_assert(std::is_final_v<Fakes::ForgeMetricsCollectorFake>);

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T id(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

void testFoundationFakes(const Domain::Sha256Digest& digest)
{
    const auto initialUtc = Domain::UtcTimePoint{} + 2s;
    const auto initialMonotonic = Domain::MonotonicTimePoint{} + 3s;
    Fakes::FakeClock clock{initialUtc, initialMonotonic};
    expect(
        clock.utcNow() == initialUtc &&
            clock.monotonicNow() == initialMonotonic,
        "fake clock did not return its injected times");
    clock.advance(4s);
    expect(
        clock.utcNow() == initialUtc + 4s &&
            clock.monotonicNow() == initialMonotonic + 4s,
        "fake clock did not advance both time domains deterministically");

    const auto firstUuid = id<Domain::Uuid>(
        "10101010-1010-4010-8010-101010101010");
    const auto secondUuid = id<Domain::Uuid>(
        "20202020-2020-4020-8020-202020202020");
    Fakes::SequenceUuidGenerator uuidGenerator{{firstUuid, secondUuid}};
    expect(
        take(uuidGenerator.next()) == firstUuid &&
            take(uuidGenerator.next()) == secondUuid &&
            uuidGenerator.consumed() == 2,
        "UUID fake did not preserve its injected sequence");
    const auto exhausted = uuidGenerator.next();
    expect(
        !exhausted &&
            exhausted.error().code == Domain::ErrorCodes::LimitExceeded,
        "UUID fake did not fail closed after sequence exhaustion");

    Fakes::ScriptedHasher hasher{digest};
    const std::array bytes{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    expect(
        take(hasher.sha256(bytes)) == digest &&
            hasher.calls() == 1 &&
            hasher.lastByteCount() == bytes.size(),
        "hasher fake did not return its script and capture the byte count");

    Fakes::ScriptedRedactor redactor;
    expect(
        take(redactor.redact("contains secret material")) == "<redacted>",
        "redactor fake did not redact deterministic secret material");
    expect(
        take(redactor.redact("public material")) == "public material",
        "redactor fake changed non-secret material");
    constexpr std::string_view privateKeyText = "PRIVATE KEY";
    const auto rejected = redactor.redact(privateKeyText);
    expect(
        !rejected &&
            rejected.error().code == Domain::ErrorCodes::RedactionRejected &&
            redactor.calls() == 3 &&
            redactor.lastInputBytes() == privateKeyText.size(),
        "redactor fake did not reject and record private-key material");
}

void testTelemetryCollectorFakes(
    const Domain::OperationId& operationId,
    const Domain::CorrelationId& correlationId,
    const Domain::PathText& root)
{
    const Domain::OperationContext context{
        operationId,
        Domain::MonotonicTimePoint{} + 10s,
        std::stop_token{},
        correlationId};

    Fakes::CpuMetricsCollectorFake cpu;
    cpu.collectResult.set(
        Domain::Result<Domain::CpuMetrics>::success(Domain::CpuMetrics{}));
    const auto cpuResult = cpu.collect(context);
    expect(
        cpuResult.hasValue() && cpu.calls() == 1 &&
            cpu.lastOperationId() &&
            cpu.lastOperationId().value() == operationId,
        "CPU collector fake did not return its deterministic script");

    Fakes::RamMetricsCollectorFake ram;
    ram.collectResult.set(
        Domain::Result<Domain::RamMetrics>::success(Domain::RamMetrics{}));
    expect(
        ram.collect(context).hasValue() && ram.calls() == 1 &&
            ram.lastOperationId() &&
            ram.lastOperationId().value() == operationId,
        "RAM collector fake did not return its deterministic script");

    Fakes::DiskVolumeCollectorFake diskVolumes;
    diskVolumes.collectResult.set(
        Domain::Result<std::vector<Domain::DiskVolume>>::success({}));
    expect(
        diskVolumes.collect(context).hasValue() &&
            diskVolumes.calls() == 1 && diskVolumes.lastOperationId() &&
            diskVolumes.lastOperationId().value() == operationId,
        "disk-volume collector fake did not return its deterministic script");

    Fakes::DiskIoMetricsCollectorFake diskIo;
    diskIo.collectResult.set(
        Domain::Result<Domain::DiskIoMetrics>::success(
            Domain::DiskIoMetrics{}));
    expect(
        diskIo.collect(context).hasValue() && diskIo.calls() == 1 &&
            diskIo.lastOperationId() &&
            diskIo.lastOperationId().value() == operationId,
        "disk-I/O collector fake did not return its deterministic script");

    Fakes::GpuMetricsCollectorFake gpu;
    gpu.collectResult.set(
        Domain::Result<std::vector<Domain::GpuMetrics>>::success({}));
    expect(
        gpu.collect(context).hasValue() && gpu.calls() == 1 &&
            gpu.lastOperationId() &&
            gpu.lastOperationId().value() == operationId,
        "GPU collector fake did not return its deterministic script");

    Fakes::ProcessMetricsCollectorFake processes;
    processes.collectResult.set(
        Domain::Result<std::vector<Domain::ProcessMetrics>>::success({}));
    expect(
        processes.collect(context).hasValue() && processes.calls() == 1 &&
            processes.lastOperationId() &&
            processes.lastOperationId().value() == operationId,
        "process collector fake did not return its deterministic script");

    Fakes::PowerMetricsCollectorFake power;
    power.collectResult.set(
        Domain::Result<Domain::PowerMetrics>::success(Domain::PowerMetrics{}));
    expect(
        power.collect(context).hasValue() && power.calls() == 1 &&
            power.lastOperationId() &&
            power.lastOperationId().value() == operationId,
        "power collector fake did not return its deterministic script");

    Fakes::SystemMetricsCollectorFake system;
    system.collectResult.set(
        Domain::Result<Domain::SystemMetrics>::success(
            Domain::SystemMetrics{}));
    expect(
        system.collect(context).hasValue() && system.calls() == 1 &&
            system.lastOperationId() &&
            system.lastOperationId().value() == operationId,
        "system collector fake did not return its deterministic script");

    Fakes::ForgeMetricsCollectorFake forge;
    forge.collectResult.set(
        Domain::Result<Domain::ForgeSnapshot>::success(
            Domain::ForgeSnapshot{Domain::UtcTimePoint{}, root, "test"}));
    expect(
        forge.collect(context).hasValue() && forge.calls() == 1 &&
            forge.lastOperationId() &&
            forge.lastOperationId().value() == operationId,
        "Forge collector fake did not return its deterministic script");

    std::stop_source cancellation;
    cancellation.request_stop();
    const Domain::OperationContext cancelledContext{
        operationId,
        context.deadline,
        cancellation.get_token(),
        correlationId};
    const auto cancelled = cpu.collect(cancelledContext);
    expect(
        !cancelled &&
            cancelled.error().code == Domain::ErrorCodes::Cancelled,
        "telemetry collector fake ignored operation cancellation");

    cpu.setNow(context.deadline);
    const auto expired = cpu.collect(context);
    expect(
        !expired &&
            expired.error().code == Domain::ErrorCodes::DeadlineExceeded,
        "telemetry collector fake ignored the injected monotonic deadline");

    cpu.setNow(Domain::MonotonicTimePoint{});
    cpu.shutdown();
    const auto shutDown = cpu.collect(context);
    expect(
        !shutDown &&
            shutDown.error().code == Domain::ErrorCodes::Cancelled &&
            cpu.calls() == 4,
        "telemetry collector fake accepted work after shutdown");
}

} // namespace

void runFoundationTelemetryFakeContractTests()
{
    const auto digest = take(Domain::Sha256Digest::parse(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    const auto operationId = id<Domain::OperationId>(
        "30303030-3030-4030-8030-303030303030");
    const auto correlationId =
        id<Domain::CorrelationId>("foundation-telemetry-correlation");
    const auto root = path("C:/foundation-telemetry-test");

    testFoundationFakes(digest);
    testTelemetryCollectorFakes(operationId, correlationId, root);
}

} // namespace ForgeConductor::Tests
