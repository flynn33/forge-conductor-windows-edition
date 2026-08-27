#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Manager/ManagerTransportLimits.h"

#include <cstdint>

namespace ForgeConductor::Manager {

[[nodiscard]] Domain::Result<std::int64_t> toManagerWireDeadline(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock,
    const ManagerTransportLimits& limits = {}) noexcept;

[[nodiscard]] Domain::Result<Domain::MonotonicTimePoint>
fromManagerWireDeadline(
    std::int64_t deadlineUtcMilliseconds,
    const Contracts::IClock& clock,
    const ManagerTransportLimits& limits = {}) noexcept;

} // namespace ForgeConductor::Manager
