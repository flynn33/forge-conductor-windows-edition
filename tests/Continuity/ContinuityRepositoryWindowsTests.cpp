#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error{                                             \
                std::string{"Requirement failed: "} + #condition};              \
        }                                                                         \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
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
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view correlation)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        std::chrono::steady_clock::now() + 2min,
        {},
        parse<Domain::CorrelationId>(correlation)};
}

constexpr std::string_view ProjectV1Digest =
    "fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f";

constexpr std::string_view ProjectV1Handoff = R"json({"completed_work":[{"summary":"Legacy checkpoint persisted"}],"constraints":["Preserve durable state"],"created_at":"2025-01-02T03:04:15Z","current_work":{"active_files":["README.md"],"phase_id":"P07","summary":"Migrate legacy persistence","work_item_id":"legacy-fixture"},"decisions":[{"decision":"Retain legacy rows"}],"evidence_references":[{"path":"legacy-evidence"}],"handoff_id":"66666666-6666-4666-8666-666666666666","host_state":{"adapter_id":"legacy-adapter","context_budget_source":"legacy-fixture","continuity_state":"checkpointPersisted","retry":{"attempt":2}},"integrity":{"content_sha256":"fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f","redaction_complete":true},"memory_references":[{"record_id":"22222222-2222-4222-8222-222222222222"}],"mission":"Preserve legacy project state","next_actions":[{"action":"Continue migration","command":"","order":1,"success_condition":"Legacy rows remain readable"}],"open_work":[{"summary":"Apply Windows migration"}],"operation_id":"55555555-5555-4555-8555-555555555555","predecessor_session":{"model":"legacy-model","provider_session_id":null,"session_id":"77777777-7777-4777-8777-777777777777"},"project":{"branch":"legacy","commit":"0123456789abcdef","dirty_summary":[],"display_name":"Legacy Project","project_id":"11111111-1111-4111-8111-111111111111","repository_root":"legacy-root"},"schema_version":"1.0","successor_session":null,"validation":{"commands":[],"open_gates":["G07"],"passed_gates":["G06"]}})json";

[[nodiscard]] std::vector<Domain::Uuid> uuidSequence()
{
    std::vector<Domain::Uuid> values;
    values.reserve(32U);
    for (std::uint32_t value = 1U; value <= 32U; ++value) {
        std::string text = "10000000-0000-4000-8000-000000000000";
        const auto decimal = std::to_string(value);
        text.replace(text.size() - decimal.size(), decimal.size(), decimal);
        values.push_back(parse<Domain::Uuid>(text));
    }
    return values;
}

struct RepositoryFixture final {
    RepositoryFixture(
        const std::filesystem::path& directory,
        Domain::ProjectId configuredProjectId)
        : projectId{std::move(configuredProjectId)}
    {
        paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
        const auto monotonic = std::chrono::steady_clock::now();
        paths->setNow(monotonic);
        paths->projectRootResult.set(
            Domain::Result<Domain::PathText>::success(
                Support::pathText(directory)));
        diagnostics = std::make_shared<Fakes::RuntimeDiagnosticsFake>(monotonic);
        const auto day = std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::January / 2};
        clock = std::make_shared<Support::FixedClock>(
            Domain::UtcTimePoint{
                day.time_since_epoch() + std::chrono::hours{3}},
            monotonic);
        redactor = std::make_shared<InfrastructureWindows::SecretRedactor>();
        hasher = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
        codec = std::make_shared<
            InfrastructureWindows::WindowsContinuityDocumentCodec>(
                hasher, clock);
        uuidGenerator = std::make_shared<Fakes::SequenceUuidGenerator>(
            uuidSequence());
        PersistenceWindows::WindowsProjectMemoryRepositoryOptions options{};
        repository = take(PersistenceWindows::WindowsProjectMemoryRepository::open(
            projectId,
            paths,
            diagnostics,
            redactor,
            hasher,
            uuidGenerator,
            clock,
            options,
            context("continuity-repository-open")));
    }

    Domain::ProjectId projectId;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Contracts::IRedactor> redactor;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher;
    std::shared_ptr<InfrastructureWindows::WindowsContinuityDocumentCodec> codec;
    std::shared_ptr<Fakes::SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryRepository> repository;
};

[[nodiscard]] Domain::ContinuityHandoff projectV1(
    RepositoryFixture& fixture)
{
    return take(fixture.codec->decode(
        ProjectV1Handoff,
        context("continuity-project-v1-decode"))).handoff;
}

