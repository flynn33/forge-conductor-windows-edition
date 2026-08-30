#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

namespace ForgeConductor::Contracts {

// Platform-neutral boundary for exactly one bounded Manager-owned
// reconciliation pass. Implementations compose LM Studio and continuity work
// later; scheduling, overlap prevention, and process lifetime stay with the
// Manager host worker.
class IManagerMaintenanceService {
public:
    virtual ~IManagerMaintenanceService() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> reconcile(
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
