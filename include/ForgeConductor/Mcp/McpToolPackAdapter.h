#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILegacyContextContinuityService.h"
#include "ForgeConductor/Contracts/ILegacyMemoryService.h"
#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "ForgeConductor/Contracts/IPathGlobService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"
#include "ForgeConductor/Contracts/IToolServices.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace ForgeConductor::Mcp {

// All product services used by the application-owned MCP tool packs. The
// composition root owns every referenced dependency for longer than the
// adapter and supplies the exact configured PowerShell executable rather than
// permitting ambient executable discovery at the protocol boundary.
struct McpToolPackDependencies final {
    Contracts::IToolCatalog& catalog;
    Contracts::IApplicationPaths& applicationPaths;
    Contracts::IAgentCatalog& agentCatalog;
    Contracts::IAgentSessionService& agentSessions;
    Contracts::IAgentCompletionReportInspector& reportInspector;
    Contracts::ILegacyContextContinuityService& legacyContinuity;
    Contracts::IWorkspaceAuthority& workspaceAuthority;
    Contracts::IFileSystem& fileSystem;
    Contracts::ITextFileEditService& textFileEditor;
    Contracts::IPathGlobService& pathGlob;
    Contracts::IGitService& git;
    Contracts::ILegacyMemoryService& legacyMemory;
    Contracts::IPdfService& pdf;
    Contracts::ITextSearchService& textSearch;
    Contracts::IShellService& shell;
    Contracts::IProjectRegistryRepository& projectRegistry;
    Contracts::IProjectMemoryService& projectMemory;
    Contracts::IContinuityCoordinator& continuity;
    Contracts::IContinuityDocumentCodec& continuityCodec;
    Contracts::IContinuityAutomation& continuityAutomation;
    Contracts::ITelemetryService& telemetry;
    Contracts::IClock& clock;
    Contracts::IUuidGenerator& uuidGenerator;
    Domain::ProjectMemoryLimits projectMemoryLimits;
    Domain::PathText shellExecutable;
    std::string productVersion;
    std::string runtimeName;
    std::uint32_t processId{};
};

// Parses source-compatible tool arguments into transport-neutral Domain
// requests and converts typed service outcomes back to deterministic JSON
// payloads. It owns no product state and performs no platform work directly.
class McpToolPackAdapter final : public Contracts::IToolHandler {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<McpToolPackAdapter>>
    create(McpToolPackDependencies dependencies) noexcept;

    ~McpToolPackAdapter() noexcept override;

    McpToolPackAdapter(const McpToolPackAdapter&) = delete;
    McpToolPackAdapter& operator=(const McpToolPackAdapter&) = delete;
    McpToolPackAdapter(McpToolPackAdapter&&) = delete;
    McpToolPackAdapter& operator=(McpToolPackAdapter&&) = delete;

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept override;

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> handle(
        const Contracts::AuthorizedToolCall& authorizedCall,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

private:
    class Impl;

    explicit McpToolPackAdapter(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Mcp
