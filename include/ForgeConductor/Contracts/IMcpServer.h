#pragma once

#include "ForgeConductor/Contracts/IMcpTransport.h"

namespace ForgeConductor::Contracts {

class IMcpServer {
public:
    virtual ~IMcpServer() = default;

    [[nodiscard]] virtual Domain::Result<void> run(
        IMcpTransport& transport,
        Domain::McpRole role,
        const Domain::DeploymentId& deploymentId,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
