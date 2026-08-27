#pragma once

#include "ForgeConductor/Domain/LegacyContinuityModels.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <optional>

namespace ForgeConductor::Contracts {

// Owns the bounded, process-local workspace recovered for each MCP client.
// Implementations retain only canonical roots proven by workspace authority.
class IMcpClientWorkspaceContext {
public:
    virtual ~IMcpClientWorkspaceContext() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ClientWorkspaceAdoption> adopt(
        const Domain::ClientId& clientId,
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<
        std::optional<Domain::ClientWorkspaceSnapshot>>
    snapshot(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void clear(const Domain::ClientId& clientId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
