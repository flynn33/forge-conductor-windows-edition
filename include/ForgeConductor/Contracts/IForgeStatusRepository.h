#pragma once

#include "ForgeConductor/Domain/ForgeStatusModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

namespace ForgeConductor::Contracts {

// Read-only central-store projection required by forge_status. Hardware,
// process, history, and delivery telemetry remain outside this P14 contract.
class IForgeStatusRepository {
public:
    virtual ~IForgeStatusRepository() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ForgeStatusProjection> snapshot(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