[[nodiscard]] Domain::ContinuityHandoff handoffFor(
    RepositoryFixture& fixture,
    const std::string_view operationId,
    const std::string_view handoffId,
    const std::string_view predecessorSessionId,
    const std::string_view adapterId = "windows-test-adapter")
{
    auto handoff = projectV1(fixture);
    handoff.project.projectId = fixture.projectId;
    handoff.operationId = parse<Domain::ContinuityOperationId>(operationId);
    handoff.handoffId = parse<Domain::ContinuityHandoffId>(handoffId);
    handoff.predecessorSession.sessionId =
        parse<Domain::SessionId>(predecessorSessionId);
    handoff.predecessorSession.providerSessionId.reset();
    handoff.successorSession.reset();
    handoff.hostState.adapterId = parse<Domain::AdapterId>(adapterId);
    handoff.hostState.continuityState = Domain::ContinuityState::Idle;
    handoff.hostState.persistedContinuityStateName.reset();
    handoff.hostState.retry = {};
    handoff.mission = "Exercise Windows continuity repository " +
        handoff.operationId.value();
    return take(fixture.codec->encode(
        handoff,
        context("continuity-handoff-finalize"))).handoff;
}

void executeDatabaseSql(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    const auto encodedPath = Support::pathText(databasePath).value();
    sqlite3* database{};
    const int openResult = sqlite3_open_v2(
        encodedPath.c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (openResult != SQLITE_OK) {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
        throw std::runtime_error{
            "Could not open the continuity database for direct inspection."};
    }

    const int timeoutResult = sqlite3_busy_timeout(database, 5'000);
    char* errorMessage{};
    const int executeResult = timeoutResult == SQLITE_OK
        ? sqlite3_exec(
              database,
              std::string{sql}.c_str(),
              nullptr,
              nullptr,
              &errorMessage)
        : timeoutResult;
    std::string errorText;
    if (errorMessage != nullptr) {
        errorText = errorMessage;
        sqlite3_free(errorMessage);
    }
    const int closeResult = sqlite3_close_v2(database);
    if (executeResult != SQLITE_OK) {
        throw std::runtime_error{
            "Could not execute continuity inspection SQL: " + errorText};
    }
    REQUIRE(closeResult == SQLITE_OK);
}

[[nodiscard]] std::int64_t queryDatabaseInteger(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    const auto encodedPath = Support::pathText(databasePath).value();
    sqlite3* database{};
    REQUIRE(sqlite3_open_v2(
                encodedPath.c_str(),
                &database,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                    SQLITE_OPEN_NOFOLLOW,
                nullptr) == SQLITE_OK);
    sqlite3_stmt* statement{};
    const std::string ownedSql{sql};
    const int prepareResult = sqlite3_prepare_v2(
        database, ownedSql.c_str(), -1, &statement, nullptr);
    const int stepResult = prepareResult == SQLITE_OK
        ? sqlite3_step(statement)
        : prepareResult;
    const auto value = stepResult == SQLITE_ROW
        ? sqlite3_column_int64(statement, 0)
        : std::int64_t{-1};
    if (statement != nullptr) {
        static_cast<void>(sqlite3_finalize(statement));
    }
    const int closeResult = sqlite3_close_v2(database);
    REQUIRE(stepResult == SQLITE_ROW);
    REQUIRE(closeResult == SQLITE_OK);
    return value;
}

[[nodiscard]] std::string queryDatabaseText(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    const auto encodedPath = Support::pathText(databasePath).value();
    sqlite3* database{};
    REQUIRE(sqlite3_open_v2(
                encodedPath.c_str(),
                &database,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                    SQLITE_OPEN_NOFOLLOW,
                nullptr) == SQLITE_OK);
    sqlite3_stmt* statement{};
    const std::string ownedSql{sql};
    const int prepareResult = sqlite3_prepare_v2(
        database, ownedSql.c_str(), -1, &statement, nullptr);
    const int stepResult = prepareResult == SQLITE_OK
        ? sqlite3_step(statement)
        : prepareResult;
    std::string value;
    if (stepResult == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        const auto bytes = sqlite3_column_bytes(statement, 0);
        REQUIRE(text != nullptr);
        REQUIRE(bytes >= 0);
        value.assign(
            reinterpret_cast<const char*>(text),
            static_cast<std::size_t>(bytes));
    }
    if (statement != nullptr) {
        static_cast<void>(sqlite3_finalize(statement));
    }
    const int closeResult = sqlite3_close_v2(database);
    REQUIRE(stepResult == SQLITE_ROW);
    REQUIRE(closeResult == SQLITE_OK);
    return value;
}

[[nodiscard]] Domain::ProjectMemoryWrite memoryWrite(
    std::string title,
    std::string summary,
    std::vector<std::string> tags = {})
{
    Domain::ProjectMemoryWrite write{};
    write.kind = "decision";
    write.title = std::move(title);
    write.summary = std::move(summary);
    write.tags = std::move(tags);
    return write;
}

[[nodiscard]] Domain::ContinuityOperation transition(
    PersistenceWindows::WindowsProjectMemoryRepository& repository,
    const Domain::ContinuityOperationId& operationId,
    const Domain::ContinuityState expected,
    const Domain::ContinuityState next,
    std::optional<Domain::SessionId> successor = std::nullopt,
    const std::string_view correlation = "continuity-transition")
{
    auto operation = take(repository.compareAndSet(
        operationId,
        expected,
        next,
        std::move(successor),
        std::optional<std::string>{std::string{Domain::wireName(next)}},
        context(correlation)));
    REQUIRE(operation.state == next);
    return operation;
}

void createReplayBindingAndProjectV1RoundTrip()
{
    Support::ScopedTestDirectory directory{L"continuity-repository-project-v1"};
    const auto projectId = parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
    RepositoryFixture fixture{directory.path(), projectId};
    const auto handoff = projectV1(fixture);
    const auto idempotency = take(Domain::IdempotencyKey::create(
        "project-v1-operation"));

    const auto created = take(fixture.repository->createOperation(
        handoff, idempotency, context("continuity-create-project-v1")));
    REQUIRE(created.state == Domain::ContinuityState::Idle);
    REQUIRE(created.operationId == handoff.operationId);
    REQUIRE(created.handoffId == handoff.handoffId);
    REQUIRE(created.predecessorSessionId ==
            handoff.predecessorSession.sessionId);
    REQUIRE(created.adapterId == handoff.hostState.adapterId);

    const auto replayed = take(fixture.repository->createOperation(
        handoff, idempotency, context("continuity-create-project-v1-replay")));
    REQUIRE(replayed.operationId == created.operationId);
    REQUIRE(replayed.stateChecksum == created.stateChecksum);
    REQUIRE(replayed.attempt == 0U);

    auto wrongOperation = handoff;
    wrongOperation.operationId = parse<Domain::ContinuityOperationId>(
        "55555555-5555-4555-8555-555555555556");
    requireError(
        fixture.repository->createOperation(
            wrongOperation,
            idempotency,
            context("continuity-create-binding-operation")),
        Domain::ErrorCodes::Conflict);

    auto wrongHandoff = handoff;
    wrongHandoff.handoffId = parse<Domain::ContinuityHandoffId>(
        "66666666-6666-4666-8666-666666666667");
    requireError(
        fixture.repository->createOperation(
            wrongHandoff,
            idempotency,
            context("continuity-create-binding-handoff")),
        Domain::ErrorCodes::Conflict);

    auto wrongPredecessor = handoff;
    wrongPredecessor.predecessorSession.sessionId = parse<Domain::SessionId>(
        "77777777-7777-4777-8777-777777777778");
    requireError(
        fixture.repository->createOperation(
            wrongPredecessor,
            idempotency,
            context("continuity-create-binding-predecessor")),
        Domain::ErrorCodes::Conflict);

    auto wrongAdapter = handoff;
    wrongAdapter.hostState.adapterId = parse<Domain::AdapterId>(
        "other-adapter");
    requireError(
        fixture.repository->createOperation(
            wrongAdapter,
            idempotency,
            context("continuity-create-binding-adapter")),
        Domain::ErrorCodes::Conflict);

    take(fixture.repository->storeHandoff(
        handoff, context("continuity-store-project-v1")));
    take(fixture.repository->storeHandoff(
        handoff, context("continuity-store-project-v1-replay")));

    auto changedPayload = handoff;
    changedPayload.mission = "A replay may not replace durable handoff bytes";
    requireError(
        fixture.repository->storeHandoff(
            changedPayload,
            context("continuity-store-project-v1-changed-replay")),
        Domain::ErrorCodes::Conflict);

    const auto persisted = take(fixture.repository->handoff(
        projectId,
        handoff.handoffId,
        context("continuity-read-project-v1")));
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->contentSha256.value() == ProjectV1Digest);
    const auto encoded = take(fixture.codec->encode(
        *persisted, context("continuity-reencode-project-v1")));
    REQUIRE(encoded.canonicalUtf8 == ProjectV1Handoff);
    REQUIRE(queryDatabaseText(
                directory.path() / L"memory.sqlite",
                "SELECT payload_json FROM continuity_handoffs LIMIT 1") ==
            ProjectV1Handoff);
    REQUIRE(take(fixture.repository->transitionCount(
                handoff.operationId,
                context("continuity-project-v1-transition-count"))) == 1U);
}

void fullCasAcknowledgementAndTerminalSlotRelease()
{
    Support::ScopedTestDirectory directory{L"continuity-repository-full-cas"};
    const auto projectId = parse<Domain::ProjectId>(
        "12121212-1212-4212-8212-121212121212");
    RepositoryFixture fixture{directory.path(), projectId};
    const auto handoff = handoffFor(
        fixture,
        "51515151-5151-4515-8515-515151515151",
        "61616161-6161-4616-8616-616161616161",
        "71717171-7171-4717-8717-717171717171");
    const auto idempotency = take(Domain::IdempotencyKey::create(
        "full-cas-operation"));
    const auto operation = take(fixture.repository->createOperation(
        handoff, idempotency, context("continuity-full-create")));
    const auto atomicallyStoredHandoff = take(fixture.repository->handoff(
        projectId,
        handoff.handoffId,
        context("continuity-full-atomic-handoff")));
    REQUIRE(atomicallyStoredHandoff.has_value());
    REQUIRE(atomicallyStoredHandoff->contentSha256 == handoff.contentSha256);

    requireError(
        fixture.repository->compareAndSet(
            operation.operationId,
            Domain::ContinuityState::CheckpointPreparing,
            Domain::ContinuityState::CheckpointPersisted,
            std::nullopt,
            std::nullopt,
            context("continuity-wrong-expected")),
        Domain::ErrorCodes::Conflict);
    requireError(
        fixture.repository->compareAndSet(
            operation.operationId,
            Domain::ContinuityState::Idle,
            Domain::ContinuityState::SuccessorCreating,
            std::nullopt,
            std::nullopt,
            context("continuity-invalid-transition")),
        Domain::ErrorCodes::InvalidRequest);

    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::Idle,
        Domain::ContinuityState::CheckpointPreparing));
    take(fixture.repository->storeHandoff(
        handoff, context("continuity-full-store-replay")));
    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::CheckpointPreparing,
        Domain::ContinuityState::CheckpointPersisted));
    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::CheckpointPersisted,
        Domain::ContinuityState::SuccessorCreating));

    requireError(
        fixture.repository->compareAndSet(
            operation.operationId,
            Domain::ContinuityState::SuccessorCreating,
            Domain::ContinuityState::SuccessorCreated,
            std::nullopt,
            std::nullopt,
            context("continuity-missing-successor")),
        Domain::ErrorCodes::InvalidRequest);
    const auto successor = parse<Domain::SessionId>(
        "81818181-8181-4818-8818-818181818181");
    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::SuccessorCreating,
        Domain::ContinuityState::SuccessorCreated,
        successor));

    const auto wrongSuccessor = parse<Domain::SessionId>(
        "82828282-8282-4828-8828-828282828282");
    requireError(
        fixture.repository->compareAndSet(
            operation.operationId,
            Domain::ContinuityState::SuccessorCreating,
            Domain::ContinuityState::SuccessorCreated,
            wrongSuccessor,
            std::nullopt,
            context("continuity-successor-binding-conflict")),
        Domain::ErrorCodes::Conflict);

    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::SuccessorCreated,
        Domain::ContinuityState::BootstrapSending));
    const auto durableHandoff = take(fixture.repository->handoff(
        projectId,
        handoff.handoffId,
        context("continuity-read-ack-handoff")));
    REQUIRE(durableHandoff.has_value());
    REQUIRE(durableHandoff->successorSession.has_value());
    REQUIRE(durableHandoff->successorSession->sessionId == successor);

    const auto acknowledgement = Domain::HandoffAcknowledgement{
        handoff.handoffId,
        successor,
        handoff.hostState.adapterId,
        durableHandoff->contentSha256};
    auto wrongHandoffAck = acknowledgement;
    wrongHandoffAck.handoffId = parse<Domain::ContinuityHandoffId>(
        "62626262-6262-4626-8626-626262626262");
    requireError(
        fixture.repository->acknowledge(
            operation.operationId,
            wrongHandoffAck,
            context("continuity-ack-wrong-handoff")),
        Domain::ErrorCodes::IntegrityFailure);
    auto wrongSessionAck = acknowledgement;
    wrongSessionAck.successorSessionId = wrongSuccessor;
    requireError(
        fixture.repository->acknowledge(
            operation.operationId,
            wrongSessionAck,
            context("continuity-ack-wrong-session")),
        Domain::ErrorCodes::IntegrityFailure);
    auto wrongAdapterAck = acknowledgement;
    wrongAdapterAck.adapterId = parse<Domain::AdapterId>("other-adapter");
    requireError(
        fixture.repository->acknowledge(
            operation.operationId,
            wrongAdapterAck,
            context("continuity-ack-wrong-adapter")),
        Domain::ErrorCodes::IntegrityFailure);
    auto wrongHashAck = acknowledgement;
    wrongHashAck.canonicalHandoffSha256 = parse<Domain::Sha256Digest>(
        "0000000000000000000000000000000000000000000000000000000000000000");
    requireError(
        fixture.repository->acknowledge(
            operation.operationId,
            wrongHashAck,
            context("continuity-ack-wrong-hash")),
        Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(!take(fixture.repository->activeSession(
        projectId, context("continuity-no-pointer-before-complete"))));

    const auto acknowledged = take(fixture.repository->acknowledge(
        operation.operationId,
        acknowledgement,
        context("continuity-ack-exact")));
    REQUIRE(acknowledged.state == Domain::ContinuityState::Acknowledged);
    const auto acknowledgementReplay = take(fixture.repository->acknowledge(
        operation.operationId,
        acknowledgement,
        context("continuity-ack-exact-replay")));
    REQUIRE(acknowledgementReplay.stateChecksum ==
            acknowledged.stateChecksum);

    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::Acknowledged,
        Domain::ContinuityState::PredecessorSealing));
    const auto completed = transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::PredecessorSealing,
        Domain::ContinuityState::Completed);
    REQUIRE(completed.successorSessionId ==
            std::optional<Domain::SessionId>{successor});
    REQUIRE(take(fixture.repository->activeSession(
                projectId,
                context("continuity-pointer-after-complete"))) ==
            std::optional<Domain::SessionId>{successor});
    REQUIRE(!take(fixture.repository->activeOperation(
        projectId, context("continuity-no-active-after-complete"))));
    REQUIRE(take(fixture.repository->transitionCount(
                operation.operationId,
                context("continuity-full-transition-count"))) == 9U);

    const auto chain = queryDatabaseText(
        directory.path() / L"memory.sqlite",
        "SELECT group_concat(edge, ',') FROM ("
        "SELECT COALESCE(from_state,'<null>') || '>' || to_state || ':' || "
        "attempt AS edge FROM rollover_transitions "
        "WHERE operation_id='51515151-5151-4515-8515-515151515151' "
        "ORDER BY id)");
    REQUIRE(chain ==
            "<null>>idle:0,idle>checkpoint_preparing:1,"
            "checkpoint_preparing>checkpoint_persisted:2,"
            "checkpoint_persisted>successor_creating:3,"
            "successor_creating>successor_created:4,"
            "successor_created>bootstrap_sending:5,"
            "bootstrap_sending>acknowledged:6,"
            "acknowledged>predecessor_sealing:7,"
            "predecessor_sealing>completed:8");

    const auto cancelledHandoff = handoffFor(
        fixture,
        "52525252-5252-4525-8525-525252525252",
        "63636363-6363-4636-8636-636363636363",
        "73737373-7373-4737-8737-737373737373");
    const auto cancelledOperation = take(fixture.repository->createOperation(
        cancelledHandoff,
        take(Domain::IdempotencyKey::create("cancelled-slot-release")),
        context("continuity-create-after-completed")));
    static_cast<void>(transition(
        *fixture.repository,
        cancelledOperation.operationId,
        Domain::ContinuityState::Idle,
        Domain::ContinuityState::Cancelling));
    static_cast<void>(transition(
        *fixture.repository,
        cancelledOperation.operationId,
        Domain::ContinuityState::Cancelling,
        Domain::ContinuityState::Cancelled));
    REQUIRE(!take(fixture.repository->activeOperation(
        projectId, context("continuity-no-active-after-cancelled"))));

    const auto thirdHandoff = handoffFor(
        fixture,
        "53535353-5353-4535-8535-535353535353",
        "64646464-6464-4646-8646-646464646464",
        "74747474-7474-4747-8747-747474747474");
    const auto third = take(fixture.repository->createOperation(
        thirdHandoff,
        take(Domain::IdempotencyKey::create("third-active-operation")),
        context("continuity-create-after-cancelled")));
    REQUIRE(third.state == Domain::ContinuityState::Idle);
    const auto status = take(fixture.repository->status(
        projectId, context("continuity-status-three-operations")));
    REQUIRE(status.operationCount == 3U);
    REQUIRE(status.activeOperation.has_value());
    REQUIRE(status.activeOperation->operationId == third.operationId);
}

