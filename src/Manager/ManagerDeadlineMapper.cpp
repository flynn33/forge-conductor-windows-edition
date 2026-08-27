#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"

#include "ForgeConductor/Domain/Error.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace ForgeConductor::Manager {
namespace {

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, message, retryable));
}

[[nodiscard]] bool validLimits(const ManagerTransportLimits& limits) noexcept
{
    return limits.maximumRequestLifetime > std::chrono::milliseconds::zero() &&
        limits.maximumRequestLifetime <=
            ManagerTransportLimits::DefaultMaximumRequestLifetime;
}

[[nodiscard]] Domain::Result<std::int64_t> checkedMilliseconds(
    const std::chrono::milliseconds value) noexcept
{
    using Count = decltype(value.count());
    constexpr auto Minimum = (std::numeric_limits<std::int64_t>::min)();
    constexpr auto Maximum = (std::numeric_limits<std::int64_t>::max)();
    if constexpr (std::numeric_limits<Count>::is_signed) {
        if (value.count() < static_cast<Count>(Minimum) ||
            value.count() > static_cast<Count>(Maximum)) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager deadline is outside the signed wire range.");
        }
    } else if (value.count() > static_cast<Count>(Maximum)) {
        return failure<std::int64_t>(
            Domain::ErrorCodes::InvalidRequest,
            "The manager deadline is outside the signed wire range.");
    }
    return Domain::Result<std::int64_t>::success(
        static_cast<std::int64_t>(value.count()));
}

} // namespace

Domain::Result<std::int64_t> toManagerWireDeadline(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock,
    const ManagerTransportLimits& limits) noexcept
{
    try {
        if (!validLimits(limits)) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InternalFailure,
                "The manager request lifetime limit is invalid.");
        }
        if (context.isCancellationRequested()) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::Cancelled,
                "The manager request was cancelled before transport.");
        }

        // Reading monotonic time first intentionally makes any time spent
        // obtaining UTC enlarge, rather than shorten, the wire deadline.
        const auto monotonicNow = clock.monotonicNow();
        if (context.isExpired(monotonicNow)) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The manager request deadline has expired.");
        }
        const auto maximumLocalOffset =
            std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(
                limits.maximumRequestLifetime);
        const auto maximumLocalDeadline =
            monotonicNow > Domain::MonotonicTimePoint::max() - maximumLocalOffset
            ? Domain::MonotonicTimePoint::max()
            : monotonicNow + maximumLocalOffset;
        if (context.deadline > maximumLocalDeadline) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager request deadline exceeds the lifetime bound.");
        }
        const auto remaining = context.deadline - monotonicNow;

        const auto utcNow = clock.utcNow();
        if (utcNow.time_since_epoch() <= Domain::UtcTimePoint::duration::zero()) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InvalidRequest,
                "The current UTC time is outside the positive wire range.");
        }
        const auto systemOffset =
            std::chrono::ceil<Domain::UtcTimePoint::duration>(remaining);
        if (systemOffset <= Domain::UtcTimePoint::duration::zero() ||
            utcNow > Domain::UtcTimePoint::max() - systemOffset) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager request deadline overflows the UTC clock.");
        }
        const auto utcFloor = std::chrono::floor<std::chrono::milliseconds>(
            utcNow.time_since_epoch());
        auto floorCount = checkedMilliseconds(utcFloor);
        if (!floorCount) {
            return floorCount;
        }

        const auto utcFraction = utcNow.time_since_epoch() - utcFloor;
        const auto roundedOffset = std::chrono::ceil<std::chrono::milliseconds>(
            utcFraction + remaining);
        auto offsetCount = checkedMilliseconds(roundedOffset);
        if (!offsetCount || offsetCount.value() <= 0) {
            return offsetCount
                ? failure<std::int64_t>(
                      Domain::ErrorCodes::DeadlineExceeded,
                      "The manager request deadline has expired.")
                : offsetCount;
        }
        const auto base = floorCount.value();
        const auto offset = offsetCount.value();
        if (base > (std::numeric_limits<std::int64_t>::max)() - offset) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager request deadline overflows the wire range.");
        }
        const auto wireDeadline = base + offset;
        if (wireDeadline <= 0) {
            return failure<std::int64_t>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager request deadline is not a positive UTC value.");
        }
        return Domain::Result<std::int64_t>::success(wireDeadline);
    } catch (...) {
        return failure<std::int64_t>(
            Domain::ErrorCodes::InternalFailure,
            "The manager request deadline could not be mapped to UTC.");
    }
}

