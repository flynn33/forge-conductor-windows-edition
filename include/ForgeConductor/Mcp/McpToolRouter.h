#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IToolServices.h"

#include <cstddef>
#include <memory>
#include <span>

namespace ForgeConductor::Mcp {

// Owns the exact catalog-to-handler registration map and enforces authority,
// loop policy, cancellation, and privacy-safe audit boundaries for tool calls.
class McpToolRouter final : public Contracts::IToolRouter {
public:
    static constexpr std::size_t MaximumRegisteredTools = 64U;
    static constexpr std::size_t MaximumActiveOperations = 64U;
    static constexpr std::size_t MaximumToolNameBytes = 128U;
    static constexpr std::size_t MaximumProtocolVersionBytes = 64U;
    static constexpr std::size_t MaximumCatalogBytes = 1'048'576U;
    static constexpr std::size_t MaximumCanonicalArgumentsBytes = 1'048'576U;

    [[nodiscard]] static Domain::Result<std::unique_ptr<McpToolRouter>> create(
        Contracts::IToolCatalog& catalog,
        std::span<Contracts::IToolHandler* const> handlers,
        Contracts::IToolAuthorizer& authorizer,
        Contracts::IToolInvocationGuard& invocationGuard,
        Contracts::IAuditRepository& auditRepository,
        Contracts::IHasher& hasher,
        Contracts::IClock& clock) noexcept;

    ~McpToolRouter() noexcept override;

    McpToolRouter(const McpToolRouter&) = delete;
    McpToolRouter& operator=(const McpToolRouter&) = delete;
    McpToolRouter(McpToolRouter&&) = delete;
    McpToolRouter& operator=(McpToolRouter&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

    [[nodiscard]] std::size_t activeOperationCount() const noexcept;

private:
    class Implementation;

    explicit McpToolRouter(
        std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;
};

} // namespace ForgeConductor::Mcp
