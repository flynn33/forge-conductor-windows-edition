#pragma once

#include "ForgeConductor/Domain/ManagerStartupModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

namespace ForgeConductor::Contracts {

// Platform boundary for the one per-user Manager startup registration. The
// implementation owns registration serialization but never owns product state.
class IManagerStartupService {
public:
    virtual ~IManagerStartupService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStartupStatus> inspect(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStartupOutcome>
    registerAtLogon(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStartupOutcome> repair(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStartupOutcome>
    setEnabled(
        const Domain::ManagerStartupDefinition& expected,
        bool enabled,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStartupOutcome> startNow(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStartupOutcome> remove(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
