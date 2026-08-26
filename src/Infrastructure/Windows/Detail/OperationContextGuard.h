#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

[[nodiscard]] Domain::Result<void> validateOperationContext(
    const Domain::OperationContext& context,
    Domain::MonotonicTimePoint now,
    std::string_view action) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
