#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"

#include <cstddef>
#include <span>

namespace ForgeConductor::Dashboard {

class DashboardHttpParser final {
public:
    static constexpr std::size_t MaximumHeaderBytes = 32U * 1024U;
    static constexpr std::size_t MaximumHeaderCount = 64U;
    static constexpr std::size_t MaximumTargetBytes = 8U * 1024U;
    static constexpr std::size_t MaximumBodyBytes = 1024U * 1024U;

    [[nodiscard]] DashboardHttpParseResult parse(
        std::span<const std::byte> buffer,
        bool streamComplete) const noexcept;
};

} // namespace ForgeConductor::Dashboard
