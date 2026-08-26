#include "ForgeConductor/Domain/GraphicsModels.h"

#include <cmath>

namespace ForgeConductor::Domain {

Result<void> validateRenderRequest(const RenderRequest& request)
{
    if (request.widthPixels == 0 || request.heightPixels == 0 ||
        !std::isfinite(request.scale) || request.scale <= 0.0 || request.scale > 8.0) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Render dimensions and scale must be positive and bounded."));
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
