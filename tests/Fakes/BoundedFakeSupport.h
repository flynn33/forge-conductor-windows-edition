#pragma once

#include "ForgeConductor/Domain/OperationContext.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

enum class FakeLifecycleState { Open, Closed, Shutdown };

class BoundedFakeOperationGate final {
public:
    explicit BoundedFakeOperationGate(
        const Domain::MonotonicTimePoint now = {}) noexcept
        : now_{now}
    {
    }

    [[nodiscard]] Domain::Result<void> enter(
        const Domain::OperationContext& context) noexcept
    {
        try {
            ++calls_;
            lastContext_ = context;
            if (state_ == FakeLifecycleState::Closed) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The deterministic repository boundary is closed."));
            }
            if (state_ == FakeLifecycleState::Shutdown ||
                context.isCancellationRequested() ||
                (cancelledOperation_ &&
                 cancelledOperation_.value() == context.operationId)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic operation deadline expired."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic operation context could not be recorded."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            cancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void close() noexcept { state_ = FakeLifecycleState::Closed; }
    void shutdown() noexcept { state_ = FakeLifecycleState::Shutdown; }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

    [[nodiscard]] FakeLifecycleState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return lastContext_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    cancelledOperation() const noexcept
    {
        return cancelledOperation_;
    }

private:
    std::optional<Domain::OperationContext> lastContext_;
    std::optional<Domain::OperationId> cancelledOperation_;
    Domain::MonotonicTimePoint now_{};
    std::size_t calls_{};
    FakeLifecycleState state_{FakeLifecycleState::Open};
};

template <typename T>
[[nodiscard]] Domain::Result<T> propagateFakeGateFailure(
    Domain::Result<void> accepted) noexcept
{
    return Domain::Result<T>::failure(std::move(accepted).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> fakeInternalFailure() noexcept
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The deterministic fake could not retain its bounded state."));
}

} // namespace ForgeConductor::Tests::Fakes
