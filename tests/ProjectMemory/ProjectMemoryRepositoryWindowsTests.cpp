#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <winsqlite/winsqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::OperationContext context(const std::string_view correlation)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        std::chrono::steady_clock::now() + std::chrono::minutes{2},
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] std::vector<Domain::Uuid> uuids()
{
    std::vector<Domain::Uuid> values;
    for (const char* const value : {
             "10000000-0000-4000-8000-000000000001",
             "10000000-0000-4000-8000-000000000002",
             "10000000-0000-4000-8000-000000000003",
             "10000000-0000-4000-8000-000000000004",
             "10000000-0000-4000-8000-000000000005",
             "10000000-0000-4000-8000-000000000006",
             "10000000-0000-4000-8000-000000000007",
             "10000000-0000-4000-8000-000000000008"}) {
        values.push_back(parse<Domain::Uuid>(value));
    }
    return values;
}

class WhitespaceOnMarkerRedactor final : public Contracts::IRedactor {
public:
    [[nodiscard]] Domain::Result<std::string> redact(
        const std::string_view value) noexcept override
    {
        try {
            return Domain::Result<std::string>::success(
                value == "erase-during-redaction"
                    ? std::string{" \t\r\n"}
                    : std::string{value});
        } catch (...) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The test redactor could not allocate its result."));
        }
    }
};

struct RepositoryFixture final {
    RepositoryFixture(
        const std::filesystem::path& directory,
        const Domain::ProjectId& projectId,
        std::vector<Domain::Uuid> uuidSequence = uuids(),
        const bool enableFts5 = false,
        std::shared_ptr<Contracts::IRedactor> configuredRedactor = {})
    {
        paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
        const auto now = std::chrono::steady_clock::now();
        paths->setNow(now);
        paths->projectRootResult.set(
            Domain::Result<Domain::PathText>::success(Support::pathText(directory)));
        diagnostics = std::make_shared<Fakes::RuntimeDiagnosticsFake>(now);
        const auto day = std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::January / 2};
        clock = std::make_shared<Support::FixedClock>(
            Domain::UtcTimePoint{day.time_since_epoch() + std::chrono::hours{3}},
            now);
        redactor = configuredRedactor
            ? std::move(configuredRedactor)
            : std::make_shared<InfrastructureWindows::SecretRedactor>();
        hasher = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
        uuidGenerator = std::make_shared<Fakes::SequenceUuidGenerator>(
            std::move(uuidSequence));
        PersistenceWindows::WindowsProjectMemoryRepositoryOptions options{};
        options.database.enableFts5 = enableFts5;
        repository = take(PersistenceWindows::WindowsProjectMemoryRepository::open(
            projectId,
            paths,
            diagnostics,
            redactor,
            hasher,
            uuidGenerator,
            clock,
            options,
            context("project-memory-repository-open")));
    }

    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Contracts::IRedactor> redactor;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher;
    std::shared_ptr<Fakes::SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryRepository> repository;
};

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
        throw std::runtime_error{"Could not open the adversarial project-memory database."};
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
            "Could not inject adversarial project-memory data: " + errorText};
    }
    REQUIRE(closeResult == SQLITE_OK);
}

