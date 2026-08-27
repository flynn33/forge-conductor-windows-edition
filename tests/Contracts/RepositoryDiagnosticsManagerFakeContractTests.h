#pragma once

#include "Fakes/ContinuityRepositoryFake.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/ManagerFakes.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Fakes/ToolServiceFakes.h"
#include "ForgeConductor/Contracts/ILegacyMemoryRepository.h"

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ForgeConductor::Tests {
namespace RepositoryDiagnosticsManagerFakeContractTestsDetail {

namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace ProductContracts = ForgeConductor::Contracts;
using namespace std::chrono_literals;

static_assert(std::is_final_v<Fakes::ProjectRegistryRepositoryFake>);
static_assert(std::is_final_v<Fakes::ProjectMemoryRepositoryFake>);
static_assert(std::is_base_of_v<
    ProductContracts::IProjectRepository,
    Fakes::ProjectMemoryRepositoryFake>);
static_assert(std::is_final_v<Fakes::ProjectMemoryRepositoryFactoryFake>);
static_assert(std::is_final_v<Fakes::LegacyMemoryServiceFake>);
static_assert(std::is_abstract_v<ProductContracts::ILegacyMemoryRepository>);
static_assert(
    std::has_virtual_destructor_v<ProductContracts::ILegacyMemoryRepository>);
static_assert(std::is_final_v<Fakes::ContinuityRepositoryFake>);
static_assert(std::is_final_v<Fakes::DiagnosticSinkFake>);
static_assert(std::is_final_v<Fakes::AuditRepositoryFake>);
static_assert(std::is_final_v<Fakes::DoctorServiceFake>);
static_assert(std::is_final_v<Fakes::RuntimeDiagnosticsFake>);
static_assert(std::is_final_v<Fakes::ManagerClientFake>);
static_assert(std::is_final_v<Fakes::ManagerServerFake>);
static_assert(!std::is_copy_constructible_v<Contracts::RuntimeOwnershipLease>);
static_assert(!std::is_copy_assignable_v<Contracts::RuntimeOwnershipLease>);
static_assert(std::is_nothrow_move_constructible_v<Contracts::RuntimeOwnershipLease>);

inline void require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, message);
}

struct Fixture final {
    Domain::ProjectId projectId{parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111")};
    Domain::ProjectId otherProjectId{parse<Domain::ProjectId>(
        "22222222-2222-4222-8222-222222222222")};
    Domain::OperationId operationId{parse<Domain::OperationId>(
        "33333333-3333-4333-8333-333333333333")};
    Domain::OperationId otherOperationId{parse<Domain::OperationId>(
        "44444444-4444-4444-8444-444444444444")};
    Domain::CorrelationId correlationId{
        parse<Domain::CorrelationId>("repository-fake-test")};
    Domain::PathText root{take(Domain::PathText::create("C:\\workspace"))};
    Domain::PathText artifact{
        take(Domain::PathText::create("C:\\workspace\\artifact.json"))};
    Domain::MonotonicTimePoint now{Domain::MonotonicTimePoint{} + 100ms};

    [[nodiscard]] Domain::OperationContext activeContext() const
    {
        return Domain::OperationContext{
            operationId,
            now + 1s,
            std::stop_token{},
            correlationId};
    }

    [[nodiscard]] Domain::OperationContext expiredContext() const
    {
        return Domain::OperationContext{
            operationId,
            now,
            std::stop_token{},
            correlationId};
    }

    [[nodiscard]] Domain::OperationContext cancelledContext() const
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        return Domain::OperationContext{
            operationId,
            now + 1s,
            cancellation.get_token(),
            correlationId};
    }

    [[nodiscard]] Domain::Sha256Digest digest() const
    {
        return parse<Domain::Sha256Digest>(std::string(64, 'a'));
    }

    [[nodiscard]] ProductContracts::WorkspaceAuthority authorityFor(
        const Domain::ProjectId& authorityProjectId,
        const Domain::FileAccess intent) const
    {
        Fakes::DeterministicWorkspaceAuthority issuer{
            parse<Domain::AuthorityId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            parse<Domain::ClientId>("repository-test-client"),
            {root},
            intent,
            {Domain::FileAccess::Read, Domain::FileAccess::Write},
            {Domain::FileAccess::Delete},
            false,
            7};
        return take(issuer.authorityFor(authorityProjectId, activeContext()));
    }

    [[nodiscard]] ProductContracts::WorkspaceAuthority authority() const
    {
        return authorityFor(projectId, Domain::FileAccess::Write);
    }