void retryResumeStatePersistsAndBindsExactResume()
{
    Support::ScopedTestDirectory directory{L"continuity-repository-retry"};
    const auto projectId = parse<Domain::ProjectId>(
        "13131313-1313-4313-8313-131313131313");
    const auto operationId = parse<Domain::ContinuityOperationId>(
        "54545454-5454-4545-8545-545454545454");
    const auto retryAt = Domain::UtcTimePoint{
        std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::January / 2}
                .time_since_epoch() +
        std::chrono::hours{3} + 30s};

    {
        RepositoryFixture fixture{directory.path(), projectId};
        const auto handoff = handoffFor(
            fixture,
            operationId.value(),
            "65656565-6565-4656-8656-656565656565",
            "75757575-7575-4757-8757-757575757575");
        static_cast<void>(take(fixture.repository->createOperation(
            handoff,
            take(Domain::IdempotencyKey::create("retry-resume-operation")),
            context("continuity-retry-create"))));
        static_cast<void>(transition(
            *fixture.repository,
            operationId,
            Domain::ContinuityState::Idle,
            Domain::ContinuityState::CheckpointPreparing));
        const auto retry = take(fixture.repository->recordRetry(
            operationId,
            Domain::ContinuityState::CheckpointPreparing,
            "checkpoint write interrupted",
            retryAt,
            context("continuity-record-retry")));
        REQUIRE(retry.state == Domain::ContinuityState::RetryWait);
        REQUIRE(retry.retryResumeState ==
                std::optional<Domain::ContinuityState>{
                    Domain::ContinuityState::CheckpointPreparing});
        REQUIRE(retry.retryAt == std::optional<Domain::UtcTimePoint>{retryAt});
        REQUIRE(retry.lastError ==
                std::optional<std::string>{"checkpoint write interrupted"});
        REQUIRE(retry.attempt == 3U);
        fixture.repository->close();
    }

    RepositoryFixture reopened{directory.path(), projectId};
    const auto durable = take(reopened.repository->operation(
        projectId,
        operationId,
        context("continuity-retry-read-after-reopen")));
    REQUIRE(durable.has_value());
    REQUIRE(durable->state == Domain::ContinuityState::RetryWait);
    REQUIRE(durable->retryResumeState ==
            std::optional<Domain::ContinuityState>{
                Domain::ContinuityState::CheckpointPreparing});
    REQUIRE(durable->retryAt == std::optional<Domain::UtcTimePoint>{retryAt});

    requireError(
        reopened.repository->recordRetry(
            operationId,
            Domain::ContinuityState::SuccessorCreating,
            "wrong resume refresh",
            retryAt,
            context("continuity-retry-refresh-wrong-state")),
        Domain::ErrorCodes::Conflict);
    requireError(
        reopened.repository->compareAndSet(
            operationId,
            Domain::ContinuityState::RetryWait,
            Domain::ContinuityState::SuccessorCreating,
            std::nullopt,
            std::nullopt,
            context("continuity-retry-wrong-resume")),
        Domain::ErrorCodes::Conflict);

    const auto resumed = transition(
        *reopened.repository,
        operationId,
        Domain::ContinuityState::RetryWait,
        Domain::ContinuityState::CheckpointPreparing,
        std::nullopt,
        "continuity-retry-exact-resume");
    REQUIRE(!resumed.retryResumeState.has_value());
    REQUIRE(!resumed.retryAt.has_value());
    REQUIRE(!resumed.lastError.has_value());
    REQUIRE(resumed.attempt == 4U);
    REQUIRE(take(reopened.repository->transitionCount(
                operationId,
                context("continuity-retry-transition-count"))) == 5U);
}

