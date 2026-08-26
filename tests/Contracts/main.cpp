#include "ForgeConductor/Contracts/Contracts.h"
#include "Contracts/FoundationTelemetryFakeContractTests.h"
#include "Contracts/GroupedFakeContractTests.h"
#include "Fakes/BoundedLatestValueMailboxFake.h"
#include "Fakes/ExternalServiceFakes.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/McpTransportFake.h"
#include "Fakes/RecordingContinuityCoordinator.h"
#include "Fakes/RecordingProcessSupervisor.h"
#include "Fakes/RecordingProjectMemoryService.h"
#include "Fakes/RecordingSessionHostAdapter.h"
#include "Fakes/TelemetryFakes.h"
#include "Fakes/ToolServiceFakes.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
using namespace std::chrono_literals;

#define FORGE_ASSERT_INTERFACE(Type)                     \
    static_assert(std::is_abstract_v<Type>);             \
    static_assert(std::has_virtual_destructor_v<Type>)

FORGE_ASSERT_INTERFACE(Contracts::IAgentCatalog);
FORGE_ASSERT_INTERFACE(Contracts::IAgentSessionRepository);
FORGE_ASSERT_INTERFACE(Contracts::IAgentSessionService);
FORGE_ASSERT_INTERFACE(Contracts::IConfigurationStore);
FORGE_ASSERT_INTERFACE(Contracts::IContinuityRepository);
FORGE_ASSERT_INTERFACE(Contracts::IContinuityRepositoryFactory);
FORGE_ASSERT_INTERFACE(Contracts::IContinuityCoordinator);
FORGE_ASSERT_INTERFACE(Contracts::IDiagnosticSink);
FORGE_ASSERT_INTERFACE(Contracts::IAuditRepository);
FORGE_ASSERT_INTERFACE(Contracts::IDoctorService);
FORGE_ASSERT_INTERFACE(Contracts::IRuntimeDiagnostics);
FORGE_ASSERT_INTERFACE(Contracts::IApplicationPaths);
FORGE_ASSERT_INTERFACE(Contracts::IWorkspaceAuthority);
FORGE_ASSERT_INTERFACE(Contracts::IAtomicFileStore);
FORGE_ASSERT_INTERFACE(Contracts::IFileSystem);
FORGE_ASSERT_INTERFACE(Contracts::IGraphicsDeviceService);
FORGE_ASSERT_INTERFACE(Contracts::IRenderService);
FORGE_ASSERT_INTERFACE(Contracts::IInstallerDeploymentService);
FORGE_ASSERT_INTERFACE(Contracts::ILMStudioDeploymentService);
FORGE_ASSERT_INTERFACE(Contracts::ILMStudioEnvironment);
FORGE_ASSERT_INTERFACE(Contracts::IForgeApplicationLifecycle);
FORGE_ASSERT_INTERFACE(Contracts::IClock);
FORGE_ASSERT_INTERFACE(Contracts::IUuidGenerator);
FORGE_ASSERT_INTERFACE(Contracts::IHasher);
FORGE_ASSERT_INTERFACE(Contracts::IRedactor);
FORGE_ASSERT_INTERFACE(Contracts::IDeadlineScheduler);
FORGE_ASSERT_INTERFACE(Contracts::ILatestValueMailbox<int>);
FORGE_ASSERT_INTERFACE(Contracts::ILegacyMemoryService);
FORGE_ASSERT_INTERFACE(Contracts::IManagerClient);
FORGE_ASSERT_INTERFACE(Contracts::IManagerServer);
FORGE_ASSERT_INTERFACE(Contracts::IMcpServer);
FORGE_ASSERT_INTERFACE(Contracts::IMcpTransport);
FORGE_ASSERT_INTERFACE(Contracts::ITextSearchService);
FORGE_ASSERT_INTERFACE(Contracts::IGitService);
FORGE_ASSERT_INTERFACE(Contracts::IShellService);
FORGE_ASSERT_INTERFACE(Contracts::IPdfService);
FORGE_ASSERT_INTERFACE(Contracts::IProcessSupervisor);
FORGE_ASSERT_INTERFACE(Contracts::IProjectMemoryArtifactStore);
FORGE_ASSERT_INTERFACE(Contracts::IProjectRegistryRepository);
FORGE_ASSERT_INTERFACE(Contracts::IProjectMemoryRepository);
FORGE_ASSERT_INTERFACE(Contracts::IProjectRepository);
FORGE_ASSERT_INTERFACE(Contracts::IProjectMemoryRepositoryFactory);
FORGE_ASSERT_INTERFACE(Contracts::IProjectMemoryRepositoryOpener);
FORGE_ASSERT_INTERFACE(Contracts::IProjectMemoryService);
FORGE_ASSERT_INTERFACE(Contracts::ISecureStorage);
FORGE_ASSERT_INTERFACE(Contracts::ISessionHostAdapter);
FORGE_ASSERT_INTERFACE(Contracts::ILocalModelClient);
FORGE_ASSERT_INTERFACE(Contracts::ICpuMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::IRamMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::IDiskVolumeCollector);
FORGE_ASSERT_INTERFACE(Contracts::IDiskIoMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::IGpuMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::IProcessMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::IPowerMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::ISystemMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::IForgeMetricsCollector);
FORGE_ASSERT_INTERFACE(Contracts::ITelemetryService);
FORGE_ASSERT_INTERFACE(Contracts::IToolAuthorizer);
FORGE_ASSERT_INTERFACE(Contracts::IToolHandler);
FORGE_ASSERT_INTERFACE(Contracts::IToolCatalog);
FORGE_ASSERT_INTERFACE(Contracts::IToolRouter);

#undef FORGE_ASSERT_INTERFACE

static_assert(std::is_final_v<Fakes::RecordingProjectMemoryService>);
static_assert(std::is_final_v<Fakes::RecordingProcessSupervisor>);
static_assert(std::is_final_v<Fakes::RecordingSessionHostAdapter>);
static_assert(std::is_final_v<Fakes::RecordingContinuityCoordinator>);
static_assert(std::is_final_v<Fakes::RecordingTelemetryService>);
static_assert(std::is_final_v<Fakes::RecordingLMStudioEnvironmentFake>);
static_assert(std::is_final_v<Fakes::RecordingLMStudioDeploymentServiceFake>);
static_assert(std::is_final_v<Fakes::RecordingGraphicsDeviceServiceFake>);
static_assert(std::is_final_v<Fakes::RecordingRenderServiceFake>);
static_assert(std::is_final_v<Fakes::RecordingInstallerDeploymentServiceFake>);
static_assert(std::is_final_v<Fakes::DeterministicWorkspaceAuthority>);
static_assert(std::is_final_v<Fakes::DeterministicToolAuthorizerFake>);
static_assert(std::is_final_v<Fakes::RecordingToolHandlerFake>);
static_assert(std::is_final_v<Fakes::BoundedToolCatalogFake>);
static_assert(std::is_final_v<Fakes::RecordingToolRouterFake>);
static_assert(!std::is_default_constructible_v<Contracts::AuthorizedToolCall>);
static_assert(!std::is_aggregate_v<Contracts::AuthorizedToolCall>);
static_assert(!std::is_copy_assignable_v<Contracts::AuthorizedToolCall>);
static_assert(!std::is_move_assignable_v<Contracts::AuthorizedToolCall>);
static_assert(!std::is_constructible_v<
    Contracts::AuthorizedToolCall,
    Domain::ToolCallRequest,
    Domain::ToolEffect,
    Domain::AuthorityId,
    std::uint64_t>);
static_assert(!std::is_default_constructible_v<Contracts::WorkspaceAuthority>);
static_assert(!std::is_aggregate_v<Contracts::WorkspaceAuthority>);
static_assert(!std::is_copy_assignable_v<Contracts::WorkspaceAuthority>);
static_assert(!std::is_move_assignable_v<Contracts::WorkspaceAuthority>);
static_assert(!std::is_constructible_v<
    Contracts::WorkspaceAuthority,
    Domain::AuthorityId,
    Domain::ProjectId,
    Domain::ClientId,
    std::vector<Domain::PathText>,
    Domain::FileAccess,
    std::vector<Domain::FileAccess>,
    std::vector<Domain::FileAccess>,
    bool,
    std::uint64_t>);
