#include "OperationContextGuard.h"

#include <string>

namespace ForgeConductor::Infrastructure::Windows::Detail {

Domain::Result<void> validateOperationContext(
    const Domain::OperationContext& context,
    const Domain::MonotonicTimePoint now,
    const std::string_view action) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The operation was cancelled before Forge Conductor could " +
                    std::string{action} + "."));
        }
        if (context.isExpired(now)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The operation deadline expired before Forge Conductor could " +
                    std::string{action} + "."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The operation context could not be validated."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