void operationHandoffAndTransitionTamperingFailClosed()
{
    const auto projectId = parse<Domain::ProjectId>(
        "14141414-1414-4414-8414-141414141414");

    {
        Support::ScopedTestDirectory directory{L"continuity-operation-tamper"};
        const auto operationId = parse<Domain::ContinuityOperationId>(
            "56565656-5656-4565-8565-565656565656");
        {
            RepositoryFixture fixture{directory.path(), projectId};
            const auto handoff = handoffFor(
                fixture,
                operationId.value(),
                "67676767-6767-4676-8676-676767676767",
                "76767676-7676-4767-8767-767676767676");
            static_cast<void>(take(fixture.repository->createOperation(
                handoff,
                take(Domain::IdempotencyKey::create("operation-tamper")),
                context("continuity-operation-tamper-create"))));
            fixture.repository->close();
        }
        executeDatabaseSql(
            directory.path() / L"memory.sqlite",
            "UPDATE rollover_operations SET state_checksum="
            "'0000000000000000000000000000000000000000000000000000000000000000'");
        RepositoryFixture reopened{directory.path(), projectId};
        requireError(
            reopened.repository->operation(
                projectId,
                operationId,
                context("continuity-operation-tamper-read")),
            Domain::ErrorCodes::IntegrityFailure);
    }

    {
        Support::ScopedTestDirectory directory{L"continuity-handoff-tamper"};
        const auto operationId = parse<Domain::ContinuityOperationId>(
            "57575757-5757-4575-8575-575757575757");
        const auto handoffId = parse<Domain::ContinuityHandoffId>(
            "68686868-6868-4686-8686-686868686868");
        {
            RepositoryFixture fixture{directory.path(), projectId};
            const auto handoff = handoffFor(
                fixture,
                operationId.value(),
                handoffId.value(),
                "78787878-7878-4787-8787-787878787878");
            static_cast<void>(take(fixture.repository->createOperation(
                handoff,
                take(Domain::IdempotencyKey::create("handoff-tamper")),
                context("continuity-handoff-tamper-create"))));
            take(fixture.repository->storeHandoff(
                handoff,
                context("continuity-handoff-tamper-store")));
            fixture.repository->close();
        }
        executeDatabaseSql(
            directory.path() / L"memory.sqlite",
            "UPDATE continuity_handoffs SET payload_json=' ' || payload_json");
        RepositoryFixture reopened{directory.path(), projectId};
        const auto tampered = reopened.repository->handoff(
            projectId,
            handoffId,
            context("continuity-handoff-tamper-read"));
        REQUIRE(!tampered);
    }

    {
        Support::ScopedTestDirectory directory{L"continuity-transition-tamper"};
        const auto operationId = parse<Domain::ContinuityOperationId>(
            "58585858-5858-4585-8585-585858585858");
        {
            RepositoryFixture fixture{directory.path(), projectId};
            const auto handoff = handoffFor(
                fixture,
                operationId.value(),
                "69696969-6969-4696-8696-696969696969",
                "79797979-7979-4797-8797-797979797979");
            static_cast<void>(take(fixture.repository->createOperation(
                handoff,
                take(Domain::IdempotencyKey::create("transition-tamper")),
                context("continuity-transition-tamper-create"))));
            static_cast<void>(transition(
                *fixture.repository,
                operationId,
                Domain::ContinuityState::Idle,
                Domain::ContinuityState::CheckpointPreparing));
            fixture.repository->close();
        }
        executeDatabaseSql(
            directory.path() / L"memory.sqlite",
            "UPDATE rollover_transitions SET state_checksum="
            "'0000000000000000000000000000000000000000000000000000000000000000' "
            "WHERE id=(SELECT MAX(id) FROM rollover_transitions)");
        RepositoryFixture reopened{directory.path(), projectId};
        requireError(
            reopened.repository->transitionCount(
                operationId,
                context("continuity-transition-tamper-read")),
            Domain::ErrorCodes::IntegrityFailure);
    }
}