[[nodiscard]] std::int64_t queryDatabaseInteger(
    const std::filesystem::path& databasePath,
    const std::string_view sql)
{
    const auto encodedPath = Support::pathText(databasePath).value();
    sqlite3* database{};
    const int openResult = sqlite3_open_v2(
        encodedPath.c_str(),
        &database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (openResult != SQLITE_OK) {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
        throw std::runtime_error{"Could not open the project-memory database for inspection."};
    }

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
    if (stepResult != SQLITE_ROW) {
        throw std::runtime_error{"Could not query the project-memory database."};
    }
    REQUIRE(closeResult == SQLITE_OK);
    return value;
}

void requireGetFailure(
    PersistenceWindows::WindowsProjectMemoryRepository& repository,
    const Domain::ProjectId& projectId,
    const Domain::MemoryRecordId& recordId,
    const std::string_view errorCode,
    const std::string_view correlation)
{
    const auto result = repository.get(
        Domain::GetProjectMemoryRequest{projectId, {recordId}, true},
        context(correlation));
    REQUIRE(!result);
    REQUIRE(result.error().code == errorCode);
}

[[nodiscard]] Domain::ProjectMemoryWrite write(
    std::string title,
    std::string summary,
    std::optional<std::string> body = std::nullopt,
    std::vector<std::string> tags = {})
{
    Domain::ProjectMemoryWrite value{};
    value.kind = "decision";
    value.title = std::move(title);
    value.summary = std::move(summary);
    value.body = std::move(body);
    value.tags = std::move(tags);
    return value;
}

void crudSearchConflictLinkAndReset()
{
    Support::ScopedTestDirectory directory{L"project-memory-repository-crud"};
    const auto projectId = parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
    RepositoryFixture fixture{directory.path(), projectId};

    auto firstWrite = write(
        "Alpha decision",
        "keep secret=credential out of memory",
        "Authorization: Bearer token-value",
        {" Architecture ", "release"});
    firstWrite.idempotencyKey = take(Domain::IdempotencyKey::create("alpha-once"));
    const auto first = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, firstWrite},
        context("project-memory-remember-first")));
    REQUIRE(first.disposition == Domain::MemoryWriteDisposition::Inserted);

    const auto deduplicated = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, firstWrite},
        context("project-memory-remember-dedupe")));
    REQUIRE(deduplicated.disposition == Domain::MemoryWriteDisposition::Deduplicated);
    REQUIRE(deduplicated.recordId == first.recordId);

    auto secondWrite = write(
        "Beta decision", "second record", "bounded body", {"architecture"});
    secondWrite.idempotencyKey = take(Domain::IdempotencyKey::create("beta-once"));
    secondWrite.relatedIds.push_back(first.recordId);
    const auto second = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, secondWrite},
        context("project-memory-remember-second")));

    const auto linked = take(fixture.repository->link(
        Domain::LinkProjectMemoryRequest{
            projectId, first.recordId, second.recordId, "supports"},
        context("project-memory-link")));
    REQUIRE(linked.disposition == Domain::LinkDisposition::Inserted);
    const auto relinked = take(fixture.repository->link(
        Domain::LinkProjectMemoryRequest{
            projectId, first.recordId, second.recordId, "supports"},
        context("project-memory-relink")));
    REQUIRE(relinked.disposition == Domain::LinkDisposition::Deduplicated);

    const auto fetched = take(fixture.repository->get(
        Domain::GetProjectMemoryRequest{projectId, {first.recordId}, true},
        context("project-memory-get")));
    REQUIRE(fetched.records.size() == 1U);
    REQUIRE(fetched.records.front().summary.find("credential") == std::string::npos);
    REQUIRE(fetched.records.front().body.has_value());
    REQUIRE(fetched.records.front().body->find("token-value") == std::string::npos);

    const auto page = take(fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId,
            "decision",
            {},
            {"architecture"},
            std::nullopt,
            1U,
            std::nullopt,
            false,
            64U * 1024U},
        context("project-memory-search")));
    REQUIRE(page.records.size() == 1U);
    REQUIRE(page.truncated);
    REQUIRE(page.nextCursor.has_value());

    const auto conflict = fixture.repository->update(
        Domain::UpdateProjectMemoryRequest{
            projectId, first.recordId, 99U, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt},
        context("project-memory-update-conflict"));
    REQUIRE(!conflict);
    REQUIRE(conflict.error().code == Domain::ErrorCodes::Conflict);

    const auto touched = take(fixture.repository->update(
        Domain::UpdateProjectMemoryRequest{
            projectId, first.recordId, first.recordVersion, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt},
        context("project-memory-update-touch")));
    REQUIRE(touched.version == first.recordVersion + 1U);
    REQUIRE(touched.contentHash == first.contentHash);

    const auto forgotten = take(fixture.repository->forget(
        Domain::ForgetProjectMemoryRequest{projectId, second.recordId},
        context("project-memory-forget")));
    REQUIRE(forgotten.disposition == Domain::ForgetDisposition::Tombstoned);
    const auto forgottenAgain = take(fixture.repository->forget(
        Domain::ForgetProjectMemoryRequest{projectId, second.recordId},
        context("project-memory-forget-again")));
    REQUIRE(forgottenAgain.disposition == Domain::ForgetDisposition::NotFound);
    const auto forgottenRetry = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, secondWrite},
        context("project-memory-forgotten-idempotency-retry")));
    REQUIRE(
        forgottenRetry.disposition ==
        Domain::MemoryWriteDisposition::Deduplicated);
    REQUIRE(forgottenRetry.recordId == second.recordId);
    REQUIRE(forgottenRetry.recordVersion == second.recordVersion + 1U);
    REQUIRE(forgottenRetry.contentHash != second.contentHash);

    const auto status = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-status")));
    REQUIRE(status.recordCount == 1U);
    REQUIRE(status.tombstoneCount == 1U);
    REQUIRE(status.integrityOk);
    REQUIRE(status.databaseBytes > 0U);

    const std::string token = "RESET PROJECT MEMORY " + projectId.value();
    const auto reset = take(fixture.repository->resetMemory(
        Domain::DestructiveConfirmation{
            "reset_project_memory", projectId.value(), token},
        context("project-memory-reset")));
    REQUIRE(reset.verified);
    REQUIRE(reset.recordsRemoved == 2U);
    REQUIRE(reset.linksRemoved >= 1U);
    const auto emptyStatus = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-status-after-reset")));
    REQUIRE(emptyStatus.recordCount == 0U);
    REQUIRE(emptyStatus.tombstoneCount == 0U);

    fixture.repository->close();
    fixture.repository->close();
}

