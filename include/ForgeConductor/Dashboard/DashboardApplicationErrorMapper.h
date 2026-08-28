#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"
#include "ForgeConductor/Domain/Error.h"

#include <cstdint>

namespace ForgeConductor::Dashboard {

enum class DashboardApplicationErrorOrigin : std::uint8_t {
    RequestBody,
    Dependency,
    SseSubscription,
    ResponseEncoding,
};

// Converts typed internal failures to bounded, stable HTTP-safe metadata.
// Arbitrary dependency messages and evidence identifiers never cross the
// dashboard boundary.
class DashboardApplicationErrorMapper final {
public:
    [[nodiscard]] static DashboardHttpRejection map(
        const Domain::Error& error,
        DashboardApplicationErrorOrigin origin) noexcept;
};

} // namespace ForgeConductor::Dashboard