void continuityResetPreservesProjectMemoryAndMetadata()
{
    Support::ScopedTestDirectory directory{L"continuity-reset-preservation"};
    const auto projectId = parse<Domain::ProjectId>(
        "15151515-1515-4515-8515-151515151515");
    RepositoryFixture fixture{directory.path(), projectId};
    const auto first = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId,
            memoryWrite(
                "Preserved decision A",
                "first preserved memory record",
                {"continuity", "preserved"})},
        context("continuity-reset-memory-first")));
    auto secondWrite = memoryWrite(
        "Preserved decision B",
        "second preserved memory record",
        {"continuity"});
    secondWrite.relatedIds.push_back(first.recordId);
    const auto second = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, secondWrite},
        context("continuity-reset-memory-second")));
    static_cast<void>(take(fixture.repository->link(
        Domain::LinkProjectMemoryRequest{
            projectId, first.recordId, second.recordId, "supports"},
        context("continuity-reset-memory-link"))));

    const auto databasePath = directory.path() / L"memory.sqlite";
    executeDatabaseSql(
        databasePath,
        "INSERT INTO artifacts(id,project_id,path,checksum,created_at) VALUES("
        "'90909090-9090-4090-8090-909090909090',"
        "'15151515-1515-4515-8515-151515151515','preserved-artifact.json',"
        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
        "'2026-01-02T03:00:00Z');"
        "INSERT INTO project_metadata(project_id,display_name,repository_identity,"
        "schema_version,created_at,updated_at) VALUES("
        "'15151515-1515-4515-8515-151515151515','Preserved Project',"
        "'preserved-repository',3,'2026-01-02T03:00:00Z',"
        "'2026-01-02T03:00:00Z');");

    const auto handoff = handoffFor(
        fixture,
        "59595959-5959-4595-8595-595959595959",
        "70707070-7070-4707-8707-707070707070",
        "80808080-8080-4808-8808-808080808080");
    const auto operation = take(fixture.repository->createOperation(
        handoff,
        take(Domain::IdempotencyKey::create("continuity-reset-operation")),
        context("continuity-reset-operation-create")));
    static_cast<void>(transition(
        *fixture.repository,
        operation.operationId,
        Domain::ContinuityState::Idle,
        Domain::ContinuityState::CheckpointPreparing));
    take(fixture.repository->storeHandoff(
        handoff, context("continuity-reset-handoff-store")));

    const auto reset = take(fixture.repository->resetContinuity(
        Domain::ContinuityResetRequest{
            projectId,
            Domain::DestructiveConfirmation{
                "reset_project_continuity",
                projectId.value(),
                "RESET PROJECT CONTINUITY " + projectId.value()}},
        context("continuity-reset-execute")));
    REQUIRE(reset.projectId == projectId);
    REQUIRE(reset.report.verified);
    REQUIRE(reset.report.recordsRemoved == 1U);
    REQUIRE(reset.report.linksRemoved == 1U);
    REQUIRE(reset.report.eventsRemoved == 2U);

    const auto memoryStatus = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("continuity-reset-memory-status")));
    REQUIRE(memoryStatus.recordCount == 2U);
    REQUIRE(memoryStatus.tombstoneCount == 0U);
    const auto continuityStatus = take(fixture.repository->status(
        projectId, context("continuity-reset-status")));
    REQUIRE(continuityStatus.operationCount == 0U);
    REQUIRE(continuityStatus.handoffCount == 0U);
    REQUIRE(!continuityStatus.activeOperation.has_value());

    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM memory_records") == 2);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM memory_tags") == 2);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM memory_record_tags") == 3);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM memory_links") >= 1);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM artifacts") == 1);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM project_metadata") == 1);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM continuity_handoffs") == 0);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM rollover_operations") == 0);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM rollover_transitions") == 0);
    REQUIRE(queryDatabaseInteger(
                databasePath,
                "SELECT COUNT(*) FROM project_active_sessions") == 0);
}