void batchAtomicityBoundsAndPrivateKeyRejection()
{
    Support::ScopedTestDirectory directory{L"project-memory-repository-bounds"};
    const auto projectId = parse<Domain::ProjectId>(
        "22222222-2222-4222-8222-222222222222");
    RepositoryFixture fixture{directory.path(), projectId};

    const auto invalidBatch = fixture.repository->rememberBatch(
        Domain::RememberProjectMemoryBatchRequest{
            projectId,
            {write("valid", "valid summary"), write("", "invalid summary")}},
        context("project-memory-invalid-batch"));
    REQUIRE(!invalidBatch);
    const auto emptyStatus = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-empty-status")));
    REQUIRE(emptyStatus.recordCount == 0U);

    const auto privateKey = fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId,
            write(
                "private key",
                "must reject",
                "-----BEGIN PRIVATE KEY-----\nmaterial")},
        context("project-memory-private-key"));
    REQUIRE(!privateKey);
    REQUIRE(privateKey.error().code == Domain::ErrorCodes::RedactionRejected);

    const std::string body(2U * 1024U, 'x');
    const auto inserted = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("large", "large response", body)},
        context("project-memory-large")));
    const auto oversized = fixture.repository->get(
        Domain::GetProjectMemoryRequest{
            projectId, {inserted.recordId}, true, 1024U},
        context("project-memory-get-bound"));
    REQUIRE(!oversized);
    REQUIRE(oversized.error().code == Domain::ErrorCodes::PayloadTooLarge);
}

void projectIsolationAndScopeFailure()
{
    Support::ScopedTestDirectory firstDirectory{L"project-memory-isolation-a"};
    Support::ScopedTestDirectory secondDirectory{L"project-memory-isolation-b"};
    const auto firstProject = parse<Domain::ProjectId>(
        "33333333-3333-4333-8333-333333333333");
    const auto secondProject = parse<Domain::ProjectId>(
        "44444444-4444-4444-8444-444444444444");
    RepositoryFixture first{firstDirectory.path(), firstProject};
    RepositoryFixture second{secondDirectory.path(), secondProject};
    const auto record = take(first.repository->remember(
        Domain::RememberProjectMemoryRequest{
            firstProject, write("isolated", "only project A")},
        context("project-memory-isolation-write")));

    const auto secondGet = take(second.repository->get(
        Domain::GetProjectMemoryRequest{secondProject, {record.recordId}, true},
        context("project-memory-isolation-negative")));
    REQUIRE(secondGet.records.empty());

    const auto mismatched = first.repository->get(
        Domain::GetProjectMemoryRequest{secondProject, {record.recordId}, true},
        context("project-memory-scope-mismatch"));
    REQUIRE(!mismatched);
    REQUIRE(mismatched.error().code == Domain::ErrorCodes::ProjectScopeMismatch);
}

void deterministicRankingPaginationAndRecentOrder()
{
    Support::ScopedTestDirectory directory{L"project-memory-ranking"};
    const auto projectId = parse<Domain::ProjectId>(
        "55555555-5555-4555-8555-555555555555");
    RepositoryFixture fixture{directory.path(), projectId};

    const auto exact = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("needle", "rank exact", std::nullopt, {"rank"})},
        context("project-memory-rank-exact")));
    const auto title = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("prefix needle suffix", "rank title")},
        context("project-memory-rank-title")));
    const auto summary = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("summary-only", "contains needle")},
        context("project-memory-rank-summary")));
    const auto body = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("body-only", "rank body", "contains needle")},
        context("project-memory-rank-body")));
    static_cast<void>(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("unmatched", "not relevant")},
        context("project-memory-rank-unmatched")));

    const auto first = take(fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId, "needle", {}, {}, std::nullopt, 2U, std::nullopt,
            false, 64U * 1024U},
        context("project-memory-rank-page-one")));
    REQUIRE(first.records.size() == 2U);
    REQUIRE(first.records[0].record.id == exact.recordId);
    REQUIRE(first.records[0].score == 410.0);
    REQUIRE(first.records[1].record.id == title.recordId);
    REQUIRE(first.records[1].score == 210.0);
    REQUIRE(first.truncated);
    REQUIRE(first.nextCursor.has_value());

    const auto second = take(fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId, "needle", {}, {}, std::nullopt, 2U, first.nextCursor,
            false, 64U * 1024U},
        context("project-memory-rank-page-two")));
    REQUIRE(second.records.size() == 2U);
    REQUIRE(second.records[0].record.id == summary.recordId);
    REQUIRE(second.records[0].score == 90.0);
    REQUIRE(second.records[1].record.id == body.recordId);
    REQUIRE(second.records[1].score == 40.0);
    REQUIRE(!second.truncated);
    REQUIRE(!second.nextCursor.has_value());

    const auto exactId = take(fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId, exact.recordId.value(), {}, {}, std::nullopt, 20U,
            std::nullopt, false, 64U * 1024U},
        context("project-memory-rank-id")));
    REQUIRE(exactId.records.size() == 1U);
    REQUIRE(exactId.records.front().record.id == exact.recordId);
    REQUIRE(exactId.records.front().score == 1010.0);

    const auto tagged = take(fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId, "needle", {}, {"rank"}, std::nullopt, 20U,
            std::nullopt, false, 64U * 1024U},
        context("project-memory-rank-tag")));
    REQUIRE(tagged.records.size() == 1U);
    REQUIRE(tagged.records.front().record.id == exact.recordId);

    const auto recent = take(fixture.repository->listRecent(
        Domain::ListRecentProjectMemoryRequest{
            projectId, {}, std::nullopt, 3U, std::nullopt, false,
            64U * 1024U},
        context("project-memory-recent-order")));
    REQUIRE(recent.records.size() == 3U);
    REQUIRE(recent.records[0].record.id == exact.recordId);
    REQUIRE(recent.records[1].record.id == title.recordId);
    REQUIRE(recent.records[2].record.id == summary.recordId);
}

