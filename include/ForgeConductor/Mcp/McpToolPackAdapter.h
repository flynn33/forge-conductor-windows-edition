#pragma once

#include "ForgeConductor/Contracts/IScheduledTaskService.h"

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IEvidenceService.h"
#include "ForgeConductor/Contracts/IGitHubReadService.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"
#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IForgeStatusRepository.h"
#include "ForgeConductor/Contracts/ILegacyContextContinuityService.h"
#include "ForgeConductor/Contracts/ILegacyMemoryService.h"
#include "ForgeConductor/Contracts/IMcpClientWorkspaceContext.h"
#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "ForgeConductor/Contracts/IPathGlobService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/IProjectPolicyService.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include "ForgeConductor/Contracts/IWebAccessService.h"
#include "ForgeConductor/Contracts/IArtifactDocumentService.h"
#include "ForgeConductor/Contracts/IDesktopArtifactService.h"
#include "ForgeConductor/Contracts/IImageProviderService.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>
#include <span>
#include <string>

namespace ForgeConductor::Mcp {

// All product services used by the application-owned MCP tool packs. The
// composition root owns every referenced dependency for longer than the
// adapter and supplies the configured PowerShell timeout and exact executable
// rather than permitting ambient defaults or discovery at the protocol boundary.
struct McpToolPackDependencies final {
    Contracts::IToolCatalog& catalog;
    Contracts::IApplicationPaths& applicationPaths;
    Contracts::IAgentCatalog& agentCatalog;
    Contracts::IAgentSessionService& agentSessions;
    Contracts::IAgentCompletionReportInspector& reportInspector;
    Contracts::ILegacyContextContinuityService& legacyContinuity;
    Contracts::IMcpClientWorkspaceContext& clientWorkspaceContext;
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
    Contracts::IContinuityAutomationStatusSource& continuityAutomationStatus;
    Contracts::IForgeStatusRepository& forgeStatus;
    Contracts::IClock& clock;
    Contracts::IUuidGenerator& uuidGenerator;
    Contracts::IHasher& hasher;
    Domain::ProjectMemoryLimits projectMemoryLimits;
    std::chrono::seconds shellDefaultTimeout;
    Domain::PathText shellExecutable;
    std::string productVersion;
    std::string runtimeName;
    std::uint32_t processId{};
    Contracts::IProjectPolicyService* projectPolicy{};
    std::string startupBindingSource{"unspecified"};
    std::function<std::string()> visibleChatContinuityStatus;
    std::function<void(std::string_view, bool, std::string_view)> visibleChatToolResult;
    std::function<void(const Domain::ProjectId&, const Domain::PathText&)> visibleChatWorkspaceBinding;
    std::function<Domain::Result<std::string>(const Domain::OperationContext&)> providerInspection;
    std::function<Domain::Result<std::string>(const Domain::OperationContext&)> systemInspection;
    Contracts::IGitHubReadService* githubRead{};
    Contracts::IEvidenceService* evidence{};
    // CLI forwards only these explicitly brokered tools to the authenticated,
    // same-profile Manager. Manager composition leaves this callback empty.
    std::function<Domain::Result<std::string>(std::string_view, std::string_view,
        const Domain::ProjectId&, const Domain::OperationContext&)> durableToolBroker;
    std::function<Contracts::IManagedRunService*()> reviewerRuns;
    Contracts::IWebAccessService* webAccess{};
    Contracts::IArtifactDocumentService* artifactDocuments{};
    Contracts::IDesktopArtifactService* desktopArtifacts{};
    std::function<Contracts::IManagedRunService*()> workerRuns;
    std::function<Contracts::IScheduledTaskService*()> scheduledTasks;
    std::string managerStartupError;
    Contracts::IImageProviderService* imageProvider{};
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

    // Produces the exact project, ordered instruction-package, and development-
    // policy locations advertised during MCP initialization. The same bounded
    // projection is returned by forge_status so clients do not have to infer
    // workspace identity from continuity packets or the application data root.
    [[nodiscard]] Domain::Result<std::string> bootstrapInstructions(
        const Domain::ProjectId& projectId,
        const Domain::PathText& projectRoot,
        const Domain::OperationContext& context) noexcept;

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
