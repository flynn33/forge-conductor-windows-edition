#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IMcpClientWorkspaceContext.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Mcp {

// Maps hostile continuity paths to registered, authorized project aliases and
// retains one canonical authority root per bounded MCP client.
class McpClientWorkspaceContext final
    : public Contracts::IMcpClientWorkspaceContext {
public:
    static constexpr std::size_t MaximumTrackedClients = 128U;
    static constexpr std::size_t MaximumRegisteredProjects = 1'024U;
    static constexpr std::size_t MaximumAliasesPerProject = 32U;
    static constexpr std::size_t MaximumRecoveryCandidates = 32U;

    McpClientWorkspaceContext(
        Contracts::IProjectRegistryRepository& projectRegistry,
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        const Contracts::IClock& clock);
    ~McpClientWorkspaceContext() noexcept override;

    McpClientWorkspaceContext(const McpClientWorkspaceContext&) = delete;
    McpClientWorkspaceContext& operator=(
        const McpClientWorkspaceContext&) = delete;
    McpClientWorkspaceContext(McpClientWorkspaceContext&&) = delete;
    McpClientWorkspaceContext& operator=(McpClientWorkspaceContext&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ClientWorkspaceAdoption> adopt(
        const Domain::ClientId& clientId,
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ClientWorkspaceSnapshot>>
    snapshot(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;

    void clear(const Domain::ClientId& clientId) noexcept override;
    void shutdown() noexcept override;

    [[nodiscard]] std::size_t trackedClientCount() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace ForgeConductor::Mcp