void batchFailureRollsBackPreviouslyInsertedRows()
{
    Support::ScopedTestDirectory directory{L"project-memory-batch-rollback"};
    const auto projectId = parse<Domain::ProjectId>(
        "66666666-6666-4666-8666-666666666666");
    const auto onlyGeneratedUuid = parse<Domain::Uuid>(
        "20000000-0000-4000-8000-000000000001");
    RepositoryFixture fixture{
        directory.path(), projectId, {onlyGeneratedUuid}};

    const auto failed = fixture.repository->rememberBatch(
        Domain::RememberProjectMemoryBatchRequest{
            projectId,
            {write("first transactional row", "must be rolled back"),
             write("second transactional row", "forces UUID exhaustion")}},
        context("project-memory-batch-mid-transaction-failure"));
    REQUIRE(!failed);
    REQUIRE(failed.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(fixture.uuidGenerator->consumed() == 1U);

    const auto status = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-batch-rollback-status")));
    REQUIRE(status.recordCount == 0U);
    REQUIRE(status.tombstoneCount == 0U);
    REQUIRE(status.eventCount == 0U);

    const auto rolledBackId = parse<Domain::MemoryRecordId>(
        onlyGeneratedUuid.value());
    const auto lookup = take(fixture.repository->get(
        Domain::GetProjectMemoryRequest{projectId, {rolledBackId}, true},
        context("project-memory-batch-rollback-lookup")));
    REQUIRE(lookup.records.empty());
}

void getPreservesCallerOrderAndDuplicates()
{
    Support::ScopedTestDirectory directory{L"project-memory-get-order"};
    const auto projectId = parse<Domain::ProjectId>(
        "77777777-7777-4777-8777-777777777777");
    RepositoryFixture fixture{directory.path(), projectId};

    const auto first = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("first", "first record")},
        context("project-memory-get-order-first")));
    const auto second = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("second", "second record")},
        context("project-memory-get-order-second")));
    const auto third = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("third", "third record")},
        context("project-memory-get-order-third")));

    const auto fetched = take(fixture.repository->get(
        Domain::GetProjectMemoryRequest{
            projectId,
            {third.recordId, first.recordId, third.recordId, second.recordId},
            false},
        context("project-memory-get-order-fetch")));
    REQUIRE(fetched.records.size() == 4U);
    REQUIRE(fetched.records[0].id == third.recordId);
    REQUIRE(fetched.records[1].id == first.recordId);
    REQUIRE(fetched.records[2].id == third.recordId);
    REQUIRE(fetched.records[3].id == second.recordId);
}

void oversizedFirstSearchRecordFailsExplicitly()
{
    Support::ScopedTestDirectory directory{L"project-memory-first-row-cap"};
    const auto projectId = parse<Domain::ProjectId>(
        "88888888-8888-4888-8888-888888888888");
    RepositoryFixture fixture{directory.path(), projectId};

    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId,
            write(
                "oversized-first-record",
                "response cap regression",
                std::string(2U * 1024U, 'z'))},
        context("project-memory-first-row-cap-write"))));

    const auto measured = take(fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId,
            "oversized-first-record",
            {},
            {},
            std::nullopt,
            20U,
            std::nullopt,
            true,
            64U * 1024U},
        context("project-memory-first-row-cap-measure")));
    REQUIRE(measured.records.size() == 1U);
    REQUIRE(!measured.truncated);
    REQUIRE(measured.encodedBytes > 1024U);

    std::size_t exactLimit = measured.encodedBytes;
    bool exactLimitConverged{};
    for (std::size_t attempt = 0U; attempt < 8U; ++attempt) {
        const auto exact = take(fixture.repository->search(
            Domain::SearchProjectMemoryRequest{
                projectId,
                "oversized-first-record",
                {},
                {},
                std::nullopt,
                20U,
                std::nullopt,
                true,
                exactLimit},
            context("project-memory-first-row-cap-exact")));
        REQUIRE(exact.records.size() == 1U);
        REQUIRE(exact.encodedBytes <= exactLimit);
        if (exact.encodedBytes == exactLimit) {
            exactLimitConverged = true;
            break;
        }
        exactLimit = exact.encodedBytes;
    }
    REQUIRE(exactLimitConverged);

    const auto search = fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId,
            "oversized-first-record",
            {},
            {},
            std::nullopt,
            20U,
            std::nullopt,
            true,
            1024U},
        context("project-memory-first-row-cap-search"));
    REQUIRE(!search);
    REQUIRE(search.error().code == Domain::ErrorCodes::PayloadTooLarge);

    const auto oneByteShort = fixture.repository->search(
        Domain::SearchProjectMemoryRequest{
            projectId,
            "oversized-first-record",
            {},
            {},
            std::nullopt,
            20U,
            std::nullopt,
            true,
            exactLimit - 1U},
        context("project-memory-first-row-cap-one-byte-short"));
    REQUIRE(!oneByteShort);
    REQUIRE(oneByteShort.error().code == Domain::ErrorCodes::PayloadTooLarge);
}

