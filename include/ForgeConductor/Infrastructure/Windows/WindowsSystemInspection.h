#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <string>

namespace ForgeConductor::Infrastructure::Windows {

// Native read-only inspection. Never returns command lines, environments or tokens.
class WindowsSystemInspection final {
public:
    [[nodiscard]] static Domain::Result<std::string> lmStudioDesktopVersion(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] static Domain::Result<std::string> inspect(
        const Domain::OperationContext& context) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