static_assert(!std::is_default_constructible_v<Contracts::AuthorizedPath>);
static_assert(!std::is_aggregate_v<Contracts::AuthorizedPath>);
static_assert(!std::is_copy_assignable_v<Contracts::AuthorizedPath>);
static_assert(!std::is_move_assignable_v<Contracts::AuthorizedPath>);
static_assert(!std::is_constructible_v<
    Contracts::AuthorizedPath,
    Domain::AuthorityId,
    Domain::PathText,
    Domain::PathText,
    Domain::FileAccess>);
static_assert(!std::is_assignable_v<
    decltype((std::declval<const Contracts::WorkspaceAuthority&>().trustedRoots())),
    std::vector<Domain::PathText>>);
static_assert(!std::is_assignable_v<
    decltype((std::declval<const Contracts::AuthorizedPath&>().canonicalPath())),
    Domain::PathText>);
static_assert(noexcept(std::declval<Contracts::IForgeApplicationLifecycle&>().start()));
static_assert(noexcept(std::declval<Contracts::IForgeApplicationLifecycle&>().stop()));
static_assert(std::is_same_v<
    decltype(std::declval<Contracts::IForgeApplicationLifecycle&>().start()),
    Domain::Result<void>>);
using DeadlineSchedulerSignature = Domain::Result<void> (
    Contracts::IDeadlineScheduler::*)(
        const Domain::OperationContext&) noexcept;
using ProjectRepositoryExportSignature =
    Domain::Result<Domain::ProjectMemoryExport> (
        Contracts::IProjectMemoryRepository::*)(
            const Domain::ExportProjectMemoryRequest&,
            const Contracts::WorkspaceAuthority&,
            const Contracts::AuthorizedToolCall&,
            const Domain::OperationContext&) noexcept;
using ProjectServiceExportSignature =
    Domain::Result<Domain::ProjectMemoryExport> (
        Contracts::IProjectMemoryService::*)(
            const Domain::ExportProjectMemoryRequest&,
            const Contracts::WorkspaceAuthority&,
            const Contracts::AuthorizedToolCall&,
            const Domain::OperationContext&) noexcept;