void persistedHostileRowsFailClosed()
{
    Support::ScopedTestDirectory directory{L"project-memory-hostile-rows"};
    const auto projectId = parse<Domain::ProjectId>(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    std::vector<Domain::MemoryRecordId> recordIds;
    {
        RepositoryFixture fixture{directory.path(), projectId};
        for (const auto& title : {
                 "unsupported schema",
                 "out of range number",
                 "nonfinite number",
                 "invalid utf8",
                 "embedded nul",
                 "noncanonical kind",
                 "content hash mismatch"}) {
            recordIds.push_back(take(fixture.repository->remember(
                Domain::RememberProjectMemoryRequest{
                    projectId, write(title, "adversarial persisted row")},
                context("project-memory-hostile-row-write"))).recordId);
        }
        fixture.repository->close();
    }

    std::string mutation{"BEGIN IMMEDIATE;"};
    mutation += "UPDATE memory_records SET schema_version=2 WHERE id='" +
                recordIds[0].value() + "';";
    mutation += "UPDATE memory_records SET importance=2.0 WHERE id='" +
                recordIds[1].value() + "';";
    mutation += "UPDATE memory_records SET confidence=1e999 WHERE id='" +
                recordIds[2].value() + "';";
    mutation += "UPDATE memory_records SET title=CAST(X'80' AS TEXT) WHERE id='" +
                recordIds[3].value() + "';";
    mutation += "UPDATE memory_records SET summary='visible'||char(0)||'hidden' WHERE id='" +
                recordIds[4].value() + "';";
    mutation += "UPDATE memory_records SET kind=' Decision ' WHERE id='" +
                recordIds[5].value() + "';";
    mutation += "UPDATE memory_records SET content_hash='" +
                std::string(64U, '0') + "' WHERE id='" +
                recordIds[6].value() + "';COMMIT;";
    executeDatabaseSql(directory.path() / L"memory.sqlite", mutation);

    RepositoryFixture reopened{directory.path(), projectId};
    requireGetFailure(
        *reopened.repository,
        projectId,
        recordIds[0],
        Domain::ErrorCodes::UnsupportedVersion,
        "project-memory-hostile-schema");
    for (std::size_t index = 1U; index < recordIds.size(); ++index) {
        requireGetFailure(
            *reopened.repository,
            projectId,
            recordIds[index],
            Domain::ErrorCodes::IntegrityFailure,
            "project-memory-hostile-semantic");
    }
}

void incompleteFtsObjectSetIsUnavailable()
{
    Support::ScopedTestDirectory directory{L"project-memory-incomplete-fts"};
    const auto projectId = parse<Domain::ProjectId>(
        "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    RepositoryFixture fixture{directory.path(), projectId, uuids(), true};

    const auto complete = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-fts-complete")));
    REQUIRE(complete.fullTextSearchAvailable);

    executeDatabaseSql(
        directory.path() / L"memory.sqlite",
        "DROP TRIGGER memory_fts_delete;");
    const auto incomplete = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-fts-incomplete")));
    REQUIRE(!incomplete.fullTextSearchAvailable);
}

void cursorInt64BoundaryFailsClosed()
{
    Support::ScopedTestDirectory directory{L"project-memory-cursor-boundary"};
    const auto projectId = parse<Domain::ProjectId>(
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    RepositoryFixture fixture{directory.path(), projectId};
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("cursor boundary", "must remain bounded")},
        context("project-memory-cursor-boundary-write"))));

    const auto maximum = take(fixture.repository->listRecent(
        Domain::ListRecentProjectMemoryRequest{
            projectId,
            {},
            std::nullopt,
            20U,
            std::optional<std::string>{"djE6OTIyMzM3MjAzNjg1NDc3NTgwNw=="},
            false,
            64U * 1024U},
        context("project-memory-cursor-int64-maximum")));
    REQUIRE(maximum.records.empty());
    REQUIRE(!maximum.truncated);
    REQUIRE(!maximum.nextCursor.has_value());

    const auto beyondMaximum = fixture.repository->listRecent(
        Domain::ListRecentProjectMemoryRequest{
            projectId,
            {},
            std::nullopt,
            20U,
            std::optional<std::string>{"djE6OTIyMzM3MjAzNjg1NDc3NTgwOA=="},
            false,
            64U * 1024U},
        context("project-memory-cursor-beyond-int64"));
    REQUIRE(!beyondMaximum);
    REQUIRE(beyondMaximum.error().code == Domain::ErrorCodes::InvalidRequest);
}

void closeRejectsFurtherRepositoryUse()
{
    Support::ScopedTestDirectory directory{L"project-memory-post-close"};
    const auto projectId = parse<Domain::ProjectId>(
        "99999999-9999-4999-8999-999999999999");
    RepositoryFixture fixture{directory.path(), projectId};

    fixture.repository->close();
    const auto rejected = fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-post-close-status"));
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::InvalidRequest);
}

