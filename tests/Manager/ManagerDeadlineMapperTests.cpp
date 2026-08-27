#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Manager;

using namespace std::chrono_literals;

class FakeClock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utc{};
    Domain::MonotonicTimePoint monotonic{};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utc;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic;
    }
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error{message};
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
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

[[nodiscard]] Domain::OperationId operationId()
{
    return Domain::OperationId::parse(
               "11111111-1111-4111-8111-111111111111")
        .value();
}

[[nodiscard]] Domain::CorrelationId correlationId()
{
    return Domain::CorrelationId::parse("deadline-mapper-test").value();
}

[[nodiscard]] Domain::OperationContext context(
    const Domain::MonotonicTimePoint deadline,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        operationId(), deadline, cancellation, correlationId()};
}

void testDefaultLimits()
{
    const Manager::ManagerTransportLimits limits;
    require(
        limits.maximumFrameBytes == 2U * 1024U * 1024U,
        "default frame bound");
    require(limits.maximumRequestLifetime == 5min, "default lifetime");
    require(limits.connectTimeout == 2s, "default connect timeout");
    require(limits.maximumConcurrentClientRequests == 16U, "client bound");
    require(limits.maximumActiveRegularOperations == 3U, "dispatcher bound");
}

void testWireMappingRoundsUpWithoutEarlyExpiry()
{
    FakeClock clock;
    clock.utc = Domain::UtcTimePoint{
        std::chrono::duration_cast<Domain::UtcTimePoint::duration>(
            10'000ms + 250us)};
    clock.monotonic = Domain::MonotonicTimePoint{
        std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(100s)};
    const auto localDeadline = clock.monotonic + 1'500us;

    const auto wire = Manager::toManagerWireDeadline(
        context(localDeadline), clock);
    require(wire.hasValue(), "wire mapping");
    require(wire.value() == 10'002, "wire deadline must round upward");

    const auto mapped = Manager::fromManagerWireDeadline(wire.value(), clock);
    require(mapped.hasValue(), "local mapping");
    require(
        mapped.value() >= localDeadline,
        "round trip may not produce an early local deadline");
    require(
        mapped.value() - localDeadline <= 1ms,
        "round trip rounding is bounded to one millisecond");
}

void testOutboundValidation()
{
    FakeClock clock;
    clock.utc = Domain::UtcTimePoint{100s};
    clock.monotonic = Domain::MonotonicTimePoint{10s};

    requireError(
        Manager::toManagerWireDeadline(context(clock.monotonic), clock),
        Domain::ErrorCodes::DeadlineExceeded,
        "expired outbound deadline");
    requireError(
        Manager::toManagerWireDeadline(
            context(clock.monotonic + 5min + 1ms), clock),
        Domain::ErrorCodes::InvalidRequest,
        "distant outbound deadline");

    std::stop_source cancellation;
    cancellation.request_stop();
    requireError(
        Manager::toManagerWireDeadline(
            context(clock.monotonic + 1s, cancellation.get_token()), clock),
        Domain::ErrorCodes::Cancelled,
        "cancelled outbound deadline");

    Manager::ManagerTransportLimits invalid;
    invalid.maximumRequestLifetime = 0ms;
    requireError(
        Manager::toManagerWireDeadline(
            context(clock.monotonic + 1s), clock, invalid),
        Domain::ErrorCodes::InternalFailure,
        "invalid lifetime configuration");
}

void testInboundValidationAndBoundary()
{
    FakeClock clock;
    clock.utc = Domain::UtcTimePoint{
        std::chrono::duration_cast<Domain::UtcTimePoint::duration>(
            20'000ms + 250us)};
    clock.monotonic = Domain::MonotonicTimePoint{50s};

    requireError(
        Manager::fromManagerWireDeadline(-1, clock),
        Domain::ErrorCodes::InvalidRequest,
        "negative wire deadline");
    requireError(
        Manager::fromManagerWireDeadline(20'000, clock),
        Domain::ErrorCodes::DeadlineExceeded,
        "expired wire deadline");
    requireError(
        Manager::fromManagerWireDeadline(320'002, clock),
        Domain::ErrorCodes::InvalidRequest,
        "distant wire deadline");

    // ceil(now + five minutes) is the sole rounding-slack endpoint accepted.
    const auto boundary = Manager::fromManagerWireDeadline(320'001, clock);
    require(boundary.hasValue(), "rounded five-minute boundary");
    require(
        boundary.value() >= clock.monotonic + 5min,
        "boundary mapping may not expire early");
    require(
        boundary.value() <= clock.monotonic + 5min + 1ms,
        "boundary mapping rounding must stay within one millisecond");
}

void testExtremeClocksFailWithoutArithmeticOverflow()
{
    FakeClock clock;
    clock.utc = Domain::UtcTimePoint::max();
    clock.monotonic = Domain::MonotonicTimePoint{10s};
    requireError(
        Manager::toManagerWireDeadline(
            context(clock.monotonic + 1s), clock),
        Domain::ErrorCodes::InvalidRequest,
        "maximum UTC outbound clock");
    requireError(
        Manager::fromManagerWireDeadline(
            (std::numeric_limits<std::int64_t>::max)(), clock),
        Domain::ErrorCodes::InvalidRequest,
        "maximum UTC inbound clock");

    clock.utc = Domain::UtcTimePoint{100s};
    clock.monotonic = Domain::MonotonicTimePoint::min();
    requireError(
        Manager::toManagerWireDeadline(
            context(Domain::MonotonicTimePoint::max()), clock),
        Domain::ErrorCodes::InvalidRequest,
        "extreme monotonic span");

    clock.utc = Domain::UtcTimePoint{
        -Domain::UtcTimePoint::duration{1}};
    clock.monotonic = Domain::MonotonicTimePoint{10s};
    requireError(
        Manager::toManagerWireDeadline(
            context(clock.monotonic + 1s), clock),
        Domain::ErrorCodes::InvalidRequest,
        "negative current UTC clock");
}

} // namespace

int main()
{
    try {
        testDefaultLimits();
        testWireMappingRoundsUpWithoutEarlyExpiry();
        testOutboundValidation();
        testInboundValidationAndBoundary();
        testExtremeClocksFailWithoutArithmeticOverflow();
        std::cout << "Manager deadline mapper tests passed: 5 groups\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Manager deadline mapper tests failed: "
                  << failure.what() << '\n';
        return 1;
    }
}
