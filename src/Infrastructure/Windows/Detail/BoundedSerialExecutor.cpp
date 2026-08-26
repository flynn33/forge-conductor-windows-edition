#include "BoundedSerialExecutor.h"

#include <chrono>
#include <stop_token>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error closedError(const std::string_view action)
{
    return Domain::makeError(Domain::ErrorCodes::TransportClosed,
                             std::string{action} +
                                 " cannot start after its owner begins shutdown.");
}

[[nodiscard]] Domain::Error cancelledError(const std::string_view action)
{
    return Domain::makeError(Domain::ErrorCodes::Cancelled,
                             std::string{action} +
                                 " was cancelled while waiting for serialized ownership.");
}

[[nodiscard]] Domain::Error deadlineError(const std::string_view action)
{
    return Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                             std::string{action} +
                                 " exceeded its deadline while waiting for serialized ownership.");
}

} // namespace

BoundedSerialExecutor::Lease::Lease(BoundedSerialExecutor& owner) noexcept : owner_{&owner} {}

BoundedSerialExecutor::Lease::Lease(Lease&& other) noexcept
    : owner_{std::exchange(other.owner_, nullptr)}
{
}

BoundedSerialExecutor::Lease::~Lease() { release(); }

void BoundedSerialExecutor::Lease::release() noexcept
{
    if (owner_ != nullptr) {
        owner_->release();
        owner_ = nullptr;
    }
}

BoundedSerialExecutor::~BoundedSerialExecutor() { shutdown(); }

Domain::Result<BoundedSerialExecutor::Lease>
BoundedSerialExecutor::acquire(const Domain::OperationContext& context,
                               const std::string_view action) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        if (shutdownRequested_) {
            return Domain::Result<Lease>::failure(closedError(action));
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<Lease>::failure(cancelledError(action));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Lease>::failure(deadlineError(action));
        }
        if ((active_ ? 1U : 0U) + waiting_ >= MaximumPendingOperationCount) {
            return Domain::Result<Lease>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                std::string{action} + " exceeds the bounded serialized-operation queue."));
        }

        if (!active_) {
            active_ = true;
            return Domain::Result<Lease>::success(Lease{*this});
        }

        ++waiting_;
        std::stop_callback cancellationWake{context.cancellation,
                                            [this]() noexcept { condition_.notify_all(); }};

        while (active_ && !shutdownRequested_ && !context.isCancellationRequested()) {
            if (condition_.wait_until(lock, context.deadline) == std::cv_status::timeout) {
                break;
            }
        }
        --waiting_;
        condition_.notify_all();

        if (shutdownRequested_) {
            return Domain::Result<Lease>::failure(closedError(action));
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<Lease>::failure(cancelledError(action));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Lease>::failure(deadlineError(action));
        }

        active_ = true;
        return Domain::Result<Lease>::success(Lease{*this});
    } catch (...) {
        return Domain::Result<Lease>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              std::string{action} + " could not acquire serialized ownership."));
    }
}

void BoundedSerialExecutor::shutdown() noexcept
{
    beginShutdown();
    try {
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this]() noexcept { return !active_ && waiting_ == 0U; });
    } catch (...) {
        // Destruction and shutdown cannot surface exceptions across the boundary.
    }
}

void BoundedSerialExecutor::beginShutdown() noexcept
{
    try {
        std::lock_guard lock{mutex_};
        shutdownRequested_ = true;
        condition_.notify_all();
    } catch (...) {
    }
}

bool BoundedSerialExecutor::waitUntilIdle(const std::chrono::milliseconds timeout) noexcept
{
    try {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout,
                                   [this]() noexcept { return !active_ && waiting_ == 0U; });
    } catch (...) {
        return false;
    }
}

void BoundedSerialExecutor::release() noexcept
{
    std::lock_guard lock{mutex_};
    active_ = false;
    condition_.notify_all();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