void recordsSurviveRepositoryRestart()
{
    Support::ScopedTestDirectory directory{L"project-memory-restart"};
    const auto projectId = parse<Domain::ProjectId>(
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");

    std::optional<Domain::MemoryRecordId> recordId;
    {
        RepositoryFixture first{directory.path(), projectId};
        const auto inserted = take(first.repository->remember(
            Domain::RememberProjectMemoryRequest{
                projectId,
                write("durable", "survives restart", "persisted body")},
            context("project-memory-restart-write")));
        recordId = inserted.recordId;
        first.repository->close();
    }

    REQUIRE(recordId.has_value());
    RepositoryFixture reopened{directory.path(), projectId};
    const auto fetched = take(reopened.repository->get(
        Domain::GetProjectMemoryRequest{projectId, {*recordId}, true},
        context("project-memory-restart-read")));
    REQUIRE(fetched.records.size() == 1U);
    REQUIRE(fetched.records.front().id == *recordId);
    REQUIRE(fetched.records.front().title == "durable");
    REQUIRE(fetched.records.front().body == std::optional<std::string>{"persisted body"});

    const auto status = take(reopened.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-restart-status")));
    REQUIRE(status.recordCount == 1U);
    REQUIRE(status.integrityOk);
}

void cancelledAndExpiredOperationsAreRejected()
{
    Support::ScopedTestDirectory directory{L"project-memory-context-guard"};
    const auto projectId = parse<Domain::ProjectId>(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    RepositoryFixture fixture{directory.path(), projectId};

    std::stop_source stopSource;
    stopSource.request_stop();
    const Domain::OperationContext cancelled{
        parse<Domain::OperationId>("cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        fixture.clock->monotonicNow() + std::chrono::minutes{1},
        stopSource.get_token(),
        parse<Domain::CorrelationId>("project-memory-cancelled")};
    const auto cancelledResult = fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId}, cancelled);
    REQUIRE(!cancelledResult);
    REQUIRE(cancelledResult.error().code == Domain::ErrorCodes::Cancelled);

    const Domain::OperationContext expired{
        parse<Domain::OperationId>("dddddddd-dddd-4ddd-8ddd-dddddddddddd"),
        fixture.clock->monotonicNow(),
        {},
        parse<Domain::CorrelationId>("project-memory-expired")};
    const auto expiredResult = fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId}, expired);
    REQUIRE(!expiredResult);
    REQUIRE(expiredResult.error().code == Domain::ErrorCodes::DeadlineExceeded);
}