void sameProjectContentionAndIndependentProjectsRemainBounded()
{
    {
        Support::ScopedTestDirectory directory{L"continuity-same-project-race"};
        const auto projectId = parse<Domain::ProjectId>(
            "16161616-1616-4616-8616-161616161616");
        RepositoryFixture first{directory.path(), projectId};
        RepositoryFixture second{directory.path(), projectId};
        const std::array handoffs{
            handoffFor(
                first,
                "91919191-9191-4191-8191-919191919191",
                "92929292-9292-4292-8292-929292929292",
                "93939393-9393-4393-8393-939393939393"),
            handoffFor(
                second,
                "94949494-9494-4494-8494-949494949494",
                "95959595-9595-4595-8595-959595959595",
                "96969696-9696-4696-8696-969696969696")};
        const std::array idempotency{
            take(Domain::IdempotencyKey::create("same-project-first")),
            take(Domain::IdempotencyKey::create("same-project-second"))};
        std::array<std::optional<Domain::Result<Domain::ContinuityOperation>>, 2U>
            results;
        std::latch start{3};
        std::jthread firstWorker{[&] {
            start.count_down();
            start.wait();
            results[0].emplace(first.repository->createOperation(
                handoffs[0],
                idempotency[0],
                context("continuity-same-project-first")));
        }};
        std::jthread secondWorker{[&] {
            start.count_down();
            start.wait();
            results[1].emplace(second.repository->createOperation(
                handoffs[1],
                idempotency[1],
                context("continuity-same-project-second")));
        }};
        start.count_down();
        start.wait();
        firstWorker.join();
        secondWorker.join();

        REQUIRE(results[0].has_value());
        REQUIRE(results[1].has_value());
        const auto successCount =
            static_cast<std::size_t>(static_cast<bool>(*results[0])) +
            static_cast<std::size_t>(static_cast<bool>(*results[1]));
        REQUIRE(successCount == 1U);
        for (const auto& result : results) {
            if (!*result) {
                REQUIRE(result->error().code == Domain::ErrorCodes::Conflict);
            }
        }
        REQUIRE(queryDatabaseInteger(
                    directory.path() / L"memory.sqlite",
                    "SELECT COUNT(*) FROM rollover_operations") == 1);
        REQUIRE(queryDatabaseInteger(
                    directory.path() / L"memory.sqlite",
                    "SELECT COUNT(*) FROM rollover_operations WHERE state='idle'") == 1);
    }

    {
        Support::ScopedTestDirectory firstDirectory{
            L"continuity-independent-project-a"};
        Support::ScopedTestDirectory secondDirectory{
            L"continuity-independent-project-b"};
        const auto firstProjectId = parse<Domain::ProjectId>(
            "17171717-1717-4717-8717-171717171717");
        const auto secondProjectId = parse<Domain::ProjectId>(
            "18181818-1818-4818-8818-181818181818");
        RepositoryFixture first{firstDirectory.path(), firstProjectId};
        RepositoryFixture second{secondDirectory.path(), secondProjectId};
        const auto firstHandoff = handoffFor(
            first,
            "a1a1a1a1-a1a1-41a1-81a1-a1a1a1a1a1a1",
            "a2a2a2a2-a2a2-42a2-82a2-a2a2a2a2a2a2",
            "a3a3a3a3-a3a3-43a3-83a3-a3a3a3a3a3a3");
        const auto secondHandoff = handoffFor(
            second,
            "b1b1b1b1-b1b1-41b1-81b1-b1b1b1b1b1b1",
            "b2b2b2b2-b2b2-42b2-82b2-b2b2b2b2b2b2",
            "b3b3b3b3-b3b3-43b3-83b3-b3b3b3b3b3b3");
        std::array<std::optional<Domain::Result<Domain::ContinuityOperation>>, 2U>
            results;
        std::latch start{3};
        std::jthread firstWorker{[&] {
            start.count_down();
            start.wait();
            results[0].emplace(first.repository->createOperation(
                firstHandoff,
                take(Domain::IdempotencyKey::create("independent-first")),
                context("continuity-independent-first")));
        }};
        std::jthread secondWorker{[&] {
            start.count_down();
            start.wait();
            results[1].emplace(second.repository->createOperation(
                secondHandoff,
                take(Domain::IdempotencyKey::create("independent-second")),
                context("continuity-independent-second")));
        }};
        start.count_down();
        start.wait();
        firstWorker.join();
        secondWorker.join();
        REQUIRE(results[0].has_value() && *results[0]);
        REQUIRE(results[1].has_value() && *results[1]);
        REQUIRE(results[0]->value().projectId == firstProjectId);
        REQUIRE(results[1]->value().projectId == secondProjectId);
    }
}

} // namespace

int main()
{
    try {
        createReplayBindingAndProjectV1RoundTrip();
        std::cout << "PASS continuity_repository.create_replay_binding_project_v1\n";
        fullCasAcknowledgementAndTerminalSlotRelease();
        std::cout << "PASS continuity_repository.cas_ack_pointer_slot_release\n";
        retryResumeStatePersistsAndBindsExactResume();
        std::cout << "PASS continuity_repository.retry_resume_persistence\n";
        operationHandoffAndTransitionTamperingFailClosed();
        std::cout << "PASS continuity_repository.integrity_tamper_rejection\n";
        continuityResetPreservesProjectMemoryAndMetadata();
        std::cout << "PASS continuity_repository.reset_preserves_project_data\n";
        sameProjectContentionAndIndependentProjectsRemainBounded();
        std::cout << "PASS continuity_repository.project_concurrency\n";
        std::cout << "SUMMARY passed=6 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