    [[nodiscard]] ProductContracts::AuthorizedToolCall authorization(
        const ProductContracts::WorkspaceAuthority& authority,
        std::string toolName,
        const Domain::ToolEffect effect,
        std::optional<Domain::ProjectId> authorizationProjectId) const
    {
        const Domain::ToolCallRequest call{
            Domain::McpRequestMetadata{
                parse<Domain::RequestId>("export-request"),
                correlationId,
                parse<Domain::ClientId>("repository-test-client"),
                std::move(authorizationProjectId),
                "1.0"},
            toolName,
            "{}"};
        const Domain::ToolAuthorizationRequest request{
            call,
            effect,
            Domain::AuthorityReference{
                authority.authorityId(), authority.generation()}};
        Fakes::DeterministicToolAuthorizerFake authorizer{
            std::move(toolName), effect, now};
        return take(authorizer.authorize(request, authority, activeContext()));
    }

    [[nodiscard]] ProductContracts::AuthorizedToolCall exportAuthorization(
        const ProductContracts::WorkspaceAuthority& authority) const
    {
        return authorization(
            authority,
            "project_memory.export",
            Domain::ToolEffect::Write,
            projectId);
    }

};

inline void testProjectRegistry(const Fixture& fixture)
{
    Fakes::ProjectRegistryRepositoryFake registry{1, fixture.now};
    const Domain::ProjectMemoryDescriptor descriptor{
        fixture.projectId,
        "project",
        std::nullopt,
        {fixture.root}};
    require(registry.seedDescriptor(descriptor).hasValue(),
            "registry rejected its first bounded descriptor");
    requireError(
        registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
            fixture.otherProjectId, "other", std::nullopt, {fixture.root}}),
        Domain::ErrorCodes::LimitExceeded,
        "registry exceeded its descriptor capacity");

    const Domain::ProjectInitialization initialization{
        descriptor,
        Domain::ProjectMemorySchemaVersion,
        Domain::ProjectMemoryCapabilityVersion,
        {},
        true,
        false,
        true};
    require(registry.seedInitialization(initialization).hasValue(),
            "registry did not seed initialization");
    const Domain::InitializeProjectRequest initializeRequest{
        fixture.root, fixture.projectId, "project", std::nullopt, std::nullopt};
    const auto initialized = registry.initialize(
        initializeRequest, fixture.activeContext());
    require(initialized && initialized.value().project.id == fixture.projectId,
            "registry did not preserve the requested project binding");
    const auto listed = registry.list(1, fixture.activeContext());
    require(listed && listed.value().size() == 1,
            "registry did not return its bounded descriptor list");
    require(registry.detachAlias(
                fixture.projectId, fixture.root, fixture.activeContext()).hasValue(),
            "registry did not preserve the typed detach-alias request");
    require(registry.lastRequest().has_value(),
            "registry did not retain its latest typed request");
    requireError(
        registry.descriptor(fixture.projectId, fixture.expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded,
        "registry ignored an expired operation");
    registry.shutdown();
    requireError(
        registry.list(1, fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "registry accepted work after shutdown");
}

inline void testProjectMemoryRepository(const Fixture& fixture)
{
    Fakes::ProjectMemoryRepositoryFake repository{fixture.projectId, fixture.now};
    repository.statusResult.set(
        Domain::Result<Domain::ProjectMemoryStatus>::success(
            Domain::ProjectMemoryStatus{fixture.projectId}));
    repository.exportResult.set(
        Domain::Result<Domain::ProjectMemoryExport>::success(
            Domain::ProjectMemoryExport{
                fixture.projectId, fixture.artifact, fixture.digest(), 1}));
    repository.quickCheckResult.set(Domain::Result<void>::success());

    const auto status = repository.status(
        Domain::ProjectMemoryStatusRequest{fixture.projectId},
        fixture.activeContext());
    require(status && status.value().projectId == fixture.projectId,
            "memory repository did not return its scripted status");
    requireError(
        repository.status(
            Domain::ProjectMemoryStatusRequest{fixture.otherProjectId},
            fixture.activeContext()),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "memory repository accepted a cross-project request");

    const auto authority = fixture.authority();
    const auto projectlessAuthorization = fixture.authorization(
        authority,
        "project_memory.export",
        Domain::ToolEffect::Write,
        std::nullopt);
    requireError(
        repository.exportMemory(
            Domain::ExportProjectMemoryRequest{fixture.projectId},
            authority,
            projectlessAuthorization,
            fixture.activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "memory repository accepted a projectless export capability");

    const auto wrongToolAuthorization = fixture.authorization(
        authority,
        "project_memory.import",
        Domain::ToolEffect::Write,
        fixture.projectId);
    requireError(
        repository.exportMemory(
            Domain::ExportProjectMemoryRequest{fixture.projectId},
            authority,
            wrongToolAuthorization,
            fixture.activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "memory repository accepted an import capability for export");

    const auto wrongEffectAuthorization = fixture.authorization(
        authority,
        "project_memory.export",
        Domain::ToolEffect::Read,
        fixture.projectId);
    requireError(
        repository.exportMemory(
            Domain::ExportProjectMemoryRequest{fixture.projectId},
            authority,
            wrongEffectAuthorization,
            fixture.activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "memory repository accepted a read capability for export");

    const auto readIntentAuthority = fixture.authorityFor(
        fixture.projectId,
        Domain::FileAccess::Read);
    const auto readIntentAuthorization =
        fixture.exportAuthorization(readIntentAuthority);
    requireError(
        repository.exportMemory(
            Domain::ExportProjectMemoryRequest{fixture.projectId},
            readIntentAuthority,
            readIntentAuthorization,
            fixture.activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "memory repository accepted read intent despite a write grant");

    const auto otherAuthority = fixture.authorityFor(
        fixture.otherProjectId,
        Domain::FileAccess::Write);
    const auto crossProjectAuthorization = fixture.authorization(
        otherAuthority,
        "project_memory.export",
        Domain::ToolEffect::Write,
        fixture.otherProjectId);
    requireError(
        repository.exportMemory(
            Domain::ExportProjectMemoryRequest{fixture.otherProjectId},
            otherAuthority,
            crossProjectAuthorization,
            fixture.activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "memory repository accepted a cross-project export capability");

    const auto authorization = fixture.exportAuthorization(authority);
    const auto exported = repository.exportMemory(
        Domain::ExportProjectMemoryRequest{fixture.projectId},
        authority,
        authorization,
        fixture.activeContext());
    require(exported && exported.value().projectId == fixture.projectId,
            "memory repository did not return its scripted export");
    require(repository.lastExportAuthority() &&
                repository.lastExportAuthority()->projectId() == fixture.projectId,
            "memory repository lost the typed export authority");
    require(repository.lastExportAuthorization() &&
                repository.lastExportAuthorization()->matchesProject(
                    fixture.projectId),
            "memory repository lost the authorized export capability");

    repository.close();
    requireError(
        repository.quickCheck(fixture.activeContext()),
        Domain::ErrorCodes::TransportClosed,
        "memory repository accepted work after close");
}

inline void testProjectMemoryRepositoryFactory(const Fixture& fixture)
{
    Fakes::ProjectMemoryRepositoryFactoryFake factory{2, 1, fixture.now};
    auto first = std::make_shared<Fakes::ProjectMemoryRepositoryFake>(
        fixture.projectId, fixture.now);
    auto second = std::make_shared<Fakes::ProjectMemoryRepositoryFake>(
        fixture.otherProjectId, fixture.now);
    require(factory.addRepository(first).hasValue(),
            "factory rejected its first repository");
    require(factory.addRepository(second).hasValue(),
            "factory rejected its second known repository");
    require(factory.open(fixture.projectId, fixture.activeContext()).hasValue(),
            "factory did not open its first repository");
    requireError(
        factory.open(fixture.otherProjectId, fixture.activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "factory exceeded its open-repository bound");
    require(factory.close(fixture.projectId, fixture.activeContext()).hasValue(),
            "factory did not close its first repository");
    require(factory.open(fixture.otherProjectId, fixture.activeContext()).hasValue(),
            "factory did not reuse capacity after close");
    factory.shutdown();
    require(factory.openCount() == 0,
            "factory retained open repositories after shutdown");
    requireError(
        factory.open(fixture.projectId, fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "factory accepted work after shutdown");
}

inline void testLegacyMemory(const Fixture& fixture)
{
    const Domain::DestructiveConfirmation expected{
        "purge_legacy_memory",
        "legacy-global-memory",
        "PURGE LEGACY GLOBAL MEMORY"};
    Fakes::LegacyMemoryServiceFake legacy{1, expected, fixture.now};
    const Domain::LegacyMemorySetRequest first{"note.one", "body", {}};
    const Domain::LegacyMemorySetRequest second{"note.two", "body", {}};
    require(legacy.set(first, fixture.activeContext()).hasValue(),
            "legacy fake rejected its first note");
    requireError(
        legacy.set(second, fixture.activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "legacy fake exceeded its note capacity");
    const auto clamped = legacy.list(
        Domain::LegacyMemoryListRequest{
            std::nullopt,
            std::nullopt,
            true,
            false,
            static_cast<std::int64_t>(
                Domain::LegacyMemoryLimits::MaximumQueryLimit + 1U)},
        fixture.activeContext());
    require(clamped && clamped.value().notes.size() == 1U,
            "legacy fake did not clamp the authoritative query limit");

    const std::string decomposedEAcute{"e\xCC\x81", 3U};
    const std::string composedEAcute{"\xC3\xA9", 2U};
    auto unicode = std::make_shared<Fakes::UnicodeCanonicalizerFake>(
        std::vector<Fakes::UnicodeCanonicalizerFake::Mapping>{
            {decomposedEAcute, composedEAcute},
            {composedEAcute, composedEAcute}});
    Fakes::LegacyMemoryServiceFake unicodeLegacy{
        1, expected, fixture.now, unicode};
    const auto unicodeSet = unicodeLegacy.set(
        Domain::LegacyMemorySetRequest{
            "unicode.note",
            "body",
            {decomposedEAcute, composedEAcute}},
        fixture.activeContext());
    require(
        unicodeSet && unicodeSet.value().note.tags ==
                          std::vector<std::string>{decomposedEAcute},
        "legacy fake did not preserve canonical Unicode tag semantics");
    const auto unicodeList = unicodeLegacy.list(
        Domain::LegacyMemoryListRequest{
            std::nullopt, composedEAcute, true, false, 50},
        fixture.activeContext());
    require(unicodeList && unicodeList.value().notes.size() == 1U,
            "legacy fake did not match a canonical-equivalent tag filter");

    const Domain::DestructiveConfirmation wrong{
        "purge_legacy_memory",
        "another-scope",
        "PURGE LEGACY GLOBAL MEMORY"};
    requireError(
        legacy.purge(wrong, fixture.activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "legacy fake accepted mismatched destructive confirmation");
    require(legacy.lastPurgeConfirmation() &&
                legacy.lastPurgeConfirmation()->scope == "another-scope",
            "legacy fake lost the typed destructive confirmation");
    const auto purged = legacy.purge(expected, fixture.activeContext());
    require(purged && purged.value().notesRemoved == 1U &&
                purged.value().verified && legacy.noteCount() == 0,
            "legacy fake did not purge its bounded notes");

    requireError(
        legacy.get(Domain::LegacyMemoryGetRequest{"note.one"},
                   fixture.expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded,
        "legacy fake ignored an expired context");
    legacy.shutdown();
    requireError(
        legacy.get(Domain::LegacyMemoryGetRequest{"note.one"},
                   fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "legacy fake accepted work after shutdown");
}

inline Domain::ContinuityOperation continuityOperation(const Fixture& fixture)
{
    return Domain::ContinuityOperation{
        parse<Domain::ContinuityOperationId>(
            "55555555-5555-4555-8555-555555555555"),
        fixture.projectId,
        parse<Domain::SessionId>("66666666-6666-4666-8666-666666666666"),
        std::nullopt,
        parse<Domain::ContinuityHandoffId>(
            "77777777-7777-4777-8777-777777777777"),
        Domain::ContinuityState::Idle,
        0,
        parse<Domain::AdapterId>("deterministic-adapter"),
        take(Domain::IdempotencyKey::create("continuity:operation")),
        std::nullopt,
        std::nullopt,
        Domain::UtcTimePoint{},
        Domain::UtcTimePoint{},
        std::nullopt,
        std::nullopt,
        fixture.digest(),
        std::nullopt};
}

inline Domain::ContinuityHandoff continuityHandoff(
    const Fixture& fixture,
    const Domain::ContinuityOperation& operation)
{
    return Domain::ContinuityHandoff{
        operation.handoffId,
        operation.operationId,
        Domain::UtcTimePoint{},
        Domain::ContinuityProject{
            fixture.projectId,
            "Repository fake",
            fixture.root,
            "main",
            "abc123",
            {}},
        Domain::ContinuitySession{
            operation.predecessorSessionId,
            std::nullopt,
            std::nullopt,
            std::nullopt},
        Domain::ContinuitySession{
            operation.successorSessionId.value(),
            std::nullopt,
            std::nullopt,
            std::nullopt},
        "Exercise deterministic acknowledgement persistence.",
        {},
        Domain::ContinuityCurrentWork{
            "P05",
            "continuity-repository",
            "Verify acknowledgement binding.",
            {}},
        {},
        {},
        {},
        Domain::ContinuityValidation{{}, {}, {}},
        {},
        {},
        {},
        Domain::ContinuityHostState{
            operation.adapterId,
            Domain::ContinuityState::CheckpointPreparing,
            "test",
            Domain::ContinuityRetryState{}},
        fixture.digest(),
        true};
}

inline void testContinuityRepository(const Fixture& fixture)
{
    Fakes::ContinuityRepositoryFake continuity{fixture.projectId, fixture.now};
    auto operation = continuityOperation(fixture);
    operation.successorSessionId = parse<Domain::SessionId>(
        "88888888-8888-4888-8888-888888888888");
    const auto operationId = operation.operationId;
    const auto handoff = continuityHandoff(fixture, operation);
    const auto created = continuity.createOperation(
        handoff, operation.idempotencyKey, fixture.activeContext());
    require(
        created && created.value().state == Domain::ContinuityState::Idle &&
            !created.value().successorSessionId,
        "continuity fake did not create its bounded idle operation");
    require(
        continuity.createOperation(
            handoff, operation.idempotencyKey, fixture.activeContext())
            .hasValue(),
        "continuity fake did not idempotently replay operation creation");
    auto crossProjectHandoff = handoff;
    crossProjectHandoff.project.projectId = fixture.otherProjectId;
    requireError(
        continuity.createOperation(
            crossProjectHandoff,
            operation.idempotencyKey,
            fixture.activeContext()),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "continuity fake accepted cross-project operation creation");
    const auto active = continuity.activeOperation(
        fixture.projectId, fixture.activeContext());
    require(active && active.value() &&
                active.value()->projectId == fixture.projectId,
            "continuity fake did not preserve project binding");
    requireError(
        continuity.activeOperation(
            fixture.otherProjectId, fixture.activeContext()),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "continuity fake accepted a cross-project lookup");
    const auto transitioned = continuity.compareAndSet(
        operationId,
        Domain::ContinuityState::Idle,
        Domain::ContinuityState::CheckpointPreparing,
        std::nullopt,
        std::string{"checkpoint-intent"},
        fixture.activeContext());
    require(transitioned &&
                transitioned.value().state ==
                    Domain::ContinuityState::CheckpointPreparing,
            "continuity fake did not enforce compare-and-set state");
    requireError(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::Idle,
            Domain::ContinuityState::CheckpointPersisted,
            std::nullopt,
            std::nullopt,
            fixture.activeContext()),
        Domain::ErrorCodes::Conflict,
        "continuity fake accepted a stale compare-and-set state");
    const auto retryAt = Domain::UtcTimePoint{} + 1s;
    const auto waiting = continuity.recordRetry(
        operationId,
        Domain::ContinuityState::CheckpointPreparing,
        "transient host failure",
        retryAt,
        fixture.activeContext());
    require(
        waiting && waiting.value().state == Domain::ContinuityState::RetryWait &&
            waiting.value().retryResumeState ==
                std::optional<Domain::ContinuityState>{
                    Domain::ContinuityState::CheckpointPreparing} &&
            waiting.value().retryAt == std::optional<Domain::UtcTimePoint>{retryAt} &&
            waiting.value().attempt == 3U,
        "continuity fake did not retain exact retry-resume state");
    const auto recoveringStatus = continuity.status(
        fixture.projectId, fixture.activeContext());
    require(
        recoveringStatus && recoveringStatus.value().recoveryRequired,
        "continuity fake did not expose retry recovery status");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::RetryWait,
            Domain::ContinuityState::CheckpointPreparing,
            std::nullopt,
            std::string{"retry-resume"},
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not resume the exact persisted retry state");
    require(
        continuity.storeHandoff(handoff, fixture.activeContext()).hasValue(),
        "continuity fake did not store the acknowledgement handoff");
    const auto lookedUp = continuity.operation(
        fixture.projectId, operationId, fixture.activeContext());
    require(
        lookedUp && lookedUp.value() &&
            lookedUp.value()->operationId == operationId,
        "continuity fake did not return its project-bound operation");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::CheckpointPreparing,
            Domain::ContinuityState::CheckpointPersisted,
            std::nullopt,
            std::nullopt,
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not persist its checkpoint transition");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::CheckpointPersisted,
            Domain::ContinuityState::SuccessorCreating,
            std::nullopt,
            std::nullopt,
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not persist successor-creation intent");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::SuccessorCreating,
            Domain::ContinuityState::SuccessorCreated,
            operation.successorSessionId,
            std::string{"successor-created"},
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not retain the successor binding");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::SuccessorCreated,
            Domain::ContinuityState::BootstrapSending,
            std::nullopt,
            std::string{"bootstrap-intent"},
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not persist bootstrap intent");
    const Domain::HandoffAcknowledgement acknowledgement{
        handoff.handoffId,
        operation.successorSessionId.value(),
        operation.adapterId,
        handoff.contentSha256};
    auto mismatchedAcknowledgement = acknowledgement;
    mismatchedAcknowledgement.canonicalHandoffSha256 =
        parse<Domain::Sha256Digest>(std::string(64, 'b'));
    requireError(
        continuity.acknowledge(
            operationId,
            mismatchedAcknowledgement,
            fixture.activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "continuity fake accepted a substituted acknowledgement digest");
    const auto acknowledged = continuity.acknowledge(
        operationId,
        acknowledgement,
        fixture.activeContext());
    require(
        acknowledged &&
            acknowledged.value().state == Domain::ContinuityState::Acknowledged &&
            acknowledged.value().acknowledgedSessionId ==
                operation.successorSessionId &&
            acknowledged.value().acknowledgedHandoffId == operation.handoffId,
        "continuity fake did not persist an exact acknowledgement");
    require(
        continuity.lastAcknowledgement() &&
            continuity.lastAcknowledgement()->canonicalHandoffSha256 ==
                handoff.contentSha256,
        "continuity fake did not retain exact acknowledgement evidence");
    const auto transitionCountBeforeDuplicate = continuity.transitionCount(
        operationId, fixture.activeContext());
    require(
        transitionCountBeforeDuplicate &&
            transitionCountBeforeDuplicate.value() == 10U,
        "continuity fake did not count each durable transition");
    require(
        continuity.acknowledge(
            operationId, acknowledgement, fixture.activeContext())
            .hasValue(),
        "continuity fake did not idempotently replay an exact acknowledgement");
    require(
        continuity.transitionCount(operationId, fixture.activeContext()).value() ==
            transitionCountBeforeDuplicate.value(),
        "continuity fake duplicated transition history during acknowledgement replay");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::Acknowledged,
            Domain::ContinuityState::PredecessorSealing,
            std::nullopt,
            std::string{"seal-intent"},
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not persist predecessor-sealing intent");
    require(
        continuity.compareAndSet(
            operationId,
            Domain::ContinuityState::PredecessorSealing,
            Domain::ContinuityState::Completed,
            std::nullopt,
            std::string{"completed"},
            fixture.activeContext())
            .hasValue(),
        "continuity fake did not complete the operation");
    const auto activeSession = continuity.activeSession(
        fixture.projectId, fixture.activeContext());
    require(
        activeSession && activeSession.value() == operation.successorSessionId,
        "continuity fake did not publish the completed successor session");
    const auto terminalActive = continuity.activeOperation(
        fixture.projectId, fixture.activeContext());
    require(
        terminalActive && !terminalActive.value(),
        "continuity fake exposed a terminal operation as active");
    requireError(
        continuity.resetContinuity(
            Domain::ContinuityResetRequest{
                fixture.otherProjectId,
                Domain::DestructiveConfirmation{
                    "reset_project_continuity",
                    fixture.otherProjectId.value(),
                    "RESET PROJECT CONTINUITY " +
                        fixture.otherProjectId.value()}},
            fixture.activeContext()),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "continuity fake accepted a cross-project reset");
    const auto reset = continuity.resetContinuity(
        Domain::ContinuityResetRequest{
            fixture.projectId,
            Domain::DestructiveConfirmation{
                "reset_project_continuity",
                fixture.projectId.value(),
                "RESET PROJECT CONTINUITY " + fixture.projectId.value()}},
        fixture.activeContext());
    require(
        reset && reset.value().report.recordsRemoved == 1U &&
            reset.value().report.linksRemoved == 1U &&
            reset.value().report.eventsRemoved == 13U &&
            reset.value().report.verified,
        "continuity fake did not reset only its bounded continuity records");
    const auto emptyStatus = continuity.status(
        fixture.projectId, fixture.activeContext());
    require(
        emptyStatus && emptyStatus.value().operationCount == 0U &&
            emptyStatus.value().handoffCount == 0U,
        "continuity fake retained records after scoped reset");
    requireError(
        continuity.activeOperation(fixture.projectId, fixture.expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded,
        "continuity fake ignored an expired context");
    continuity.close();
    requireError(
        continuity.activeOperation(fixture.projectId, fixture.activeContext()),
        Domain::ErrorCodes::TransportClosed,
        "continuity fake accepted work after close");

    Fakes::ProjectMemoryRepositoryFake aggregate{fixture.projectId, fixture.now};
    ProductContracts::IProjectRepository& repository = aggregate;
    require(
        repository.createOperation(
            handoff, operation.idempotencyKey, fixture.activeContext())
            .hasValue(),
        "project repository aggregate did not delegate continuity creation");
    require(
        repository.storeHandoff(handoff, fixture.activeContext()).hasValue() &&
            aggregate.continuity().storedHandoff().has_value(),
        "project repository aggregate did not share its continuity owner");
    repository.close();
    require(aggregate.closeCount() == 1U,
            "project repository aggregate did not close exactly once");
    requireError(
        repository.activeOperation(fixture.projectId, fixture.activeContext()),
        Domain::ErrorCodes::TransportClosed,
        "project repository aggregate left continuity open after close");
}

inline void testDiagnostics(const Fixture& fixture)
{
    Fakes::DiagnosticSinkFake sink{1, fixture.now};
    const Domain::DiagnosticEnvelope event{
        Domain::UtcTimePoint{},
        "fake.event",
        Domain::DiagnosticSeverity::Info,
        "test",
        1,
        Domain::DiagnosticCategory::Diagnostics,
        {}};
    require(sink.record(event, fixture.activeContext()).hasValue(),
            "diagnostic sink rejected its first event");
    requireError(
        sink.record(event, fixture.activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "diagnostic sink exceeded its event capacity");
    const auto recent = sink.recent(1, fixture.activeContext());
    require(recent && recent.value().size() == 1,
            "diagnostic sink did not return bounded recent events");

    sink.exportResult.set(
        Domain::Result<Domain::DiagnosticExportResult>::success(
            Domain::DiagnosticExportResult{
                fixture.artifact, 1, 100, fixture.digest()}));
    const Domain::DiagnosticExportRequest exportRequest{
        fixture.root, std::string{"diagnostics"}};
    const auto authority = fixture.authority();
    require(sink.exportData(
                exportRequest, authority, fixture.activeContext()).hasValue(),
            "diagnostic sink did not return its scripted export");
    require(sink.lastExportRequest() &&
                sink.lastExportRequest()->basename == exportRequest.basename,
            "diagnostic sink lost its typed export request");
    require(sink.lastExportAuthority() &&
                sink.lastExportAuthority()->projectId() == fixture.projectId,
            "diagnostic sink lost its export authority");
    sink.shutdown();
    requireError(
        sink.recent(1, fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "diagnostic sink accepted work after shutdown");

    Fakes::AuditRepositoryFake audit{1, fixture.now};
    const Domain::AuditEvent auditEvent{
        Domain::UtcTimePoint{}, std::nullopt, "tool", std::nullopt, "ok",
        std::nullopt, std::nullopt};
    require(audit.append(auditEvent, fixture.activeContext()).hasValue(),
            "audit repository rejected its first event");
    requireError(
        audit.append(auditEvent, fixture.activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "audit repository exceeded its event capacity");
    audit.close();
    requireError(
        audit.recent(1, fixture.activeContext()),
        Domain::ErrorCodes::TransportClosed,
        "audit repository accepted work after close");

    Fakes::DoctorServiceFake doctor{fixture.now};
    doctor.runResult.set(Domain::Result<Domain::DoctorReport>::success(
        Domain::DoctorReport{
            true,
            "1.0",
            fixture.root,
            {},
            Domain::TelemetryHealthReport{
                true, "telemetry", "native", false, "event", "native", "native", false},
            true,
            fixture.root}));
    require(doctor.run(fixture.activeContext()).hasValue(),
            "doctor fake did not return its scripted report");
    doctor.shutdown();
    requireError(
        doctor.run(fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "doctor fake accepted work after shutdown");

    Fakes::RuntimeDiagnosticsFake runtime{fixture.now};
    runtime.snapshotResult.set(
        Domain::Result<Domain::RuntimeDiagnosticSnapshot>::success(
            Domain::RuntimeDiagnosticSnapshot{Domain::UtcTimePoint{}, 1, 0, 0, 1, 0}));
    require(runtime.snapshot(fixture.activeContext()).hasValue(),
            "runtime diagnostics fake did not return its scripted snapshot");

    {
        auto operationLease = take(runtime.acquire(
            Contracts::RuntimeOwnerKind::OwnedOperation,
            fixture.activeContext()));
        require(operationLease.active(),
                "runtime ownership lease was not active");
        require(
            runtime.activeOwnership(Contracts::RuntimeOwnerKind::OwnedOperation) == 1U,
            "runtime ownership acquisition did not increment its fixed counter");
    }
    require(
        runtime.activeOwnership(Contracts::RuntimeOwnerKind::OwnedOperation) == 0U,
        "runtime ownership lease destruction did not decrement its fixed counter");

    auto telemetryLease = take(runtime.acquire(
        Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot,
        fixture.activeContext()));
    requireError(
        runtime.acquire(
            Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot,
            fixture.activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "runtime diagnostics fake exceeded capacity-one telemetry ownership");
    telemetryLease.reset();
    require(
        runtime.activeOwnership(
            Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot) == 0U,
        "runtime telemetry ownership did not return to zero");

    requireError(
        runtime.snapshot(fixture.expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded,
        "runtime diagnostics fake ignored an expired context");
    runtime.shutdown();
    requireError(
        runtime.snapshot(fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "runtime diagnostics fake accepted work after shutdown");
    requireError(
        runtime.acquire(
            Contracts::RuntimeOwnerKind::OwnedOperation,
            fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "runtime diagnostics fake acquired ownership after shutdown");
}

inline void testManager(const Fixture& fixture)
{
    Fakes::ManagerClientFake client{fixture.now};
    const Domain::ManagerStatus status{.home = fixture.root};
    const Domain::ManagerSettings settings;
    client.statusResult.set(
        Domain::Result<Domain::ManagerStatus>::success(status));
    client.settingsResult.set(
        Domain::Result<Domain::ManagerSettings>::success(settings));
    client.controlResult.set(
        Domain::Result<Domain::ManagerStatus>::success(status));
    const Domain::ManagerSettingsUpdateOutcome settingsUpdate{
        settings,
        true,
        true,
        status};
    client.updateSettingsResult.set(
        Domain::Result<Domain::ManagerSettingsUpdateOutcome>::success(
            settingsUpdate));
    require(client.status(fixture.activeContext()).hasValue(),
            "manager client did not return its scripted status");
    const Domain::ManagerControlRequest controlRequest{
        Domain::ManagerControlAction::Restart};
    require(
        client.control(controlRequest, fixture.activeContext()).hasValue(),
        "manager client did not return its scripted control status");
    require(
        client.lastControlRequest() &&
            client.lastControlRequest()->action == controlRequest.action,
        "manager client lost its typed control request");
    requireError(
        client.control(controlRequest, fixture.expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded,
        "manager client did not forward the control deadline");
    const Domain::ManagerSettingsPatch patch{
        std::nullopt,
        Domain::DefaultManagerDashboardPort,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt};
    const auto updated = client.updateSettings(
        patch, true, fixture.activeContext());
    require(updated.hasValue() &&
                updated.value().settings.dashboardPort ==
                    settings.dashboardPort &&
                updated.value().applied &&
                updated.value().bindingChanged &&
                updated.value().status.home == status.home,
            "manager client did not return its complete scripted update outcome");
    require(client.lastSettingsPatch() &&
                client.lastSettingsPatch()->dashboardPort == patch.dashboardPort &&
                client.lastApplyImmediately().value_or(false),
            "manager client lost its typed settings update");
    requireError(
        client.status(fixture.cancelledContext()),
        Domain::ErrorCodes::Cancelled,
        "manager client ignored operation cancellation");
    client.shutdown();
    requireError(
        client.settings(fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "manager client accepted work after shutdown");

    Fakes::ManagerServerFake server{fixture.now};
    server.runResult.set(Domain::Result<void>::success());
    require(server.run(fixture.activeContext()).hasValue(),
            "manager server did not return its scripted run result");
    server.cancel(fixture.otherOperationId);
    require(server.run(fixture.activeContext()).hasValue(),
            "manager server cancelled the wrong operation");
    server.cancel(fixture.operationId);
    requireError(
        server.run(fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "manager server did not cancel the targeted operation");
    require(server.cancelledOperation() &&
                server.cancelledOperation().value() == fixture.operationId,
            "manager server lost the targeted cancellation identity");
    server.shutdown();
    requireError(
        server.run(fixture.activeContext()),
        Domain::ErrorCodes::Cancelled,
        "manager server accepted work after shutdown");
}

} // namespace RepositoryDiagnosticsManagerFakeContractTestsDetail

inline void runRepositoryDiagnosticsManagerFakeContractTests()
{
    const RepositoryDiagnosticsManagerFakeContractTestsDetail::Fixture fixture;
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testProjectRegistry(fixture);
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testProjectMemoryRepository(fixture);
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testProjectMemoryRepositoryFactory(
        fixture);
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testLegacyMemory(fixture);
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testContinuityRepository(fixture);
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testDiagnostics(fixture);
    RepositoryDiagnosticsManagerFakeContractTestsDetail::testManager(fixture);
}

} // namespace ForgeConductor::Tests