void updateNormalizesRequiredTextAndRejectsEmptyResults()
{
    Support::ScopedTestDirectory directory{L"project-memory-update-normalization"};
    const auto projectId = parse<Domain::ProjectId>(
        "10101010-1010-4010-8010-101010101010");
    auto redactor = std::make_shared<WhitespaceOnMarkerRedactor>();
    RepositoryFixture fixture{directory.path(), projectId, uuids(), false, redactor};

    const auto inserted = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("original title", "original summary")},
        context("project-memory-update-normalization-write")));
    const auto normalized = take(fixture.repository->update(
        Domain::UpdateProjectMemoryRequest{
            projectId,
            inserted.recordId,
            inserted.recordVersion,
            std::optional<std::string>{" \t normalized title \r\n"},
            std::optional<std::string>{"\n normalized summary \t"},
            std::nullopt,
            std::nullopt},
        context("project-memory-update-normalization-success")));
    REQUIRE(normalized.title == "normalized title");
    REQUIRE(normalized.summary == "normalized summary");
    REQUIRE(normalized.version == inserted.recordVersion + 1U);

    const auto whitespaceTitle = fixture.repository->update(
        Domain::UpdateProjectMemoryRequest{
            projectId,
            inserted.recordId,
            normalized.version,
            std::optional<std::string>{" \t\r\n"},
            std::nullopt,
            std::nullopt,
            std::nullopt},
        context("project-memory-update-empty-title"));
    REQUIRE(!whitespaceTitle);
    REQUIRE(whitespaceTitle.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto whitespaceSummary = fixture.repository->update(
        Domain::UpdateProjectMemoryRequest{
            projectId,
            inserted.recordId,
            normalized.version,
            std::nullopt,
            std::optional<std::string>{"\n\t "},
            std::nullopt,
            std::nullopt},
        context("project-memory-update-empty-summary"));
    REQUIRE(!whitespaceSummary);
    REQUIRE(whitespaceSummary.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto redactedEmpty = fixture.repository->update(
        Domain::UpdateProjectMemoryRequest{
            projectId,
            inserted.recordId,
            normalized.version,
            std::optional<std::string>{"erase-during-redaction"},
            std::nullopt,
            std::nullopt,
            std::nullopt},
        context("project-memory-update-redacted-empty"));
    REQUIRE(!redactedEmpty);
    REQUIRE(redactedEmpty.error().code == Domain::ErrorCodes::InvalidRequest);

    const auto unchanged = take(fixture.repository->get(
        Domain::GetProjectMemoryRequest{projectId, {inserted.recordId}, true},
        context("project-memory-update-rejected-read")));
    REQUIRE(unchanged.records.size() == 1U);
    REQUIRE(unchanged.records.front().version == normalized.version);
    REQUIRE(unchanged.records.front().title == "normalized title");
    REQUIRE(unchanged.records.front().summary == "normalized summary");
    const auto status = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-update-rejected-status")));
    REQUIRE(status.eventCount == 2U);
}

void cursorEnvelopeUsesScalarExactByteAccounting()
{
    Support::ScopedTestDirectory directory{L"project-memory-cursor-envelope"};
    const auto projectId = parse<Domain::ProjectId>(
        "20202020-2020-4020-8020-202020202020");
    RepositoryFixture fixture{directory.path(), projectId};
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("envelope first", "first summary")},
        context("project-memory-cursor-envelope-first"))));
    static_cast<void>(take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId, write("envelope second", "second summary")},
        context("project-memory-cursor-envelope-second"))));

    const auto page = take(fixture.repository->listRecent(
        Domain::ListRecentProjectMemoryRequest{
            projectId, {}, std::nullopt, 1U, std::nullopt, false, 64U * 1024U},
        context("project-memory-cursor-envelope-page")));
    REQUIRE(page.records.size() == 1U);
    REQUIRE(page.truncated);
    REQUIRE(page.nextCursor == std::optional<std::string>{"djE6MQ=="});

    const auto& record = page.records.front().record;
    const std::string encodedRecord =
        "{\"confidence\":1.0,\"content_hash\":\"" + record.contentHash.value() +
        "\",\"created_at\":\"2026-01-02T03:00:00Z\",\"expires_at\":null,\"id\":\"" +
        record.id.value() +
        "\",\"importance\":0.5,\"is_tombstone\":false,\"kind\":\"decision\","
        "\"last_accessed_at\":\"2026-01-02T03:00:00Z\",\"project_id\":\"" +
        projectId.value() +
        "\",\"schema_version\":1,\"session_id\":null,"
        "\"source_kind\":\"external_integration\","
        "\"source_reference\":null,\"summary\":\"first summary\",\"tags\":[],"
        "\"title\":\"envelope first\",\"updated_at\":\"2026-01-02T03:00:00Z\","
        "\"version\":1}";
    std::size_t expectedBytes{};
    bool stabilized{};
    for (std::size_t attempt = 0U; attempt < 8U; ++attempt) {
        const std::string expectedEnvelope =
            "{\"capability_version\":1,\"count\":1,\"encoded_bytes\":" +
            std::to_string(expectedBytes) +
            ",\"maximum_response_bytes\":65536,\"next_cursor\":\"djE6MQ==\","
            "\"ok\":true,\"project_id\":\"" + projectId.value() +
            "\",\"records\":[" + encodedRecord +
            "],\"schema_version\":1,\"truncated\":true}";
        const auto measured = expectedEnvelope.size();
        if (measured == expectedBytes) {
            stabilized = true;
            break;
        }
        expectedBytes = measured;
    }
    REQUIRE(stabilized);
    if (page.encodedBytes != expectedBytes) {
        throw std::runtime_error{
            "Exact cursor envelope bytes differ: repository=" +
            std::to_string(page.encodedBytes) + ", independent=" +
            std::to_string(expectedBytes)};
    }
}

void tombstoneHashAvoidsBodylessTwinCollision()
{
    Support::ScopedTestDirectory directory{L"project-memory-tombstone-collision"};
    const auto projectId = parse<Domain::ProjectId>(
        "30303030-3030-4030-8030-303030303030");
    RepositoryFixture fixture{directory.path(), projectId};

    auto bodyfulWrite = write(
        "collision title", "collision summary", "body retained only before forget", {"collision"});
    bodyfulWrite.idempotencyKey = take(Domain::IdempotencyKey::create(
        "bodyful-collision-idempotency"));
    const auto bodyful = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, bodyfulWrite},
        context("project-memory-tombstone-bodyful")));
    const auto bodyless = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{
            projectId,
            write("collision title", "collision summary", std::nullopt, {"collision"})},
        context("project-memory-tombstone-bodyless")));
    REQUIRE(bodyful.contentHash != bodyless.contentHash);

    const auto forgotten = take(fixture.repository->forget(
        Domain::ForgetProjectMemoryRequest{projectId, bodyful.recordId},
        context("project-memory-tombstone-collision-forget")));
    REQUIRE(forgotten.disposition == Domain::ForgetDisposition::Tombstoned);
    const auto retry = take(fixture.repository->remember(
        Domain::RememberProjectMemoryRequest{projectId, bodyfulWrite},
        context("project-memory-tombstone-collision-readable")));
    REQUIRE(retry.disposition == Domain::MemoryWriteDisposition::Deduplicated);
    REQUIRE(retry.recordId == bodyful.recordId);
    REQUIRE(retry.recordVersion == bodyful.recordVersion + 1U);
    REQUIRE(retry.contentHash != bodyful.contentHash);
    REQUIRE(retry.contentHash != bodyless.contentHash);

    const auto forgottenAgain = take(fixture.repository->forget(
        Domain::ForgetProjectMemoryRequest{projectId, bodyful.recordId},
        context("project-memory-tombstone-collision-idempotent")));
    REQUIRE(forgottenAgain.disposition == Domain::ForgetDisposition::NotFound);
    const auto status = take(fixture.repository->status(
        Domain::ProjectMemoryStatusRequest{projectId},
        context("project-memory-tombstone-collision-status")));
    REQUIRE(status.recordCount == 1U);
    REQUIRE(status.tombstoneCount == 1U);
    REQUIRE(status.integrityOk);
}

