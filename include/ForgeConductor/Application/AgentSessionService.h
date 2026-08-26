#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <chrono>
#include <memory>

namespace ForgeConductor::Application {

// The injected catalog, repository, report inspector, workspace authority,
// clock, and UUID generator must outlive this service. The service owns only
// bounded in-memory binding projections. Its synchronous operation admission
// is drained before repository close.
class AgentSessionService final : public Contracts::IAgentSessionService {
public:
    AgentSessionService(
        Contracts::IAgentCatalog& catalog,
        Contracts::IAgentSessionRepository& repository,
        Contracts::IAgentCompletionReportInspector& reportInspector,
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator,
        std::chrono::seconds idleTtl =
            Domain::AgentSessionLimits::DefaultIdleTtl);
    ~AgentSessionService() noexcept override;

    AgentSessionService(const AgentSessionService&) = delete;
    AgentSessionService& operator=(const AgentSessionService&) = delete;
    AgentSessionService(AgentSessionService&&) = delete;
    AgentSessionService& operator=(AgentSessionService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::AgentSession> start(
        const Domain::AgentId& agentId,
        const std::optional<Domain::ClientId>& clientId,
        std::string_view goal,
        const std::optional<Domain::PathText>& workingDirectory,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentSession> status(
        const Domain::SessionId& sessionId,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentSession> complete(
        const Domain::SessionId& sessionId,
        std::string_view summary,
        bool succeeded,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::size_t> pruneStale(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentRunStartOutcome> startRun(
        const Domain::AgentRunStartRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentRunStatusOutcome> runStatus(
        const Domain::AgentRunStatusRequest& request,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentRunCompleteOutcome> completeRun(
        const Domain::AgentRunCompleteRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> attach(
        const Domain::AgentRunReattachRequest& request,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> rehydrate(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::optional<Domain::ActiveBinding>> binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<bool> touchIfActive(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
