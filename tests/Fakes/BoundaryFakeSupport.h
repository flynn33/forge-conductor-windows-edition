#pragma once

#include "ForgeConductor/Domain/OperationContext.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ForgeConductor::Tests::Fakes {

inline constexpr std::size_t DefaultBoundaryCaptureBytesMaximum = 64 * 1024;
inline constexpr std::size_t DefaultBoundaryCaptureItemsMaximum = 64;
inline constexpr std::size_t DefaultBoundaryCaptureTextBytesMaximum = 4 * 1024;

class DeterministicBoundaryState final {
public:
    explicit DeterministicBoundaryState(
        const Domain::MonotonicTimePoint now = {}) noexcept
        : now_{now}
    {
    }

    [[nodiscard]] Domain::Result<void> begin(
        const Domain::OperationContext& context) noexcept
    {
        try {
            ++calls_;
            lastContext_ = context;
            if (shutdown_ || context.isCancellationRequested()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic boundary operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic boundary operation deadline expired."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic boundary context could not be captured."));
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }
    void shutdown() noexcept { shutdown_ = true; }

    [[nodiscard]] Domain::MonotonicTimePoint now() const noexcept { return now_; }
    [[nodiscard]] bool isShutdown() const noexcept { return shutdown_; }
    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return lastContext_;
    }

private:
    Domain::MonotonicTimePoint now_{};
    std::optional<Domain::OperationContext> lastContext_;
    std::size_t calls_{};
    bool shutdown_{};
};

[[nodiscard]] inline Domain::Error boundaryPayloadTooLarge(
    const std::string_view message)
{
    return Domain::makeError(Domain::ErrorCodes::PayloadTooLarge, std::string{message});
}

[[nodiscard]] inline Domain::Error boundaryLimitExceeded(
    const std::string_view message)
{
    return Domain::makeError(Domain::ErrorCodes::LimitExceeded, std::string{message});
}

} // namespace ForgeConductor::Tests::Fakes
