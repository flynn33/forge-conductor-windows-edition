#pragma once

#include "ForgeConductor/Dashboard/DashboardSessionCloseRequest.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <span>

namespace ForgeConductor::Dashboard {

class DashboardSessionCloseRequestDecoder final {
public:
    // A 64 KiB request accommodates 4,000 Unicode scalar values even when
    // every non-BMP value is represented by a JSON surrogate-pair escape,
    // while keeping this flat administrative mutation substantially below the
    // dashboard transport's one MiB aggregate body ceiling.
    static constexpr std::size_t MaximumRequestBytes = 64U * 1024U;
    static constexpr std::size_t MaximumJsonNesting = 16U;

    [[nodiscard]] static Domain::Result<DashboardSessionCloseRequest> decode(
        std::span<const std::byte> body,
        std::size_t maximumBytes = MaximumRequestBytes) noexcept;
};

} // namespace ForgeConductor::Dashboard