using AgentStatusSignature = Domain::Result<Domain::AgentSession> (
    Contracts::IAgentSessionService::*)(
        const Domain::SessionId&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept;

static_assert(std::is_same_v<
    decltype(&Contracts::IDeadlineScheduler::waitUntil),
    DeadlineSchedulerSignature>);
static_assert(std::is_same_v<
    decltype(&Contracts::IProjectMemoryRepository::exportMemory),
    ProjectRepositoryExportSignature>);
static_assert(std::is_same_v<
    decltype(&Contracts::IProjectMemoryService::exportMemory),
    ProjectServiceExportSignature>);
static_assert(std::is_same_v<
    decltype(&Contracts::IAgentSessionService::status),
    AgentStatusSignature>);

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

template <typename T>
T id(const std::string_view value)
{
    return take(T::parse(value));
}

struct Fixture final {
    Domain::ProjectId projectId{
        id<Domain::ProjectId>("11111111-1111-4111-8111-111111111111")};
    Domain::MemoryRecordId recordId{
        id<Domain::MemoryRecordId>("22222222-2222-4222-8222-222222222222")};
    Domain::SessionId predecessorSessionId{
        id<Domain::SessionId>("33333333-3333-4333-8333-333333333333")};
    Domain::SessionId successorSessionId{
        id<Domain::SessionId>("44444444-4444-4444-8444-444444444444")};
    Domain::OperationId operationId{
        id<Domain::OperationId>("55555555-5555-4555-8555-555555555555")};
    Domain::ContinuityOperationId continuityOperationId{
        id<Domain::ContinuityOperationId>(
            "66666666-6666-4666-8666-666666666666")};
    Domain::ContinuityHandoffId handoffId{
        id<Domain::ContinuityHandoffId>(
            "77777777-7777-4777-8777-777777777777")};
    Domain::AuthorityId authorityId{
        id<Domain::AuthorityId>("88888888-8888-4888-8888-888888888888")};
    Domain::AuthorityId otherAuthorityId{
        id<Domain::AuthorityId>("99999999-9999-4999-8999-999999999999")};
    Domain::ProjectId otherProjectId{
        id<Domain::ProjectId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")};
    Domain::ContinuityOperationId otherContinuityOperationId{
        id<Domain::ContinuityOperationId>(
            "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")};
    Domain::SessionId otherSessionId{
        id<Domain::SessionId>("cccccccc-cccc-4ccc-8ccc-cccccccccccc")};
    Domain::ClientId clientId{id<Domain::ClientId>("client-test")};
    Domain::ClientId otherClientId{id<Domain::ClientId>("client-other")};
    Domain::AdapterId adapterId{id<Domain::AdapterId>("forge-native")};
    Domain::DeploymentId deploymentId{
        id<Domain::DeploymentId>("deployment-test")};
    Domain::ProviderSessionId providerSessionId{
        id<Domain::ProviderSessionId>("provider-test")};
    Domain::CorrelationId correlationId{
        id<Domain::CorrelationId>("correlation-test")};
    Domain::CorrelationId otherCorrelationId{
        id<Domain::CorrelationId>("correlation-other")};
    Domain::RequestId requestId{id<Domain::RequestId>("request-test")};
    Domain::IdempotencyKey idempotencyKey{
        take(Domain::IdempotencyKey::create("rollover-test"))};
    Domain::Sha256Digest digest{
        take(Domain::Sha256Digest::parse(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))};
    Domain::Sha256Digest otherDigest{
        take(Domain::Sha256Digest::parse(
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"))};
    Domain::PathText root{path("C:/forge-test")};

    [[nodiscard]] Domain::OperationContext context() const
    {
        return Domain::OperationContext{
            operationId,
            Domain::MonotonicTimePoint{} + 10s,
            std::stop_token{},
            correlationId};
    }

    [[nodiscard]] Contracts::WorkspaceAuthority authority(
        const Domain::FileAccess intent = Domain::FileAccess::Execute,
        std::vector<Domain::FileAccess> grants = {
            Domain::FileAccess::Read,
            Domain::FileAccess::Execute},
        const bool shellEnabled = true,
        const std::uint64_t generation = 1) const
    {
        Fakes::DeterministicWorkspaceAuthority issuer{
            authorityId,
            clientId,
            {root},
            intent,
            std::move(grants),
            {Domain::FileAccess::Delete},
            shellEnabled,
            generation};
        return take(issuer.authorityFor(projectId, context()));
    }

    [[nodiscard]] Domain::ProjectMemoryWrite memoryWrite() const
    {
        return Domain::ProjectMemoryWrite{
            "decision",
            "Typed contracts",
            "Preserve authority, cancellation, and deadlines.",
            std::nullopt,
            {"p05"},
            0.8,
            1.0,
            "test",
            std::nullopt,
            predecessorSessionId,
            std::nullopt,
            {},
            std::nullopt};
    }

    [[nodiscard]] Domain::ToolCallRequest toolCall(
        const std::string_view toolName,
        const std::string_view canonicalArguments = "{}") const
    {
        return Domain::ToolCallRequest{
            Domain::McpRequestMetadata{
                requestId,
                correlationId,
                clientId,
                projectId,
                "1.0"},
            std::string{toolName},
            std::string{canonicalArguments}};
    }

    [[nodiscard]] Contracts::AuthorizedToolCall authorization(
        const std::string_view toolName,
        const Domain::ToolEffect effect,
        const Contracts::WorkspaceAuthority& authority,
        const std::string_view canonicalArguments = "{}") const
    {
        Fakes::DeterministicToolAuthorizerFake authorizer{
            std::string{toolName}, effect};
        return take(authorizer.authorize(
            Domain::ToolAuthorizationRequest{
                toolCall(toolName, canonicalArguments),
                effect,
                Domain::AuthorityReference{
                    authority.authorityId(),
                    authority.generation()}},
            authority,
            context()));
    }

    [[nodiscard]] Domain::HostSession successorSession() const
    {
        return Domain::HostSession{
            successorSessionId,
            projectId,
            continuityOperationId,
            predecessorSessionId,
            idempotencyKey,
            providerSessionId,
            std::string{"test-model"},
            Domain::HostSessionStatus::Ready};
    }

    [[nodiscard]] Domain::ContinuityHandoff handoff() const
    {
        return Domain::ContinuityHandoff{
            handoffId,
            continuityOperationId,
            Domain::UtcTimePoint{},
            Domain::ContinuityProject{
                projectId,
                "Forge Test",
                root,
                "main",
                "abc123",
                {}},
            Domain::ContinuitySession{
                predecessorSessionId,
                std::nullopt,
                std::nullopt,
                std::nullopt},
            Domain::ContinuitySession{
                successorSessionId,
                providerSessionId,
                std::string{"test-model"},
                std::nullopt},
            "Complete P05",
            {},
            Domain::ContinuityCurrentWork{
                "P05",
                "contracts",
                "Build deterministic contracts.",
                {}},
            {},
            {},
            {},
            Domain::ContinuityValidation{{}, {}, {}},
            {},
            {},
            {},
            Domain::ContinuityHostState{
                adapterId,
                Domain::ContinuityState::CheckpointPreparing,
                "test",
                Domain::ContinuityRetryState{}},
            digest,
            true};
    }
};

void testAuthorizedToolCapability(const Fixture& fixture)
{
    const auto context = fixture.context();
    const auto authority = fixture.authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        true,
        7);
    const auto call = fixture.toolCall(
        "project_memory.export",
        "{\"project_id\":\"11111111-1111-4111-8111-111111111111\"}");
    const Domain::ToolAuthorizationRequest request{
        call,
        Domain::ToolEffect::Write,
        Domain::AuthorityReference{
            authority.authorityId(),
            authority.generation()}};
    Fakes::DeterministicToolAuthorizerFake authorizer{
        "project_memory.export",
        Domain::ToolEffect::Write};

    const auto authorization = take(
        authorizer.authorize(request, authority, context));
    expect(
        authorization.requestId() == fixture.requestId,
        "authorized call did not bind the request ID");
    expect(
        authorization.correlationId() == fixture.correlationId,
        "authorized call did not bind the correlation ID");
    expect(
        authorization.clientId() == fixture.clientId,
        "authorized call did not bind the caller");
    expect(
        authorization.request().metadata.protocolVersion == "1.0",
        "authorized call did not preserve the protocol version");
    expect(
        authorization.toolName() == "project_memory.export",
        "authorized call did not bind the tool name");
    expect(
        authorization.canonicalRequest() == call.canonicalArguments,
        "authorized call did not bind the canonical request");
    expect(
        authorization.effect() == Domain::ToolEffect::Write,
        "authorized call did not bind the exact effect");
    expect(
        authorization.projectId() &&
            authorization.projectId().value() == fixture.projectId,
        "authorized call did not bind the present project");
    expect(
        authorization.authorityId() == fixture.authorityId &&
            authorization.authorityGeneration() == 7,
        "authorized call did not bind authority identity and generation");
    expect(
        authorization.matches(call) &&
            authorization.matches(authority, context) &&
            authorization.matchesProject(fixture.projectId),
        "authentic capability did not match its exact bindings");

    auto changedCanonical = call;
    changedCanonical.canonicalArguments = "{\"project_id\":\"changed\"}";
    expect(
        !authorization.matches(changedCanonical),
        "authorized call matched substituted canonical arguments");

    auto callerMismatchRequest = request;
    callerMismatchRequest.call.metadata.clientId = fixture.otherClientId;
    const auto callerMismatch =
        authorizer.authorize(callerMismatchRequest, authority, context);
    expect(
        !callerMismatch &&
            callerMismatch.error().code == Domain::ErrorCodes::Unauthorized,
        "tool authorizer accepted a mismatched caller");

    auto correlationMismatchRequest = request;
    correlationMismatchRequest.call.metadata.correlationId =
        fixture.otherCorrelationId;
    const auto correlationMismatch =
        authorizer.authorize(correlationMismatchRequest, authority, context);
    expect(
        !correlationMismatch &&
            correlationMismatch.error().code == Domain::ErrorCodes::Unauthorized,
        "tool authorizer accepted a mismatched correlation");

    auto projectMismatchRequest = request;
    projectMismatchRequest.call.metadata.projectId = fixture.otherProjectId;
    const auto projectMismatch =
        authorizer.authorize(projectMismatchRequest, authority, context);
    expect(
        !projectMismatch &&
            projectMismatch.error().code ==
                Domain::ErrorCodes::ProjectScopeMismatch,
        "tool authorizer accepted a mismatched present project");

    auto authorityIdMismatchRequest = request;
    authorityIdMismatchRequest.authority.authorityId = fixture.otherAuthorityId;
    const auto authorityIdMismatch =
        authorizer.authorize(authorityIdMismatchRequest, authority, context);
    expect(
        !authorityIdMismatch &&
            authorityIdMismatch.error().code == Domain::ErrorCodes::Unauthorized,
        "tool authorizer accepted a mismatched authority ID");

    auto generationMismatchRequest = request;
    ++generationMismatchRequest.authority.generation;
    const auto generationMismatch =
        authorizer.authorize(generationMismatchRequest, authority, context);
    expect(
        !generationMismatch &&
            generationMismatch.error().code == Domain::ErrorCodes::Unauthorized,
        "tool authorizer accepted a stale authority generation");

    auto projectlessRequest = request;
    projectlessRequest.call.metadata.projectId.reset();
    const auto projectless = take(
        authorizer.authorize(projectlessRequest, authority, context));
    expect(
        !projectless.projectId() &&
            !projectless.matchesProject(fixture.projectId) &&
            projectless.matches(authority, context),
        "projectless authorization acquired a project binding");

    const Domain::McpToolDescriptor descriptor{
        Domain::ToolDescriptor{
            "project_memory.export",
            "Export project memory.",
            "project-memory",
            Domain::ToolEffect::Write,
            Domain::ToolAvailability::Available,
            true,
            false},
        "{\"type\":\"object\"}"};
    Fakes::BoundedToolCatalogFake catalog{{descriptor}};
    Fakes::RecordingToolHandlerFake handler{{descriptor}};
    handler.handleResult.set(
        Domain::Result<Domain::ToolCallOutcome>::success(
            Domain::ToolCallOutcome{
                Domain::ToolExecutionReceipt{
                    fixture.requestId,
                    "project_memory.export",
                    true,
                    std::nullopt,
                    1ms},
                "{\"ok\":true}"}));
    Fakes::RecordingToolRouterFake router{authorizer, handler};

    expect(
        catalog.tools().size() == 1 &&
            catalog.tools().front().tool.name == "project_memory.export",
        "bounded tool catalog changed its descriptor");
    const auto routed = router.invoke(call, authority, context);
    expect(routed.hasValue(), "authorized tool call did not route");
    expect(
        handler.lastAuthorization() &&
            handler.lastAuthorization()->matches(call) &&
            handler.lastAuthorization()->authorityGeneration() == 7,
        "handler did not retain the exact authorization capability");

    std::stop_source cancellation;
    cancellation.request_stop();
    const Domain::OperationContext cancelledContext{
        fixture.operationId,
        context.deadline,
        cancellation.get_token(),
        fixture.correlationId};
    const auto cancelled = handler.handle(authorization, cancelledContext);
    expect(
        !cancelled && cancelled.error().code == Domain::ErrorCodes::Cancelled,
        "tool handler ignored operation cancellation");

    handler.setNow(context.deadline);
    const auto expired = handler.handle(authorization, context);
    expect(
        !expired &&
            expired.error().code == Domain::ErrorCodes::DeadlineExceeded,
        "tool handler ignored the operation deadline");
    handler.setNow(Domain::MonotonicTimePoint{});

    router.cancel(fixture.operationId);
    const auto routerCancelled = router.invoke(call, authority, context);
    expect(
        !routerCancelled &&
            routerCancelled.error().code == Domain::ErrorCodes::Cancelled,
        "tool router ignored targeted cancellation");
}

void testAuthorityCapabilities(const Fixture& fixture)
{
    const auto context = fixture.context();
    Fakes::DeterministicWorkspaceAuthority issuer{
        fixture.authorityId,
        fixture.clientId,
        {fixture.root},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        {Domain::FileAccess::Delete},
        true,
        7};

    const auto authority = take(issuer.authorityFor(fixture.projectId, context));
    expect(authority.authorityId() == fixture.authorityId, "authority ID binding changed");
    expect(authority.projectId() == fixture.projectId, "project binding changed");
    expect(authority.callerId() == fixture.clientId, "caller binding changed");
    expect(
        authority.trustedRoots() == std::vector<Domain::PathText>{fixture.root},
        "trusted roots binding changed");
    expect(authority.intent() == Domain::FileAccess::Read, "authority intent changed");
    expect(
        authority.grants() ==
            std::vector<Domain::FileAccess>{
                Domain::FileAccess::Read,
                Domain::FileAccess::Write},
        "authority grants changed");
    expect(
        authority.denials() ==
            std::vector<Domain::FileAccess>{Domain::FileAccess::Delete},
        "authority denials changed");
    expect(authority.shellEnabled() && authority.generation() == 7,
           "authority shell or generation binding changed");

    const auto narrowed = take(issuer.narrow(
        authority,
        {fixture.root},
        {Domain::FileAccess::Read},
        false,
        8,
        context));
    expect(narrowed.authorityId() == authority.authorityId(),
           "narrowing changed authority identity");
    expect(narrowed.projectId() == authority.projectId(),
           "narrowing changed project identity");
    expect(narrowed.callerId() == authority.callerId(),
           "narrowing changed caller identity");
    expect(narrowed.intent() == authority.intent(),
           "narrowing changed authority intent");
    expect(narrowed.denials() == authority.denials(),
           "narrowing changed authority denials");
    expect(!narrowed.shellEnabled() && narrowed.generation() == 8,
           "narrowing did not disable shell and advance generation");

    expect(
        !issuer.narrow(
            authority,
            {path("C:/outside")},
            {Domain::FileAccess::Read},
            false,
            8,
            context),
        "authority narrowing widened trusted roots");
    expect(
        !issuer.narrow(
            authority,
            {fixture.root},
            {Domain::FileAccess::Execute},
            false,
            8,
            context),
        "authority narrowing widened grants");
    expect(
        !issuer.narrow(
            narrowed,
            {fixture.root},
            {Domain::FileAccess::Read},
            true,
            9,
            context),
        "authority narrowing enabled shell");
    expect(
        !issuer.narrow(
            narrowed,
            {fixture.root},
            {Domain::FileAccess::Read},
            false,
            8,
            context),
        "authority narrowing reused a generation");

    const auto filePath = path("C:/forge-test/file.txt");
    const auto authorizedPath = take(issuer.authorize(
        narrowed,
        Domain::PathAuthorizationRequest{
            filePath,
            std::optional<Domain::PathText>{fixture.root},
            Domain::FileAccess::Read,
            false},
        context));
    expect(authorizedPath.authorityId() == narrowed.authorityId(),
           "authorized path changed authority binding");
    expect(authorizedPath.canonicalPath() == filePath,
           "authorized path changed canonical path");
    expect(authorizedPath.authorityRoot() == fixture.root,
           "authorized path changed root binding");
    expect(authorizedPath.access() == Domain::FileAccess::Read,
           "authorized path changed access binding");
    expect(
        !issuer.authorize(
            narrowed,
            Domain::PathAuthorizationRequest{
                path("C:/outside/file.txt"),
                std::optional<Domain::PathText>{fixture.root},
                Domain::FileAccess::Read,
                false},
            context),
        "authority boundary issued a path outside its root");
    expect(
        !issuer.authorize(
            narrowed,
            Domain::PathAuthorizationRequest{
                filePath,
                std::optional<Domain::PathText>{fixture.root},
                Domain::FileAccess::Write,
                false},
            context),
        "authority boundary issued a path with ungranted access");

    Fakes::DeterministicWorkspaceAuthority invalidIssuer{
        fixture.authorityId,
        fixture.clientId,
        {fixture.root},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {Domain::FileAccess::Read},
        false,
        1};
    expect(
        !invalidIssuer.authorityFor(fixture.projectId, context),
        "authority boundary issued conflicting grants and denials");
}

class LifecycleFake final : public Contracts::IForgeApplicationLifecycle {
public:
    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        started = true;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> stop() noexcept override
    {
        stopped = true;
        return Domain::Result<void>::success();
    }

    bool started{};
    bool stopped{};
};

void testTypedLifecycle()
{
    LifecycleFake lifecycle;
    expect(lifecycle.start().hasValue(), "typed lifecycle start failed");
    expect(lifecycle.stop().hasValue(), "typed lifecycle stop failed");
    expect(lifecycle.started && lifecycle.stopped, "lifecycle calls were not observed");
}

void testCapacityOneMailbox()
{
    Fakes::BoundedLatestValueMailboxFake<int> mailbox;
    int delivered{};
    std::size_t callbacks{};
    auto consumer = mailbox.setConsumer(
        [&](const std::shared_ptr<const int> value) {
            delivered = *value;
            ++callbacks;
        });
    expect(consumer.hasValue(), "mailbox consumer registration failed");

    mailbox.publish(std::make_shared<const int>(1));
    mailbox.publish(std::make_shared<const int>(2));
    mailbox.publish(std::make_shared<const int>(3));

    expect(mailbox.pendingCount() == 1, "mailbox exceeded capacity one");
    expect(*mailbox.latest() == 3, "mailbox did not retain the newest snapshot");
    expect(mailbox.drainOne(), "mailbox did not deliver its pending snapshot");
    expect(delivered == 3 && callbacks == 1, "mailbox delivered an intermediate value");
    expect(mailbox.pendingCount() == 0, "mailbox retained a drained delivery");

    mailbox.shutdown();
    mailbox.publish(std::make_shared<const int>(4));
    expect(mailbox.pendingCount() == 0, "mailbox accepted work after shutdown");
}

void testMcpTransport(const Fixture& fixture)
{
    Fakes::McpTransportFake transport{{"{\"ok\":true}"}, 64, 2};
    const auto context = fixture.context();

    auto frame = transport.receive(context);
    expect(frame.hasValue(), "MCP receive failed");
    expect(frame.value().has_value(), "MCP frame was reported as EOF");
    expect(
        frame.value().value().utf8Json == "{\"ok\":true}",
        "MCP frame changed");

    auto eof = transport.receive(context);
    expect(eof.hasValue() && !eof.value().has_value(), "clean MCP EOF was not distinct");

    expect(
        transport.send(Domain::McpFrame{"{\"result\":1}"}, context).hasValue(),
        "MCP send failed");
    expect(transport.outbound().size() == 1, "MCP output was not retained");

    Fakes::McpTransportFake bounded{{std::string(65, 'x')}, 64, 1};
    auto oversized = bounded.receive(context);
    expect(!oversized.hasValue(), "oversized MCP input was accepted");
    expect(
        oversized.error().code == Domain::ErrorCodes::PayloadTooLarge,
        "oversized MCP input returned the wrong error");
}

void testAllProjectMemoryOperations(const Fixture& fixture)
{
    Fakes::RecordingProjectMemoryService memory;
    const auto context = fixture.context();
    const auto write = fixture.memoryWrite();
    const auto writeAuthority = fixture.authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write});
    const auto exportAuthorization = fixture.authorization(
        "project_memory.export",
        Domain::ToolEffect::Write,
        writeAuthority,
        "{\"project_id\":\"11111111-1111-4111-8111-111111111111\"}");
    memory.exportResult.set(
        Domain::Result<Domain::ProjectMemoryExport>::success(
            Domain::ProjectMemoryExport{
                fixture.projectId,
                fixture.root,
                fixture.digest,
                1}));
    const Domain::DestructiveConfirmation resetConfirmation{
        "reset_project_memory",
        fixture.projectId.value(),
        "confirm-project-reset"};
    memory.resetProjectMemoryResult.set(
        Domain::Result<Domain::ResetReport>::success(
            Domain::ResetReport{
                resetConfirmation.action,
                resetConfirmation.scope,
                1,
                3,
                2,
                4,
                true}));

    (void)memory.initialize(
        Domain::InitializeProjectRequest{
            fixture.root,
            fixture.projectId,
            std::string{"Forge Test"},
            std::nullopt,
            fixture.idempotencyKey},
        context);
    (void)memory.remember(
        Domain::RememberProjectMemoryRequest{fixture.projectId, write},
        context);
    (void)memory.rememberBatch(
        Domain::RememberProjectMemoryBatchRequest{fixture.projectId, {write}},
        context);
    (void)memory.search(
        Domain::SearchProjectMemoryRequest{
            fixture.projectId,
            "typed contracts",
            {},
            {},
            std::nullopt,
            20,
            std::nullopt,
            true,
            64 * 1024},
        context);
    (void)memory.get(
        Domain::GetProjectMemoryRequest{
            fixture.projectId,
            {fixture.recordId},
            true},
        context);
    (void)memory.update(
        Domain::UpdateProjectMemoryRequest{
            fixture.projectId,
            fixture.recordId,
            1,
            std::string{"Updated"},
            std::nullopt,
            std::nullopt,
            std::nullopt},
        context);
    (void)memory.forget(
        Domain::ForgetProjectMemoryRequest{
            fixture.projectId,
            fixture.recordId},
        context);
    (void)memory.listRecent(
        Domain::ListRecentProjectMemoryRequest{
            fixture.projectId,
            {},
            std::nullopt,
            20,
            std::nullopt,
            false,
            64 * 1024},
        context);
    (void)memory.link(
        Domain::LinkProjectMemoryRequest{
            fixture.projectId,
            fixture.recordId,
            fixture.recordId,
            "supports"},
        context);
    (void)memory.exportMemory(
        Domain::ExportProjectMemoryRequest{fixture.projectId},
        writeAuthority,
        exportAuthorization,
        context);
    (void)memory.importMemory(
        Domain::ImportProjectMemoryRequest{
            fixture.projectId,
            fixture.root,
            true,
            false},
        context);
    (void)memory.status(
        Domain::ProjectMemoryStatusRequest{fixture.projectId},
        context);
    expect(
        memory
            .resetProjectMemory(
                fixture.projectId,
                resetConfirmation,
                context)
            .hasValue(),
        "project-memory reset failed");

    for (std::size_t value = 0;
         value <= static_cast<std::size_t>(Fakes::ProjectMemoryCall::Status);
         ++value) {
        expect(
            memory.callCount(static_cast<Fakes::ProjectMemoryCall>(value)) == 1,
            "one of the twelve project-memory operations was not recorded");
    }
    expect(
        memory.lastProjectId() &&
            memory.lastProjectId().value() == fixture.projectId,
        "project-memory scope was not retained");
    expect(
        memory.lastOperationId() &&
            memory.lastOperationId().value() == fixture.operationId,
        "project-memory operation context was not retained");
    expect(
        memory.lastExportAuthority() &&
            memory.lastExportAuthority()->intent() == Domain::FileAccess::Write &&
            memory.lastExportAuthority()->authorityId() == writeAuthority.authorityId(),
        "project-memory export did not retain explicit write authority");
    expect(
        memory.lastExportAuthorization() &&
            memory.lastExportAuthorization()->toolName() ==
                "project_memory.export" &&
            memory.lastExportAuthorization()->canonicalRequest() ==
                "{\"project_id\":\"11111111-1111-4111-8111-111111111111\"}" &&
            memory.lastExportAuthorization()->effect() ==
                Domain::ToolEffect::Write &&
            memory.lastExportAuthorization()->matchesProject(fixture.projectId),
        "project-memory export did not retain exact authorization evidence");

    const auto wrongOperationAuthorization = fixture.authorization(
        "agent_run_status",
        Domain::ToolEffect::Write,
        writeAuthority);
    const auto wrongOperation = memory.exportMemory(
        Domain::ExportProjectMemoryRequest{fixture.projectId},
        writeAuthority,
        wrongOperationAuthorization,
        context);
    expect(
        !wrongOperation &&
            wrongOperation.error().code == Domain::ErrorCodes::Unauthorized,
        "project-memory export accepted a capability for another operation");
    expect(
        memory.callCount(Fakes::ProjectMemoryCall::Export) == 1,
        "rejected export reached the project-memory operation");

    expect(
        memory.callCount(Fakes::ProjectMemoryCall::ResetProjectMemory) == 1 &&
            memory.lastResetConfirmation() &&
            memory.lastResetConfirmation()->action == resetConfirmation.action &&
            memory.lastResetConfirmation()->scope == resetConfirmation.scope &&
            memory.lastResetConfirmation()->token == resetConfirmation.token,
        "project-memory reset did not propagate exact confirmation evidence");
}

void testDeadlineScheduler(const Fixture& fixture)
{
    Fakes::FakeDeadlineScheduler scheduler;
    const auto context = fixture.context();

    expect(
        scheduler.waitUntil(context).hasValue(),
        "deadline scheduler rejected an active operation context");
    expect(
        scheduler.calls() == 1 && scheduler.lastDeadline() == context.deadline,
        "deadline scheduler did not retain the operation deadline");
    expect(
        scheduler.lastOperationId() &&
            scheduler.lastOperationId().value() == context.operationId,
        "deadline scheduler did not retain the operation identity");
    expect(
        scheduler.lastCorrelationId() &&
            scheduler.lastCorrelationId().value() == context.correlationId,
        "deadline scheduler did not retain the correlation identity");

    scheduler.shutdown();
    const auto cancelled = scheduler.waitUntil(context);
    expect(!cancelled, "deadline scheduler accepted work after shutdown");
    expect(
        cancelled.error().code == Domain::ErrorCodes::Cancelled,
        "deadline scheduler returned the wrong shutdown error");
}

void testExternalServiceSeams(const Fixture& fixture)
{
    const auto context = fixture.context();
    const auto readAuthority = fixture.authority(
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read});
    const auto mutationAuthority = fixture.authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write});
    const auto lmDeployAuthorization = fixture.authorization(
        "install-lmstudio-plugin",
        Domain::ToolEffect::Write,
        mutationAuthority);
    const auto executionAuthority = fixture.authority();
    const auto lmActivateAuthorization = fixture.authorization(
        "install-lmstudio-plugin",
        Domain::ToolEffect::Execute,
        executionAuthority);
    const auto installerAuthorization = fixture.authorization(
        "install",
        Domain::ToolEffect::Write,
        mutationAuthority);

    Fakes::RecordingLMStudioEnvironmentFake environment;
    environment.inspectResult.set(
        Domain::Result<Domain::LMStudioEnvironmentStatus>::success(
            Domain::LMStudioEnvironmentStatus{
                true,
                fixture.root,
                fixture.root,
                std::string{"1.0.0"}}));
    environment.connectionHealthResult.set(
        Domain::Result<Domain::LMStudioConnectionHealth>::success(
            Domain::LMStudioConnectionHealth{
                {Domain::LMStudioConnectorHealth{
                    Domain::LMStudioConnectorRole::Primary,
                    true,
                    std::string{"1.0"},
                    53,
                    "ready"}},
                Domain::LMStudioConnectionState::Ready}));

    expect(
        environment
            .inspect(
                std::optional<Domain::PathText>{fixture.root},
                readAuthority,
                context)
            .hasValue(),
        "LM Studio environment inspection failed");
    expect(
        environment.connectionHealth(context).hasValue(),
        "LM Studio connection-health query failed");
    expect(
        environment.inspectCalls() == 1 &&
            environment.connectionHealthCalls() == 1,
        "LM Studio environment fake did not record both queries");
    expect(
        environment.lastReadAuthority() &&
            environment.lastReadAuthority()->intent() == Domain::FileAccess::Read,
        "LM Studio inspection did not retain read authority");
    environment.shutdown();
    const auto stoppedEnvironment = environment.connectionHealth(context);
    expect(
        !stoppedEnvironment &&
            stoppedEnvironment.error().code == Domain::ErrorCodes::Cancelled,
        "LM Studio environment accepted work after shutdown");

    const Domain::LMStudioDeploymentRequest lmStudioRequest{
        std::optional<Domain::PathText>{fixture.root},
        true};
    Fakes::RecordingLMStudioDeploymentServiceFake lmStudioDeployment;
    lmStudioDeployment.statusResult.set(
        Domain::Result<Domain::LMStudioPluginStatus>::success(
            Domain::LMStudioPluginStatus{
                true,
                true,
                true,
                fixture.root,
                true,
                true,
                fixture.root,
                fixture.root,
                fixture.root,
                fixture.deploymentId,
                "ready"}));
    lmStudioDeployment.deployResult.set(
        Domain::Result<Domain::LMStudioInstallResult>::success(
            Domain::LMStudioInstallResult{
                true,
                fixture.root,
                {fixture.root},
                fixture.root,
                fixture.deploymentId,
                "installed"}));
    lmStudioDeployment.activateResult.set(
        Domain::Result<Domain::LMStudioHostActivationResult>::success(
            Domain::LMStudioHostActivationResult{
                fixture.deploymentId,
                false,
                true,
                false,
                true,
                {Domain::LMStudioConnectorRole::Primary,
                 Domain::LMStudioConnectorRole::Fallback},
                "ready"}));

    expect(
        lmStudioDeployment
            .status(lmStudioRequest, readAuthority, context)
            .hasValue(),
        "LM Studio deployment status failed");
    expect(
        lmStudioDeployment
            .deploy(
                lmStudioRequest,
                mutationAuthority,
                lmDeployAuthorization,
                context)
            .hasValue(),
        "LM Studio deployment failed");
    expect(
        lmStudioDeployment.lastAuthority() &&
            lmStudioDeployment.lastAuthority()->intent() ==
                Domain::FileAccess::Write,
        "LM Studio deployment did not retain write authority");
    expect(
        lmStudioDeployment.lastAuthorization() &&
            lmStudioDeployment.lastAuthorization()->matches(
                mutationAuthority,
                context) &&
            lmStudioDeployment.lastAuthorization()->effect() ==
                Domain::ToolEffect::Write,
        "LM Studio deployment did not retain exact authorization evidence");
    const Domain::LMStudioHostActivationRequest activationRequest{
        fixture.deploymentId,
        5s};
    expect(
        lmStudioDeployment
            .activate(
                activationRequest,
                executionAuthority,
                lmActivateAuthorization,
                context)
            .hasValue(),
        "LM Studio host activation failed");
    expect(
        lmStudioDeployment.statusCalls() == 1 &&
            lmStudioDeployment.deployCalls() == 1 &&
            lmStudioDeployment.activateCalls() == 1,
        "LM Studio deployment fake did not record all operations");
    expect(
        lmStudioDeployment.lastAuthorization() &&
            lmStudioDeployment.lastAuthorization()->matches(
                executionAuthority,
                context) &&
            lmStudioDeployment.lastAuthorization()->effect() ==
                Domain::ToolEffect::Execute,
        "LM Studio activation did not retain exact authorization evidence");

    const auto wrongEffect = lmStudioDeployment.deploy(
        lmStudioRequest,
        mutationAuthority,
        lmActivateAuthorization,
        context);
    expect(
        !wrongEffect &&
            wrongEffect.error().code == Domain::ErrorCodes::Unauthorized,
        "LM Studio deployment accepted a mismatched effect");

    const auto newerAuthority = fixture.authority(
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        true,
        2);
    const auto wrongAuthority = lmStudioDeployment.deploy(
        lmStudioRequest,
        newerAuthority,
        lmDeployAuthorization,
        context);
    expect(
        !wrongAuthority &&
            wrongAuthority.error().code == Domain::ErrorCodes::Unauthorized,
        "LM Studio deployment accepted a stale authority generation");

    auto projectlessCall = fixture.toolCall("install-lmstudio-plugin");
    projectlessCall.metadata.projectId.reset();
    Fakes::DeterministicToolAuthorizerFake projectlessAuthorizer{
        "install-lmstudio-plugin",
        Domain::ToolEffect::Write};
    const auto projectlessAuthorization = take(projectlessAuthorizer.authorize(
        Domain::ToolAuthorizationRequest{
            projectlessCall,
            Domain::ToolEffect::Write,
            Domain::AuthorityReference{
                mutationAuthority.authorityId(),
                mutationAuthority.generation()}},
        mutationAuthority,
        context));
    const auto missingProject = lmStudioDeployment.deploy(
        lmStudioRequest,
        mutationAuthority,
        projectlessAuthorization,
        context);
    expect(
        !missingProject &&
            missingProject.error().code == Domain::ErrorCodes::Unauthorized,
        "LM Studio deployment accepted an unbound project");
    lmStudioDeployment.cancel(fixture.operationId);
    const auto cancelledDeployment =
        lmStudioDeployment.status(lmStudioRequest, readAuthority, context);
    expect(
        !cancelledDeployment &&
            cancelledDeployment.error().code == Domain::ErrorCodes::Cancelled,
        "LM Studio deployment did not honor operation cancellation");
    lmStudioDeployment.shutdown();

    const Domain::GraphicsDeviceStatus deviceStatus{
        Domain::GraphicsDeviceState::Ready,
        "Test Adapter",
        std::uint64_t{1024},
        1};
    Fakes::RecordingGraphicsDeviceServiceFake graphicsDevice;
    graphicsDevice.initializeResult.set(
        Domain::Result<Domain::GraphicsDeviceStatus>::success(deviceStatus));
    graphicsDevice.statusResult.set(
        Domain::Result<Domain::GraphicsDeviceStatus>::success(deviceStatus));
    graphicsDevice.recoverResult.set(
        Domain::Result<Domain::GraphicsDeviceStatus>::success(deviceStatus));
    expect(
        graphicsDevice.initialize(context).hasValue(),
        "graphics-device initialization failed");
    expect(
        graphicsDevice.status(context).hasValue(),
        "graphics-device status failed");
    expect(
        graphicsDevice.recover(context).hasValue(),
        "graphics-device recovery failed");
    expect(
        graphicsDevice.initializeCalls() == 1 &&
            graphicsDevice.statusCalls() == 1 &&
            graphicsDevice.recoverCalls() == 1,
        "graphics-device fake did not record all operations");
    graphicsDevice.cancel(fixture.operationId);
    const auto cancelledGraphics = graphicsDevice.status(context);
    expect(
        !cancelledGraphics &&
            cancelledGraphics.error().code == Domain::ErrorCodes::Cancelled,
        "graphics-device service did not honor operation cancellation");
    graphicsDevice.shutdown();

    const Domain::RenderRequest renderRequest{
        640,
        480,
        1.0,
        Domain::GraphicsVisibility::Visible,
        7};
    Fakes::RecordingRenderServiceFake renderService;
    renderService.renderResult.set(
        Domain::Result<Domain::RenderOutcome>::success(
            Domain::RenderOutcome{true, false, false, 1}));
    expect(
        renderService.render(renderRequest, context).hasValue(),
        "render service failed");
    expect(
        renderService.lastRequest() &&
            renderService.lastRequest()->contentRevision ==
                renderRequest.contentRevision,
        "render service did not retain its bounded latest request");
    renderService.setNow(context.deadline);
    const auto expiredRender = renderService.render(renderRequest, context);
    expect(
        !expiredRender &&
            expiredRender.error().code == Domain::ErrorCodes::DeadlineExceeded,
        "render service did not honor the operation deadline");
    renderService.setNow(Domain::MonotonicTimePoint{});
    renderService.cancel(fixture.operationId);
    const auto cancelledRender = renderService.render(renderRequest, context);
    expect(
        !cancelledRender &&
            cancelledRender.error().code == Domain::ErrorCodes::Cancelled,
        "render service did not honor operation cancellation");
    renderService.shutdown();

    const Domain::DeploymentStatus deploymentStatus{
        Domain::DeploymentState::Installed,
        std::optional<std::string>{"1.0.0"},
        std::optional<Domain::PathText>{fixture.root},
        true,
        true,
        true};
    Fakes::RecordingInstallerDeploymentServiceFake installer;
    installer.statusResult.set(
        Domain::Result<Domain::DeploymentStatus>::success(deploymentStatus));
    installer.executeResult.set(
        Domain::Result<Domain::DeploymentReport>::success(
            Domain::DeploymentReport{
                Domain::DeploymentAction::Install,
                true,
                deploymentStatus,
                {"install"},
                std::nullopt}));
    const Domain::DeploymentRequest deploymentRequest{
        Domain::DeploymentAction::Install,
        std::optional<std::string>{"1.0.0"},
        true,
        std::nullopt};

    expect(
        installer.status(context).hasValue(),
        "installer deployment status failed");
    expect(
        installer
            .execute(
                deploymentRequest,
                mutationAuthority,
                installerAuthorization,
                context)
            .hasValue(),
        "installer deployment execution failed");
    expect(
        installer.statusCalls() == 1 && installer.executeCalls() == 1,
        "installer deployment fake did not record all operations");
    expect(
        installer.lastAuthority() &&
            installer.lastAuthority()->intent() == Domain::FileAccess::Write,
        "installer deployment did not retain mutation authority");
    expect(
        installer.lastAuthorization() &&
            installer.lastAuthorization()->matches(
                mutationAuthority,
                context) &&
            installer.lastAuthorization()->effect() ==
                Domain::ToolEffect::Write,
        "installer deployment did not retain exact authorization evidence");

    const Domain::OperationContext otherCorrelationContext{
        fixture.operationId,
        context.deadline,
        context.cancellation,
        fixture.otherCorrelationId};
    const auto wrongCorrelation = installer.execute(
        deploymentRequest,
        mutationAuthority,
        installerAuthorization,
        otherCorrelationContext);
    expect(
        !wrongCorrelation &&
            wrongCorrelation.error().code == Domain::ErrorCodes::Unauthorized,
        "installer deployment accepted a mismatched correlation");
    installer.cancel(fixture.operationId);
    const auto cancelledInstaller = installer.status(context);
    expect(
        !cancelledInstaller &&
            cancelledInstaller.error().code == Domain::ErrorCodes::Cancelled,
        "installer deployment did not honor operation cancellation");
    installer.shutdown();
}

