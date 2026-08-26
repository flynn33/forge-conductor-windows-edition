#pragma once

#include "ForgeConductor/Contracts/AuthorizedToolCall.h"
#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/ToolModels.h"
#include <cstddef>

#include <optional>
#include <string_view>
#include <vector>

namespace ForgeConductor::Contracts {

class IAgentCatalog {
public:
    virtual ~IAgentCatalog() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::AgentSpec>> all(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::AgentSpec>> get(
        const Domain::AgentId& agentId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentSpec> recommend(
        std::string_view task,
        const Domain::OperationContext& context) noexcept = 0;
};

// JSON decoding remains below the Application layer. Implementations inspect
// the canonical report object and return independently derived top-level field
// metadata so callers cannot forge schema-completeness evidence.
class IAgentCompletionReportInspector {
public:
    virtual ~IAgentCompletionReportInspector() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::AgentReportField>>
    inspect(
        std::string_view canonicalJson,
        const Domain::OperationContext& context) noexcept = 0;
};

class IAgentSessionRepository {
public:
    virtual ~IAgentSessionRepository() = default;

    [[nodiscard]] virtual Domain::Result<void> save(
        const Domain::AgentSession& session,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::AgentSession>> get(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::AgentSession>> list(
        const std::optional<Domain::AgentId>& agentId,
        const std::optional<Domain::SessionStatus>& status,
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunStartPersistenceOutcome>
    startRun(
        const Domain::AgentRunStartMutation& mutation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::AgentRunRecord>>
    getRun(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunReattachOutcome>
    reattachRun(
        const Domain::AgentRunReattachMutation& mutation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunCompletePersistenceOutcome>
    completeRun(
        const Domain::AgentRunCompleteMutation& mutation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<bool> touchRun(
        const Domain::SessionId& sessionId,
        Domain::UtcTimePoint touchedAt,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::AgentRunRecord>>
    latestOpenRun(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunRecoveryOutcome> recoverRun(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentProjectionRepairOutcome>
    repairProjection(
        const Domain::AgentProjectionRepairRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentStaleCloseOutcome> closeStale(
        const Domain::AgentStaleCloseRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

class IAgentSessionService {
public:
    virtual ~IAgentSessionService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::AgentSession> start(
        const Domain::AgentId& agentId,
        const std::optional<Domain::ClientId>& clientId,
        std::string_view goal,
        const std::optional<Domain::PathText>& workingDirectory,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentSession> status(
        const Domain::SessionId& sessionId,
        const WorkspaceAuthority& mutationAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentSession> complete(
        const Domain::SessionId& sessionId,
        std::string_view summary,
        bool succeeded,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::size_t> pruneStale(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunStartOutcome> startRun(
        const Domain::AgentRunStartRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunStatusOutcome> runStatus(
        const Domain::AgentRunStatusRequest& request,
        const WorkspaceAuthority& mutationAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunCompleteOutcome> completeRun(
        const Domain::AgentRunCompleteRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunReattachOutcome> attach(
        const Domain::AgentRunReattachRequest& request,
        const WorkspaceAuthority& mutationAuthority,
        const AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::AgentRunRecoveryOutcome> rehydrate(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::ActiveBinding>> binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<bool> touchIfActive(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
