#include "ForgeConductor/Domain/ForgeStatusModels.h"

#include <string_view>
#include <unordered_set>

namespace ForgeConductor::Domain {

Result<void> validateForgeStatusProjection(
    const ForgeStatusProjection& projection) noexcept
{
    try {
        if (projection.openSessionIds.size() >
            ForgeStatusLimits::MaximumOpenSessionIds) {
            return Result<void>::failure(makeError(
                ErrorCodes::LimitExceeded,
                "The Forge status projection exceeds the open-session bound."));
        }

        std::unordered_set<std::string_view> uniqueIds;
        uniqueIds.reserve(projection.openSessionIds.size());
        for (const auto& sessionId : projection.openSessionIds) {
            if (!uniqueIds.insert(sessionId.value()).second) {
                return Result<void>::failure(makeError(
                    ErrorCodes::IntegrityFailure,
                    "The Forge status projection contains duplicate session IDs."));
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The Forge status projection could not be validated safely."));
    }
}

} // namespace ForgeConductor::Domain