void testProcessAuthorityAndCancellation(const Fixture& fixture)
{
    Fakes::RecordingProcessSupervisor process;
    process.runResult.set(Domain::Result<Domain::ProcessResult>::success(
        Domain::ProcessResult{
            0,
            "ok",
            {},
            false,
            false,
            false,
            false,
            true,
            1ms}));

    const Domain::ProcessRequest request{
        path("C:/forge-test/tool.exe"),
        {"--version"},
        fixture.root,
        {},
        false,
        1s,
        80'000,
        20'000};
    const auto authority = fixture.authority();
    const auto context = fixture.context();

    auto result = process.run(request, authority, context);
    expect(result.hasValue(), "scripted process result failed");
    expect(
        process.lastAuthority() &&
            process.lastAuthority()->authorityId() == fixture.authorityId,
        "process authority was not retained");
    expect(
        process.lastOperationId() &&
            process.lastOperationId().value() == fixture.operationId,
        "process operation ID was not retained");

    process.cancel(fixture.operationId);
    auto cancelled = process.run(request, authority, context);
    expect(!cancelled.hasValue(), "targeted process cancellation was ignored");
    expect(
        cancelled.error().code == Domain::ErrorCodes::Cancelled,
        "targeted process cancellation returned the wrong error");
}

void testSessionHostContract(const Fixture& fixture)
{
    Fakes::RecordingSessionHostAdapter host{
        fixture.adapterId,
        "1.0.0"};
    const auto context = fixture.context();
    const auto session = fixture.successorSession();
    const auto handoff = fixture.handoff();
    const Domain::SessionCreationRequest creation{
        fixture.continuityOperationId,
        fixture.projectId,
        fixture.predecessorSessionId,
        fixture.idempotencyKey};
    const Domain::HandoffAcknowledgement acknowledgement{
        fixture.handoffId,
        fixture.successorSessionId,
        fixture.adapterId,
        fixture.digest};

    host.capabilitiesResult.set(
        Domain::Result<Domain::HostCapabilities>::success(
            Domain::HostCapabilities{
                true, true, true, true, true, true, true, true}));
    host.createSessionResult.set(
        Domain::Result<Domain::HostSession>::success(session));
    host.queryByIdempotencyKeyResult.set(
        Domain::Result<std::optional<Domain::HostSession>>::success(session));
    host.bootstrapResult.set(Domain::Result<void>::success());
    host.acknowledgementResult.set(
        Domain::Result<Domain::HandoffAcknowledgement>::success(
            acknowledgement));
    host.queryResult.set(
        Domain::Result<Domain::HostSessionStatus>::success(
            Domain::HostSessionStatus::Ready));
    host.recoverResult.set(
        Domain::Result<Domain::HostRecoveryReport>::success(
            Domain::HostRecoveryReport{1, 1, 0, 0, {session}}));

    Fakes::RecordingSessionHostAdapter unbootstrappedHost{
        fixture.adapterId,
        "1.0.0"};
    const auto noBootstrap = unbootstrappedHost.awaitAcknowledgement(
        session,
        fixture.handoffId,
        fixture.digest,
        context);
    expect(
        !noBootstrap &&
            noBootstrap.error().code == Domain::ErrorCodes::IntegrityFailure,
        "host acknowledgement succeeded without a prior bootstrap");

    expect(host.identifier() == fixture.adapterId, "host adapter ID changed");
    expect(host.version() == "1.0.0", "host adapter version changed");
    expect(host.capabilities(context).hasValue(), "host capabilities failed");
    expect(host.createSession(creation, context).hasValue(), "host create failed");
    expect(
        host.queryByIdempotencyKey(
                fixture.projectId,
                fixture.idempotencyKey,
                context)
            .hasValue(),
        "host idempotency query failed");
    expect(
        host.bootstrap(session, handoff, context).hasValue(),
        "host bootstrap failed");

    auto projectMismatchSession = session;
    projectMismatchSession.projectId = fixture.otherProjectId;
    const auto projectMismatch =
        host.bootstrap(projectMismatchSession, handoff, context);
    expect(
        !projectMismatch &&
            projectMismatch.error().code ==
                Domain::ErrorCodes::ProjectScopeMismatch,
        "host bootstrap accepted a cross-project session");

    auto operationMismatchSession = session;
    operationMismatchSession.operationId =
        fixture.otherContinuityOperationId;
    const auto operationMismatch =
        host.bootstrap(operationMismatchSession, handoff, context);
    expect(
        !operationMismatch &&
            operationMismatch.error().code == Domain::ErrorCodes::IntegrityFailure,
        "host bootstrap accepted a cross-operation session");

    auto predecessorMismatchSession = session;
    predecessorMismatchSession.predecessorSessionId = fixture.otherSessionId;
    const auto predecessorMismatch =
        host.bootstrap(predecessorMismatchSession, handoff, context);
    expect(
        !predecessorMismatch &&
            predecessorMismatch.error().code ==
                Domain::ErrorCodes::IntegrityFailure,
        "host bootstrap accepted a mismatched predecessor");

    auto successorMismatchHandoff = handoff;
    successorMismatchHandoff.successorSession.value().sessionId =
        fixture.otherSessionId;
    const auto successorMismatch =
        host.bootstrap(session, successorMismatchHandoff, context);
    expect(
        !successorMismatch &&
            successorMismatch.error().code ==
                Domain::ErrorCodes::IntegrityFailure,
        "host bootstrap accepted a mismatched successor");
    expect(
        host.awaitAcknowledgement(
                session,
                fixture.handoffId,
                fixture.digest,
                context)
            .hasValue(),
        "exact host acknowledgement failed");

    const auto expectSubstitutedSessionRejected =
        [&](const Domain::HostSession& substituted, const std::string_view message) {
            const auto result = host.awaitAcknowledgement(
                substituted,
                fixture.handoffId,
                fixture.digest,
                context);
            expect(
                !result &&
                    result.error().code == Domain::ErrorCodes::IntegrityFailure,
                message);
        };
    auto acknowledgementProjectMismatch = session;
    acknowledgementProjectMismatch.projectId = fixture.otherProjectId;
    expectSubstitutedSessionRejected(
        acknowledgementProjectMismatch,
        "host acknowledgement accepted a same-ID cross-project session");

    auto acknowledgementOperationMismatch = session;
    acknowledgementOperationMismatch.operationId =
        fixture.otherContinuityOperationId;
    expectSubstitutedSessionRejected(
        acknowledgementOperationMismatch,
        "host acknowledgement accepted a same-ID cross-operation session");

    auto acknowledgementIdempotencyMismatch = session;
    acknowledgementIdempotencyMismatch.idempotencyKey =
        take(Domain::IdempotencyKey::create("rollover-other"));
    expectSubstitutedSessionRejected(
        acknowledgementIdempotencyMismatch,
        "host acknowledgement accepted substituted idempotency evidence");

    auto acknowledgementPredecessorMismatch = session;
    acknowledgementPredecessorMismatch.predecessorSessionId =
        fixture.otherSessionId;
    expectSubstitutedSessionRejected(
        acknowledgementPredecessorMismatch,
        "host acknowledgement accepted a substituted predecessor session");

    auto acknowledgementProviderMismatch = session;
    acknowledgementProviderMismatch.providerSessionId.reset();
    expectSubstitutedSessionRejected(
        acknowledgementProviderMismatch,
        "host acknowledgement accepted substituted provider-session evidence");

    auto acknowledgementModelMismatch = session;
    acknowledgementModelMismatch.model = std::string{"other-model"};
    expectSubstitutedSessionRejected(
        acknowledgementModelMismatch,
        "host acknowledgement accepted substituted model evidence");

    auto acknowledgementStatusMismatch = session;
    acknowledgementStatusMismatch.status = Domain::HostSessionStatus::Active;
    expectSubstitutedSessionRejected(
        acknowledgementStatusMismatch,
        "host acknowledgement accepted substituted session status");
    expect(
        host.query(fixture.successorSessionId, context).hasValue(),
        "host session query failed");
    expect(
        host.recover(
                Domain::HostRecoveryRequest{
                    fixture.projectId,
                    fixture.continuityOperationId,
                    true},
                context)
            .hasValue(),
        "host recovery failed");

    expect(
        host.lastAcknowledgementHandoffId() &&
            host.lastAcknowledgementHandoffId().value() == fixture.handoffId,
        "host acknowledgement handoff ID was not retained");
    expect(
        host.lastAcknowledgementSha256() &&
            host.lastAcknowledgementSha256().value() == fixture.digest,
        "host acknowledgement checksum was not retained");

    host.acknowledgementResult.set(
        Domain::Result<Domain::HandoffAcknowledgement>::success(
            Domain::HandoffAcknowledgement{
                fixture.handoffId,
                fixture.successorSessionId,
                fixture.adapterId,
                fixture.otherDigest}));
    auto mismatch = host.awaitAcknowledgement(
        session,
        fixture.handoffId,
        fixture.digest,
        context);
    expect(!mismatch.hasValue(), "mismatched acknowledgement was accepted");
    expect(
        mismatch.error().code == Domain::ErrorCodes::IntegrityFailure,
        "mismatched acknowledgement returned the wrong error");
}

void testContinuityLifecycle(const Fixture& fixture)
{
    Fakes::RecordingContinuityCoordinator continuity;
    const auto context = fixture.context();
    const auto handoff = fixture.handoff();
    const Domain::CheckpointRequest checkpoint{handoff};
    const Domain::HandoffAcknowledgement acknowledgement{
        fixture.handoffId,
        fixture.successorSessionId,
        fixture.adapterId,
        fixture.digest};

    (void)continuity.checkpoint(checkpoint, context);
    (void)continuity.prepareHandoff(checkpoint, context);
    (void)continuity.getPendingHandoff(fixture.projectId, context);
    (void)continuity.acknowledgeHandoff(
        fixture.projectId,
        fixture.continuityOperationId,
        acknowledgement,
        context);
    (void)continuity.resume(
        Domain::HandoffResumeRequest{
            fixture.projectId,
            fixture.handoffId,
            fixture.successorSessionId},
        context);
    (void)continuity.status(fixture.projectId, context);
    (void)continuity.requestRollover(
        Domain::RolloverRequest{
            fixture.projectId,
            fixture.continuityOperationId},
        context);
    (void)continuity.recoverIncompleteOperations(
        Domain::ContinuityRecoveryRequest{fixture.projectId, true},
        context);
    (void)continuity.resetProjectContinuity(
        Domain::ContinuityResetRequest{
            fixture.projectId,
            Domain::DestructiveConfirmation{
                "reset_continuity",
                fixture.projectId.value(),
                "test-confirmation"}},
        context);

    for (std::size_t value = 0;
         value < static_cast<std::size_t>(Fakes::ContinuityCall::Count);
         ++value) {
        expect(
            continuity.callCount(static_cast<Fakes::ContinuityCall>(value)) == 1,
            "one continuity lifecycle call was not recorded");
    }
    expect(
        continuity.lastProjectId() &&
            continuity.lastProjectId().value() == fixture.projectId,
        "continuity project scope was not retained");
}

std::shared_ptr<const Domain::TelemetrySnapshot> telemetrySnapshot(
    const Fixture& fixture,
    std::string runtime)
{
    Domain::SystemMetrics system{
        Domain::UtcTimePoint{},
        "host",
        "windows",
        "x64",
        Domain::CpuMetrics{},
        Domain::RamMetrics{},
        {},
        Domain::DiskIoMetrics{},
        {},
        {},
        Domain::PowerMetrics{}};
    Domain::ForgeSnapshot forge{
        Domain::UtcTimePoint{},
        fixture.root,
        runtime,
        0,
        0,
        {},
        {},
        0,
        Domain::TelemetryHealth::Ok};
    return std::make_shared<const Domain::TelemetrySnapshot>(
        Domain::TelemetrySnapshot{
            std::move(system),
            std::move(forge),
            Domain::UtcTimePoint{},
            {},
            std::move(runtime)});
}

void testTelemetryService(const Fixture& fixture)
{
    Fakes::RecordingTelemetryService telemetry;
    const auto context = fixture.context();
    telemetry.startResult.set(Domain::Result<void>::success());

    std::string deliveredRuntime;
    expect(
        telemetry.setConsumer(
                [&](const Contracts::ITelemetryService::Snapshot snapshot) {
                    deliveredRuntime = snapshot->runtime;
                })
            .hasValue(),
        "telemetry consumer registration failed");
    expect(telemetry.start(context).hasValue(), "telemetry start failed");

    auto first = telemetrySnapshot(fixture, "first");
    telemetry.sampleResult.set(
        Domain::Result<Contracts::ITelemetryService::Snapshot>::success(first));
    expect(telemetry.sample(false, context).hasValue(), "first telemetry sample failed");

    auto second = telemetrySnapshot(fixture, "second");
    telemetry.sampleResult.set(
        Domain::Result<Contracts::ITelemetryService::Snapshot>::success(second));
    expect(telemetry.sample(true, context).hasValue(), "second telemetry sample failed");

    expect(
        telemetry.pendingCount() == Domain::TelemetryPendingSnapshotsMaximum,
        "telemetry service exceeded the capacity-one mailbox");
    expect(
        telemetry.latest() && telemetry.latest()->runtime == "second",
        "telemetry service did not retain the newest snapshot");
    expect(telemetry.drainOne(), "telemetry delivery did not drain");
    expect(
        deliveredRuntime == "second",
        "telemetry delivered an intermediate snapshot");

    telemetry.stop();
    expect(!telemetry.running(), "telemetry stop did not close the service");
}

} // namespace

int main()
{
    try {
        const Fixture fixture;
        testAuthorizedToolCapability(fixture);
        ForgeConductor::Tests::runGroupedFakeContractTests();
        testAuthorityCapabilities(fixture);
        testTypedLifecycle();
        ForgeConductor::Tests::runFoundationTelemetryFakeContractTests();
        testCapacityOneMailbox();
        testMcpTransport(fixture);
        testAllProjectMemoryOperations(fixture);
        testDeadlineScheduler(fixture);
        testExternalServiceSeams(fixture);
        testProcessAuthorityAndCancellation(fixture);
        testSessionHostContract(fixture);
        testContinuityLifecycle(fixture);
        testTelemetryService(fixture);
        std::cout << "Contracts and deterministic fakes passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Contract test failure: " << error.what() << '\n';
        return 1;
    }
}
