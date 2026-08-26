#include "ForgeConductor/Domain/DeploymentModels.h"

namespace ForgeConductor::Domain {

Result<void> validateDeploymentRequest(
    const DeploymentRequest& request,
    const std::string_view expectedPurgeScope,
    const std::string_view expectedPurgeToken)
{
    if (request.action != DeploymentAction::Purge) {
        if (!request.preserveUserData || request.confirmation) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Only an explicit purge may remove user data or carry purge confirmation."));
        }
        return Result<void>::success();
    }

    if (request.preserveUserData) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "A purge request cannot preserve the data it is explicitly removing."));
    }
    if (!request.confirmation) {
        return Result<void>::failure(makeError(
            ErrorCodes::Unauthorized,
            "A purge request requires typed destructive confirmation."));
    }
    return validateDestructiveConfirmation(
        *request.confirmation,
        "purge",
        expectedPurgeScope,
        expectedPurgeToken);
}

} // namespace ForgeConductor::Domain