Domain::Result<Domain::MonotonicTimePoint> fromManagerWireDeadline(
    const std::int64_t deadlineUtcMilliseconds,
    const Contracts::IClock& clock,
    const ManagerTransportLimits& limits) noexcept
{
    try {
        if (!validLimits(limits)) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InternalFailure,
                "The manager request lifetime limit is invalid.");
        }
        if (deadlineUtcMilliseconds < 0) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager wire deadline may not be negative.");
        }

        // Reading UTC first and monotonic time second prevents conversion work
        // from shortening the locally derived deadline.
        const auto utcNow = clock.utcNow();
        if (utcNow.time_since_epoch() <= Domain::UtcTimePoint::duration::zero()) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InvalidRequest,
                "The current UTC time is outside the positive wire range.");
        }
        const auto maximumSystemOffset =
            std::chrono::duration_cast<Domain::UtcTimePoint::duration>(
                limits.maximumRequestLifetime);
        if (utcNow > Domain::UtcTimePoint::max() - maximumSystemOffset) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager lifetime bound overflows the UTC clock.");
        }
        const auto utcFloor = std::chrono::floor<std::chrono::milliseconds>(
            utcNow.time_since_epoch());
        auto floorCount = checkedMilliseconds(utcFloor);
        if (!floorCount) {
            return failure<Domain::MonotonicTimePoint>(
                floorCount.error().code,
                "The current UTC time is outside the manager wire range.");
        }
        const auto currentFloor = floorCount.value();
        if (deadlineUtcMilliseconds <= currentFloor) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The manager wire deadline has expired.");
        }

        const auto utcFraction = utcNow.time_since_epoch() - utcFloor;
        const auto maximumWireOffset =
            std::chrono::ceil<std::chrono::milliseconds>(
                utcFraction + limits.maximumRequestLifetime);
        auto maximumOffsetCount = checkedMilliseconds(maximumWireOffset);
        if (!maximumOffsetCount ||
            currentFloor > (std::numeric_limits<std::int64_t>::max)() -
                maximumOffsetCount.value()) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager lifetime bound overflows the wire range.");
        }
        const auto maximumWire = currentFloor + maximumOffsetCount.value();
        if (deadlineUtcMilliseconds > maximumWire) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager wire deadline exceeds the lifetime bound.");
        }

        // Subtraction is safe after the ordered comparisons above: the result
        // is bounded by the configured request lifetime plus millisecond
        // representation rounding.
        const auto deltaMilliseconds =
            deadlineUtcMilliseconds - currentFloor;
        const auto remaining =
            std::chrono::milliseconds{deltaMilliseconds} - utcFraction;
        if (remaining <= decltype(remaining)::zero()) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::DeadlineExceeded,
                "The manager wire deadline has expired.");
        }

        const auto localOffset =
            std::chrono::ceil<Domain::MonotonicTimePoint::duration>(remaining);
        const auto monotonicNow = clock.monotonicNow();
        if (localOffset <= Domain::MonotonicTimePoint::duration::zero() ||
            monotonicNow >
                Domain::MonotonicTimePoint::max() - localOffset) {
            return failure<Domain::MonotonicTimePoint>(
                Domain::ErrorCodes::InvalidRequest,
                "The manager wire deadline overflows the local clock.");
        }
        return Domain::Result<Domain::MonotonicTimePoint>::success(
            monotonicNow + localOffset);
    } catch (...) {
        return failure<Domain::MonotonicTimePoint>(
            Domain::ErrorCodes::InternalFailure,
            "The manager wire deadline could not be mapped locally.");
    }
}

} // namespace ForgeConductor::Manager
