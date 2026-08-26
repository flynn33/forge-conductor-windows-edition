#pragma once

#include "Fakes/ApplicationServiceFakes.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/McpTransportFake.h"
#include "Fakes/ToolServiceFakes.h"
#include "PlatformBoundaryFakeTestSupport.h"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ForgeConductor::Tests {

namespace ApplicationServiceFakeContractTestsDetail {

namespace Support = PlatformBoundaryTestSupport;
namespace Fakes = ForgeConductor::Tests::Fakes;

[[nodiscard]] inline Domain::AgentId agentId()
{
    return Support::parsed<Domain::AgentId>("application-agent");
}

[[nodiscard]] inline Domain::SessionId sessionId()
{
    return Support::parsed<Domain::SessionId>(
        "50505050-5050-4050-8050-505050505050");
}

[[nodiscard]] inline Domain::SessionId otherSessionId()
{
    return Support::parsed<Domain::SessionId>(
        "60606060-6060-4060-8060-606060606060");
}

[[nodiscard]] inline Domain::RequestId requestId()
{
    return Support::parsed<Domain::RequestId>("application-service-request");
}

[[nodiscard]] inline Domain::RequestId otherRequestId()
{
    return Support::parsed<Domain::RequestId>(
        "application-service-request-other");
}

[[nodiscard]] inline Domain::ProviderSessionId providerSessionId()
{
    return Support::parsed<Domain::ProviderSessionId>("provider-session");
}

[[nodiscard]] inline Domain::DeploymentId deploymentId()
{
    return Support::parsed<Domain::DeploymentId>("application-deployment");
}

[[nodiscard]] inline Domain::AuthorityId otherAuthorityId()
{
    return Support::parsed<Domain::AuthorityId>(
        "90909090-9090-4090-8090-909090909090");
}

[[nodiscard]] inline Domain::AgentSpec agentSpec()
{
    return Domain::AgentSpec{
        agentId(),
        "Agent",
        "",
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        "",
        "builtin"};
}

[[nodiscard]] inline Domain::AgentSession agentSession()
{
    return Domain::AgentSession{
        sessionId(),
        agentId(),
        Support::clientId(),
        Domain::SessionStatus::Active,
        std::string{"ok"},
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{}};
}

[[nodiscard]] inline Domain::ActiveBinding activeBinding()
{
    return Domain::ActiveBinding{
        sessionId(),
        agentId(),
        "goal",
        {"read"},
        {"shell"},
        {"result"},
        {"done"},
        Support::path("C:/application-service")};
}

[[nodiscard]] inline Domain::AgentRunRecord agentRunRecord(
    const Domain::SessionStatus status = Domain::SessionStatus::Active)
{
    auto session = agentSession();
    session.status = status;
    return Domain::AgentRunRecord{
        std::move(session),
        Support::projectId(),
        std::string{"goal"},
        Support::path("C:/application-service"),
        {"result"},
        {"inspect"},
        status == Domain::SessionStatus::Closed
            ? std::optional<std::string>{"{\"result\":\"ok\"}"}
            : std::nullopt};
}

[[nodiscard]] inline Domain::AgentCompletionReport completionReport()
{
    return Domain::AgentCompletionReport{
        "{\"result\":\"ok\"}",
        {{"result", Domain::AgentReportValueKind::String, 2U}}};
}

[[nodiscard]] inline Contracts::AuthorizedToolCall authorizedCall(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context,
    const std::string_view toolName,
    const Domain::ToolEffect effect,
    const bool includeProject = true,
    const Domain::RequestId& callRequestId = requestId(),
    const Domain::SessionId& callSessionId = sessionId())
{
    Fakes::DeterministicToolAuthorizerFake authorizer{
        std::string{toolName},
        effect};
    authorizer.setNow(Domain::MonotonicTimePoint{});
    const auto canonicalArguments =
        std::string{"{\"session_id\":\""} +
        callSessionId.value() +
        "\"}";
    const auto request = Domain::ToolAuthorizationRequest{
        Domain::ToolCallRequest{
            Domain::McpRequestMetadata{
                callRequestId,
                context.correlationId,
                authority.callerId(),
                includeProject
                    ? std::optional<Domain::ProjectId>{authority.projectId()}
                    : std::nullopt,
                "1.0"},
            std::string{toolName},
            canonicalArguments},
        effect,
        Domain::AuthorityReference{
            authority.authorityId(),
            authority.generation()}};
    return Support::take(authorizer.authorize(request, authority, context));
}

} // namespace ApplicationServiceFakeContractTestsDetail

