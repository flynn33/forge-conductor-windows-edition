#pragma once

#include "ForgeConductor/Domain/LegacyContinuityModels.h"

#include <vector>

namespace ForgeConductor::Contracts {

// Human-readable legacy files are post-commit projections. Implementations
// must make a lower sequence unable to replace a newer LATEST/current-task
// projection when concurrent writers finish out of order.
class IContinuityProjectionStore {
public:
    virtual ~IContinuityProjectionStore() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityProjectionReceipt>
    write(
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
    repair(
        const std::vector<Domain::LegacyContinuityRecord>& records,
        const Domain::LegacyContinuityPointerRepairOutcome& pointers,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::size_t> reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