void eventJournalRetentionIsTransactionallyBounded()
{
    Support::ScopedTestDirectory directory{L"project-memory-event-retention"};
    const auto projectId = parse<Domain::ProjectId>(
        "40404040-4040-4040-8040-404040404040");
    std::optional<Domain::MemoryRecordId> recordId;
    {
        RepositoryFixture fixture{directory.path(), projectId};
        recordId = take(fixture.repository->remember(
            Domain::RememberProjectMemoryRequest{
                projectId, write("retention title", "retention summary")},
            context("project-memory-event-retention-write"))).recordId;
        fixture.repository->close();
    }

    executeDatabaseSql(
        directory.path() / L"memory.sqlite",
        "WITH RECURSIVE sequence(value) AS ("
        "SELECT 1 UNION ALL SELECT value+1 FROM sequence WHERE value<10001) "
        "INSERT INTO event_journal(project_id,record_id,action,detail,created_at) "
        "SELECT '40404040-4040-4040-8040-404040404040',NULL,'synthetic',"
        "printf('%d',value),'2026-01-02T03:00:00Z' FROM sequence;");
    REQUIRE(queryDatabaseInteger(
        directory.path() / L"memory.sqlite",
        "SELECT COUNT(*) FROM event_journal") == 10'002);

    {
        RepositoryFixture reopened{directory.path(), projectId};
        const auto updated = take(reopened.repository->update(
            Domain::UpdateProjectMemoryRequest{
                projectId,
                *recordId,
                1U,
                std::optional<std::string>{"retention updated"},
                std::nullopt,
                std::nullopt,
                std::nullopt},
            context("project-memory-event-retention-prune")));
        REQUIRE(updated.version == 2U);
        const auto status = take(reopened.repository->status(
            Domain::ProjectMemoryStatusRequest{projectId},
            context("project-memory-event-retention-status")));
        REQUIRE(status.eventCount == 10'000U);
        reopened.repository->close();
    }

    REQUIRE(queryDatabaseInteger(
        directory.path() / L"memory.sqlite",
        "SELECT COUNT(*) FROM event_journal") == 10'000);
    REQUIRE(queryDatabaseInteger(
        directory.path() / L"memory.sqlite",
        "SELECT COUNT(*) FROM event_journal WHERE action='inserted'") == 0);
    REQUIRE(queryDatabaseInteger(
        directory.path() / L"memory.sqlite",
        "SELECT COUNT(*) FROM event_journal WHERE action='updated'") == 1);
}

} // namespace

int main()
{
    try {
        crudSearchConflictLinkAndReset();
        std::cout << "PASS project_memory_repository.crud_search_reset\n";
        batchAtomicityBoundsAndPrivateKeyRejection();
        std::cout << "PASS project_memory_repository.atomicity_bounds_redaction\n";
        projectIsolationAndScopeFailure();
        std::cout << "PASS project_memory_repository.isolation_scope\n";
        deterministicRankingPaginationAndRecentOrder();
        std::cout << "PASS project_memory_repository.ranking_pagination_recent\n";
        batchFailureRollsBackPreviouslyInsertedRows();
        std::cout << "PASS project_memory_repository.batch_transaction_rollback\n";
        getPreservesCallerOrderAndDuplicates();
        std::cout << "PASS project_memory_repository.get_order_duplicates\n";
        oversizedFirstSearchRecordFailsExplicitly();
        std::cout << "PASS project_memory_repository.first_row_response_cap\n";
        closeRejectsFurtherRepositoryUse();
        std::cout << "PASS project_memory_repository.post_close\n";
        recordsSurviveRepositoryRestart();
        std::cout << "PASS project_memory_repository.restart_durability\n";
        cancelledAndExpiredOperationsAreRejected();
        std::cout << "PASS project_memory_repository.context_guard\n";
        persistedHostileRowsFailClosed();
        std::cout << "PASS project_memory_repository.persisted_hostile_rows\n";
        incompleteFtsObjectSetIsUnavailable();
        std::cout << "PASS project_memory_repository.incomplete_fts_unavailable\n";
        cursorInt64BoundaryFailsClosed();
        std::cout << "PASS project_memory_repository.cursor_int64_boundary\n";
        updateNormalizesRequiredTextAndRejectsEmptyResults();
        std::cout << "PASS project_memory_repository.update_normalization_empty\n";
        cursorEnvelopeUsesScalarExactByteAccounting();
        std::cout << "PASS project_memory_repository.cursor_scalar_exact_envelope\n";
        tombstoneHashAvoidsBodylessTwinCollision();
        std::cout << "PASS project_memory_repository.tombstone_hash_collision\n";
        eventJournalRetentionIsTransactionallyBounded();
        std::cout << "PASS project_memory_repository.event_journal_retention\n";
        std::cout << "SUMMARY passed=17 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