inline void runApplicationServiceFakeContractTests()
{
    namespace Detail = ApplicationServiceFakeContractTestsDetail;
    namespace Support = PlatformBoundaryTestSupport;
    namespace Fakes = ForgeConductor::Tests::Fakes;

    static_assert(std::is_final_v<Fakes::RecordingAgentCatalogFake>);
    static_assert(std::is_final_v<Fakes::RecordingAgentSessionRepositoryFake>);
    static_assert(std::is_final_v<Fakes::RecordingAgentSessionServiceFake>);
    static_assert(std::is_final_v<Fakes::RecordingLocalModelClientFake>);
    static_assert(std::is_final_v<Fakes::RecordingMcpServerFake>);
    static_assert(std::is_final_v<Fakes::RecordingForgeApplicationLifecycleFake>);

    const Domain::MonotonicTimePoint now{};
    const auto context = Support::activeContext(now);
    const auto spec = Detail::agentSpec();
    const auto session = Detail::agentSession();

    Fakes::RecordingAgentCatalogFake catalog{2, 64, now};
    catalog.allResult.set(
        Domain::Result<std::vector<Domain::AgentSpec>>::success({spec}));
    catalog.getResult.set(
        Domain::Result<std::optional<Domain::AgentSpec>>::success(spec));
    catalog.recommendResult.set(
        Domain::Result<Domain::AgentSpec>::success(spec));
    Support::require(
        Support::take(catalog.all(context)).size() == 1,
        "agent catalog did not return its bounded script");
    const std::string oversizedTask(65, 'x');
    Support::requireError(
        catalog.recommend(oversizedTask, context),
        Domain::ErrorCodes::PayloadTooLarge,
        "agent catalog retained an unbounded task");
    Support::require(
        catalog.lastCapture() &&
            catalog.lastCapture()->task.size() == 64 &&
            catalog.lastCapture()->requestedTextBytes == 65,
        "agent catalog did not retain a bounded latest capture");

    std::stop_source cancellation;
    cancellation.request_stop();
    Support::requireError(
        catalog.get(
            Detail::agentId(),
            Support::cancelledContext(now, cancellation.get_token())),
        Domain::ErrorCodes::Cancelled,
        "agent catalog accepted a cancelled context");
    Support::requireError(
        catalog.all(Support::expiredContext(now)),
        Domain::ErrorCodes::DeadlineExceeded,
        "agent catalog accepted an expired context");
    Support::require(
        catalog.lastContext() && catalog.lastContext()->deadline == now,
        "agent catalog did not expose its latest operation context");

    Fakes::RecordingAgentSessionRepositoryFake repository{2, 256, now};
    repository.saveResult.set(Domain::Result<void>::success());
    repository.getResult.set(
        Domain::Result<std::optional<Domain::AgentSession>>::success(session));
    repository.listResult.set(
        Domain::Result<std::vector<Domain::AgentSession>>::success(
            {session, session, session}));
    Support::require(
        repository.save(session, context).hasValue() &&
            repository.lastCapture() &&
            repository.lastCapture()->sessionId &&
            repository.lastCapture()->sessionId.value() == session.id,
        "agent session repository did not retain its save target");
    Support::requireError(
        repository.list(std::nullopt, std::nullopt, 2, context),
        Domain::ErrorCodes::LimitExceeded,
        "agent session repository returned an over-cap list");
    Support::requireError(
        repository.get(Detail::sessionId(), Support::expiredContext(now)),
        Domain::ErrorCodes::DeadlineExceeded,
        "agent session repository accepted an expired context");

    const auto run = Detail::agentRunRecord();
    const auto activeBinding = Detail::activeBinding();
    repository.startRunResult.set(
        Domain::Result<Domain::AgentRunStartPersistenceOutcome>::success(
            {run, activeBinding, 1U}));
    Support::require(
        repository.startRun(
            Domain::AgentRunStartMutation{run, activeBinding, "superseded"},
            context).hasValue() &&
            repository.lastCapture() &&
            repository.lastCapture()->call ==
                Fakes::AgentSessionRepositoryCall::StartRun &&
            repository.lastCapture()->startMutation &&
            repository.lastCapture()->startMutation->run.projectId ==
                Support::projectId(),
        "agent repository did not capture the atomic typed start mutation");

    repository.getRunResult.set(
        Domain::Result<std::optional<Domain::AgentRunRecord>>::success(run));
    Support::require(
        Support::take(repository.getRun(Detail::sessionId(), context))->goal ==
            std::optional<std::string>{"goal"},
        "agent repository did not return the durable run record");

    repository.reattachRunResult.set(
        Domain::Result<Domain::AgentRunReattachOutcome>::success(
            {run, activeBinding, Support::clientId(), 0U, false}));
    Support::require(
        repository.reattachRun(
            Domain::AgentRunReattachMutation{
                Detail::sessionId(),
                Support::clientId(),
                Support::clientId(),
                activeBinding,
                "reattached",
                Domain::UtcTimePoint{}},
            context).hasValue() &&
            repository.lastCapture()->reattachMutation.has_value(),
        "agent repository did not capture the reattach CAS mutation");

    const auto closedRun = Detail::agentRunRecord(Domain::SessionStatus::Closed);
    repository.completeRunResult.set(
        Domain::Result<Domain::AgentRunCompletePersistenceOutcome>::success(
            {closedRun, true}));
    Support::require(
        repository.completeRun(
            Domain::AgentRunCompleteMutation{
                Detail::sessionId(),
                Support::clientId(),
                "{\"result\":\"ok\"}",
                "complete",
                {},
                Domain::UtcTimePoint{}},
            context).hasValue() &&
            repository.lastCapture()->completeMutation.has_value(),
        "agent repository did not capture the atomic completion mutation");

    repository.touchRunResult.set(Domain::Result<bool>::success(true));
    Support::require(
        Support::take(repository.touchRun(
            Detail::sessionId(), Domain::UtcTimePoint{}, context)),
        "agent repository did not return its scripted touch result");
    repository.latestOpenRunResult.set(
        Domain::Result<std::optional<Domain::AgentRunRecord>>::success(run));
    Support::require(
        Support::take(repository.latestOpenRun(Support::clientId(), context))
            .has_value(),
        "agent repository did not return its latest open run");
    repository.recoverRunResult.set(
        Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
            {run, activeBinding, true, false}));
    Support::require(
        Support::take(repository.recoverRun(
            {Support::clientId()}, context)).usedActiveProjection,
        "agent repository did not return its recovery projection");
    repository.repairProjectionResult.set(
        Domain::Result<Domain::AgentProjectionRepairOutcome>::success(
            {activeBinding, true}));
    Support::require(
        Support::take(repository.repairProjection(
            {Support::clientId(), run, activeBinding}, context)).repaired,
        "agent repository did not capture projection repair");
    repository.closeStaleResult.set(
        Domain::Result<Domain::AgentStaleCloseOutcome>::success(
            {{closedRun}}));
    Support::require(
        Support::take(repository.closeStale(
            {Domain::UtcTimePoint{}, Domain::UtcTimePoint{}, 2U}, context))
                .closedRuns.size() == 1U,
        "agent repository did not return its bounded stale-close result");
    repository.quickCheckResult.set(Domain::Result<void>::success());
    Support::require(
        repository.quickCheck(context).hasValue(),
        "agent repository did not run its integrity check");

    const auto root = Support::path("C:/application-service");
    Fakes::DeterministicWorkspaceAuthority issuer{
        Support::authorityId(),
        Support::clientId(),
        {root},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        {Domain::FileAccess::Delete},
        false,
        1};
    issuer.setNow(now);
    const auto authority = Support::take(
        issuer.authorityFor(Support::projectId(), context));
    const auto authorization = Detail::authorizedCall(
        authority,
        context,
        Fakes::RecordingAgentSessionServiceFake::StatusToolName,
        Domain::ToolEffect::Write);

    Fakes::RecordingAgentSessionServiceFake sessions{8, 512, now};
    sessions.startResult.set(
        Domain::Result<Domain::AgentSession>::success(session));
    sessions.statusResult.set(
        Domain::Result<Domain::AgentSession>::success(session));
    sessions.completeResult.set(
        Domain::Result<Domain::AgentSession>::success(session));
    sessions.pruneStaleResult.set(Domain::Result<std::size_t>::success(1));
    Support::require(
        sessions.start(
            Detail::agentId(),
            Support::clientId(),
            "goal",
            root,
            context).hasValue(),
        "agent session service rejected a bounded start");
    Support::require(
        sessions.status(
            Detail::sessionId(),
            authority,
            authorization,
            context).hasValue() &&
            sessions.lastCapture() &&
            sessions.lastCapture()->sessionId &&
            sessions.lastCapture()->sessionId.value() == Detail::sessionId() &&
            sessions.lastCapture()->authority &&
            sessions.lastCapture()->authority->authorityId() ==
                authority.authorityId() &&
            sessions.lastCapture()->authorization &&
            sessions.lastCapture()->authorization->authorityGeneration() ==
                authority.generation() &&
            sessions.consumedStatusRequestId() &&
            sessions.consumedStatusRequestId().value() ==
                authorization.requestId() &&
            sessions.consumedStatusSessionId() &&
            sessions.consumedStatusSessionId().value() == Detail::sessionId(),
        "agent status did not preserve session and authorization binding");

    Support::requireError(
        sessions.status(
            Detail::sessionId(),
            authority,
            authorization,
            context),
        Domain::ErrorCodes::Unauthorized,
        "agent status accepted a replayed authorization");

    Support::requireError(
        sessions.status(
            Detail::otherSessionId(),
            authority,
            authorization,
            context),
        Domain::ErrorCodes::Unauthorized,
        "agent status replay crossed a session boundary");

    const auto wrongTool = Detail::authorizedCall(
        authority,
        context,
        "agent_status",
        Domain::ToolEffect::Write);
    Support::requireError(
        sessions.status(Detail::sessionId(), authority, wrongTool, context),
        Domain::ErrorCodes::Unauthorized,
        "agent status accepted the wrong tool name");

    const auto wrongEffect = Detail::authorizedCall(
        authority,
        context,
        Fakes::RecordingAgentSessionServiceFake::StatusToolName,
        Domain::ToolEffect::Read);
    Support::requireError(
        sessions.status(Detail::sessionId(), authority, wrongEffect, context),
        Domain::ErrorCodes::Unauthorized,
        "agent status accepted a non-write authorization");

    Fakes::DeterministicWorkspaceAuthority wrongIntentIssuer{
        Detail::otherAuthorityId(),
        Support::clientId(),
        {root},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        {Domain::FileAccess::Delete},
        false,
        1};
    wrongIntentIssuer.setNow(now);
    const auto wrongIntentAuthority = Support::take(
        wrongIntentIssuer.authorityFor(Support::projectId(), context));
    const auto wrongIntentAuthorization = Detail::authorizedCall(
        wrongIntentAuthority,
        context,
        Fakes::RecordingAgentSessionServiceFake::StatusToolName,
        Domain::ToolEffect::Write);
    Support::requireError(
        sessions.status(
            Detail::sessionId(),
            wrongIntentAuthority,
            wrongIntentAuthorization,
            context),
        Domain::ErrorCodes::Unauthorized,

        "agent status accepted write grants under a non-write intent");
    const auto missingProject = Detail::authorizedCall(
        authority,
        context,
        Fakes::RecordingAgentSessionServiceFake::StatusToolName,
        Domain::ToolEffect::Write,
        false);
    Support::requireError(
        sessions.status(Detail::sessionId(), authority, missingProject, context),
        Domain::ErrorCodes::Unauthorized,
        "agent status accepted an unbound project authorization");

    const auto nextGeneration = Support::take(issuer.narrow(
        authority,
        {root},
        {Domain::FileAccess::Write},
        false,
        2,
        context));
    Support::requireError(
        sessions.status(
            Detail::sessionId(),
            nextGeneration,
            authorization,
            context),
        Domain::ErrorCodes::Unauthorized,
        "agent status accepted a stale authority generation");

    Fakes::DeterministicWorkspaceAuthority otherIssuer{
        Detail::otherAuthorityId(),
        Support::clientId(),
        {root},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Write},
        {Domain::FileAccess::Delete},
        false,
        1};
    otherIssuer.setNow(now);
    const auto otherAuthority = Support::take(
        otherIssuer.authorityFor(Support::projectId(), context));
    Support::requireError(
        sessions.status(
            Detail::sessionId(),
            otherAuthority,
            authorization,
            context),
        Domain::ErrorCodes::Unauthorized,
        "agent status accepted a mismatched authority identity");

    Fakes::RecordingAgentSessionServiceFake typedSessions{8, 512, now};
    typedSessions.startRunResult.set(
        Domain::Result<Domain::AgentRunStartOutcome>::success(
            {run, activeBinding, spec, 1U, true}));
    typedSessions.runStatusResult.set(
        Domain::Result<Domain::AgentRunStatusOutcome>::success(
            {run, true, 301, true, false, activeBinding}));
    typedSessions.completeRunResult.set(
        Domain::Result<Domain::AgentRunCompleteOutcome>::success(
            {closedRun, Detail::completionReport(), true, {}}));
    typedSessions.attachResult.set(
        Domain::Result<Domain::AgentRunReattachOutcome>::success(
            {run, activeBinding, Support::clientId(), 0U, false}));
    typedSessions.rehydrateResult.set(
        Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
            {run, activeBinding, true, false}));
    typedSessions.bindingResult.set(
        Domain::Result<std::optional<Domain::ActiveBinding>>::success(
            activeBinding));
    typedSessions.touchIfActiveResult.set(Domain::Result<bool>::success(true));
    typedSessions.pruneStaleResult.set(
        Domain::Result<std::size_t>::success(0U));

    Support::require(
        typedSessions.startRun(
            {Detail::agentId(), Support::clientId(), Support::projectId(),
             "goal", root},
            context).hasValue() &&
            typedSessions.lastCapture()->startRequest->projectId ==
                Support::projectId(),
        "typed agent service did not preserve the project-scoped start request");
    Support::require(
        Support::take(typedSessions.runStatus(
            {Detail::sessionId(), Support::clientId()},
            authority,
            authorization,
            context)).abandonRisk,
        "typed agent service did not return its scripted status outcome");
    Support::requireError(
        typedSessions.attach(
            {Detail::otherSessionId(), Support::clientId()},
            authority,
            authorization,
            context),
        Domain::ErrorCodes::Unauthorized,
        "typed agent authorization replay crossed a session boundary");
    Support::require(
        typedSessions.completeRun(
            {Detail::sessionId(), Support::clientId(),
             Detail::completionReport()},
            context).hasValue() &&
            typedSessions.lastCapture()->completeRequest.has_value(),
        "typed agent service did not capture completion report metadata");
    const auto attachAuthorization = Detail::authorizedCall(
        authority,
        context,
        Fakes::RecordingAgentSessionServiceFake::StatusToolName,
        Domain::ToolEffect::Write,
        true,
        Detail::otherRequestId());
    Support::require(
        typedSessions.attach(
            {Detail::sessionId(), Support::clientId()},
            authority,
            attachAuthorization,
            context).hasValue(),
        "typed agent service rejected a bounded authorized attach");
    Support::require(
        typedSessions.rehydrate({Support::clientId()}, context).hasValue() &&
            Support::take(typedSessions.binding(Support::clientId(), context))
                .has_value() &&
            Support::take(
                typedSessions.touchIfActive(Support::clientId(), context)),
        "typed agent service did not exercise recovery, binding, and touch");
    typedSessions.shutdown();
    Support::requireError(
        typedSessions.rehydrate({Support::clientId()}, context),
        Domain::ErrorCodes::Cancelled,
        "typed agent service accepted recovery after shutdown");

    const std::string oversizedGoal(513, 'g');
    Support::requireError(
        sessions.start(
            Detail::agentId(),
            Support::clientId(),
            oversizedGoal,
            std::nullopt,
            context),
        Domain::ErrorCodes::PayloadTooLarge,
        "agent session service retained an unbounded goal");
    Support::requireError(
        sessions.complete(
            Detail::sessionId(),
            "done",
            true,
            Support::expiredContext(now)),
        Domain::ErrorCodes::DeadlineExceeded,
        "agent session service accepted an expired context");
    sessions.shutdown();
    Support::requireError(
        sessions.pruneStale(context),
        Domain::ErrorCodes::Cancelled,
        "agent session service accepted work after shutdown");

    Fakes::RecordingLocalModelClientFake localModel{now};
    localModel.queryResult.set(
        Domain::Result<Domain::HostSessionStatus>::success(
            Domain::HostSessionStatus::Ready));
    localModel.cancelResult.set(Domain::Result<void>::success());
    Support::require(
        Support::take(localModel.query(Detail::providerSessionId(), context)) ==
            Domain::HostSessionStatus::Ready,
        "local-model client did not return its scripted status");
    Support::require(
        localModel.cancel(Detail::providerSessionId(), context).hasValue() &&
            localModel.lastCapture() &&
            localModel.lastCapture()->call ==
                Fakes::LocalModelClientCall::Cancel &&
            localModel.lastCapture()->sessionId == Detail::providerSessionId(),
        "local-model cancellation did not retain its provider target");
    Support::requireError(
        localModel.query(
            Detail::providerSessionId(),
            Support::cancelledContext(now, cancellation.get_token())),
        Domain::ErrorCodes::Cancelled,
        "local-model client accepted a cancelled context");
    Support::requireError(
        localModel.cancel(
            Detail::providerSessionId(),
            Support::expiredContext(now)),
        Domain::ErrorCodes::DeadlineExceeded,
        "local-model client accepted an expired cancellation context");
    localModel.shutdown();
    Support::requireError(
        localModel.query(Detail::providerSessionId(), context),
        Domain::ErrorCodes::Cancelled,
        "local-model client accepted work after shutdown");

    Fakes::RecordingMcpServerFake server{now};
    Fakes::McpTransportFake transport;
    server.runResult.set(Domain::Result<void>::success());
    Support::require(
        server.run(
            transport,
            Domain::McpRole::Primary,
            Detail::deploymentId(),
            Support::clientId(),
            context).hasValue() &&
            server.lastCapture() &&
            server.lastCapture()->transport == &transport &&
            server.lastCapture()->deploymentId &&
            server.lastCapture()->deploymentId.value() == Detail::deploymentId(),
        "MCP server did not retain its run binding");
    server.cancel(context.operationId);
    Support::requireError(
        server.run(
            transport,
            Domain::McpRole::Primary,
            Detail::deploymentId(),
            Support::clientId(),
            context),
        Domain::ErrorCodes::Cancelled,
        "MCP server ignored targeted cancellation");
    const auto otherContext =
        Support::activeContext(now, Support::otherOperationId());
    Support::require(
        server.run(
            transport,
            Domain::McpRole::Primary,
            Detail::deploymentId(),
            Support::clientId(),
            otherContext).hasValue(),
        "MCP targeted cancellation leaked to another operation");
    Support::requireError(
        server.run(
            transport,
            Domain::McpRole::Primary,
            Detail::deploymentId(),
            Support::clientId(),
            Support::expiredContext(now)),
        Domain::ErrorCodes::DeadlineExceeded,
        "MCP server accepted an expired context");
    server.shutdown();
    Support::requireError(
        server.run(
            transport,
            Domain::McpRole::Primary,
            Detail::deploymentId(),
            Support::clientId(),
            otherContext),
        Domain::ErrorCodes::Cancelled,
        "MCP server accepted work after shutdown");

    Fakes::RecordingForgeApplicationLifecycleFake lifecycle;
    lifecycle.startResult.set(Domain::Result<void>::success());
    lifecycle.stopResult.set(Domain::Result<void>::success());
    Support::require(
        lifecycle.start().hasValue() && lifecycle.isStarted(),
        "application lifecycle did not start");
    Support::require(
        lifecycle.stop().hasValue() && lifecycle.isStopped() &&
            !lifecycle.isStarted(),
        "application lifecycle did not stop");
    Support::requireError(
        lifecycle.start(),
        Domain::ErrorCodes::Cancelled,
        "application lifecycle restarted after stop");
}

} // namespace ForgeConductor::Tests
