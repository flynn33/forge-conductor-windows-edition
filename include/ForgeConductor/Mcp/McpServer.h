#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IMcpServer.h"
#include "ForgeConductor/Contracts/IToolServices.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Mcp {

class McpServer final : public Contracts::IMcpServer {
public:
    static constexpr std::size_t MaximumPendingToolCalls = 64U;
    static constexpr std::size_t MaximumPreCancellationIds = 256U;
    static constexpr std::size_t MaximumRequestIdBytes = 256U;
    static constexpr std::size_t MaximumMethodNameBytes = 128U;
    static constexpr std::size_t MaximumToolNameBytes = 128U;

    McpServer(
        Contracts::IToolCatalog& catalog,
        Contracts::IToolRouter& router,
        Contracts::IMcpExecutionContextResolver& contextResolver,
        Contracts::IUuidGenerator& uuidGenerator,
        const Contracts::IClock& clock);
    ~McpServer() noexcept override;

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;
    McpServer(McpServer&&) = delete;
    McpServer& operator=(McpServer&&) = delete;

    [[nodiscard]] Domain::Result<void> run(
        Contracts::IMcpTransport& transport,
        Domain::McpRole role,
        const Domain::DeploymentId& deploymentId,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace ForgeConductor::Mcp
