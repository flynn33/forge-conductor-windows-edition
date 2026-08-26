#include "ForgeConductor/Infrastructure/Windows/DeadlineScheduler.h"

#include "Detail/OperationContextGuard.h"

#include <utility>

namespace ForgeConductor::Infrastructure::Windows {

DeadlineScheduler::DeadlineScheduler(const Contracts::IClock& clock) noexcept
    : clock_{clock}
{
}

Domain::Result<void> DeadlineScheduler::waitUntil(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (shutdown_.load(std::memory_order_acquire) ||
            context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The deadline gate is shut down or the operation was cancelled."));
        }

        auto validation = Detail::validateOperationContext(
            context,
            clock_.monotonicNow(),
            "evaluate the operation deadline");
        if (!validation) {
            return validation;
        }

        if (shutdown_.load(std::memory_order_acquire) ||
            context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The deadline gate was shut down while evaluating the operation."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deadline gate could not evaluate the operation context."));
    }
}

void DeadlineScheduler::shutdown() noexcept
{
    shutdown_.store(true, std::memory_order_release);
}

} // namespace ForgeConductor::Infrastructure::Windows
