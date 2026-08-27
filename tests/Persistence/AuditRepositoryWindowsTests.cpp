#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ForgeConductor/Persistence/Windows/WindowsAuditRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "PersistenceTestSupport.h"

#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Persistence = ForgeConductor::Persistence::Windows;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

using namespace std::chrono_literals;

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    require(!result, "an operation expected to fail succeeded");
    require(result.error().code == expectedCode, "an operation returned the wrong error code");
}

[[nodiscard]] Domain::UtcTimePoint utcTime(
    const int second,
    const int millisecond = 0)
{
    const auto day = std::chrono::sys_days{
        std::chrono::year{2026} / std::chrono::August / 26};
    return Domain::UtcTimePoint{
        day.time_since_epoch() + 12h + std::chrono::seconds{second} +
        std::chrono::milliseconds{millisecond}};
}

[[nodiscard]] Domain::AuditEvent auditEvent(
    const std::string_view tool,
    const Domain::UtcTimePoint timestamp = utcTime(0))
{
    return Domain::AuditEvent{
        timestamp,
        std::nullopt,
        std::string{tool},
        std::nullopt,
        "ok",
        std::nullopt,
        std::nullopt};
}

class SqliteDatabase final {
public:
    explicit SqliteDatabase(const std::filesystem::path& path)
    {
        const int result = ::sqlite3_open16(path.c_str(), &database_);
        if (result != SQLITE_OK || database_ == nullptr) {
            const std::string message = database_ != nullptr
                ? std::string{::sqlite3_errmsg(database_)}
                : std::string{"no database handle"};
            if (database_ != nullptr) {
                static_cast<void>(::sqlite3_close(database_));
                database_ = nullptr;
            }
            throw std::runtime_error{"sqlite open failed: " + message};
        }
    }

    ~SqliteDatabase() noexcept
    {
        if (database_ != nullptr) {
            static_cast<void>(::sqlite3_close(database_));
        }
    }

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    void execute(const std::string_view sql)
    {
        char* message = nullptr;
        const int result = ::sqlite3_exec(
            database_, std::string{sql}.c_str(), nullptr, nullptr, &message);
        std::string ownedMessage;
        if (message != nullptr) {
            ownedMessage = message;
            ::sqlite3_free(message);
        }
        if (result != SQLITE_OK) {
            throw std::runtime_error{
                "sqlite execute failed: " +
                (ownedMessage.empty() ? std::string{::sqlite3_errmsg(database_)}
                                      : ownedMessage)};
        }
    }

    [[nodiscard]] std::int64_t queryInteger(const std::string_view sql) const
    {
        sqlite3_stmt* statement = nullptr;
        const std::string ownedSql{sql};
        const int prepared = ::sqlite3_prepare_v2(
            database_, ownedSql.c_str(), -1, &statement, nullptr);
        if (prepared != SQLITE_OK || statement == nullptr) {
            throw std::runtime_error{
                "sqlite integer query prepare failed: " +
                std::string{::sqlite3_errmsg(database_)}};
        }
        const int stepped = ::sqlite3_step(statement);
        if (stepped != SQLITE_ROW) {
            static_cast<void>(::sqlite3_finalize(statement));
            throw std::runtime_error{
                "sqlite integer query returned no row: " +
                std::string{::sqlite3_errmsg(database_)}};
        }
        const auto value = ::sqlite3_column_int64(statement, 0);
        const int completed = ::sqlite3_step(statement);
        const int finalized = ::sqlite3_finalize(statement);
        if (completed != SQLITE_DONE || finalized != SQLITE_OK) {
            throw std::runtime_error{"sqlite integer query did not finish cleanly"};
        }
        return value;
    }

    [[nodiscard]] std::optional<std::string> queryText(
        const std::string_view sql) const
    {
        sqlite3_stmt* statement = nullptr;
        const std::string ownedSql{sql};
        const int prepared = ::sqlite3_prepare_v2(
            database_, ownedSql.c_str(), -1, &statement, nullptr);
        if (prepared != SQLITE_OK || statement == nullptr) {
            throw std::runtime_error{
                "sqlite text query prepare failed: " +
                std::string{::sqlite3_errmsg(database_)}};
        }
        const int stepped = ::sqlite3_step(statement);
        if (stepped != SQLITE_ROW) {
            static_cast<void>(::sqlite3_finalize(statement));
            throw std::runtime_error{
                "sqlite text query returned no row: " +
                std::string{::sqlite3_errmsg(database_)}};
        }
        std::optional<std::string> value;
        if (::sqlite3_column_type(statement, 0) != SQLITE_NULL) {
            const auto* text = ::sqlite3_column_text(statement, 0);
            const int bytes = ::sqlite3_column_bytes(statement, 0);
            if (text == nullptr || bytes < 0) {
                static_cast<void>(::sqlite3_finalize(statement));
                throw std::runtime_error{"sqlite text query returned invalid text"};
            }
            value.emplace(
                reinterpret_cast<const char*>(text),
                static_cast<std::size_t>(bytes));
        }
        const int completed = ::sqlite3_step(statement);
        const int finalized = ::sqlite3_finalize(statement);
        if (completed != SQLITE_DONE || finalized != SQLITE_OK) {
            throw std::runtime_error{"sqlite text query did not finish cleanly"};
        }
        return value;
    }

private:
    sqlite3* database_{};
};

struct Fixture final {
    explicit Fixture(const std::wstring_view label)
        : directory{label},
          clock{std::make_shared<Support::FixedClock>(
              utcTime(0), std::chrono::steady_clock::now())},
          paths{std::make_shared<Fakes::RecordingApplicationPathsFake>()},
          diagnostics{std::make_shared<Fakes::RuntimeDiagnosticsFake>(
              clock->monotonicNow())}
    {
        paths->setNow(clock->monotonicNow());
        paths->dataRootResult.set(Domain::Result<Domain::PathText>::success(
            Support::pathText(directory.path())));
        open();
    }

    ~Fixture() noexcept
    {
        try {
            if (repository) {
                repository->close();
            }
            if (database) {
                static_cast<void>(database->close(
                    Support::activeContext("audit-fixture-destructor")));
            }
        } catch (...) {
        }
    }

    void open()
    {
        auto opened = take(Persistence::WindowsCentralDatabase::open(
            paths,
            diagnostics,
            clock,
            Support::activeContext("audit-database-open")));
        database = std::shared_ptr<Persistence::WindowsCentralDatabase>{
            std::move(opened)};
        repository = take(Persistence::WindowsAuditRepository::attach(database));
    }

    void closeDatabase()
    {
        if (repository) {
            repository->close();
        }
        if (database) {
            take(database->close(
                Support::activeContext("audit-database-close")));
        }
        repository.reset();
        database.reset();
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return directory.path() / L"store.sqlite";
    }

    Support::ScopedTestDirectory directory;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<Persistence::WindowsCentralDatabase> database;
    std::shared_ptr<Persistence::WindowsAuditRepository> repository;
};

void attachmentRoundTripAndPrivacyAreProductionBounded()
{
    requireError(
        Persistence::WindowsAuditRepository::attach({}),
        Domain::ErrorCodes::InvalidRequest);

    Fixture fixture{L"audit-round-trip"};
    const auto client = take(Domain::ClientId::parse("mcp-primary-client"));
    const auto digest = take(Domain::Sha256Digest::parse(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    const Domain::AuditEvent first{
        utcTime(0, 123),
        client,
        "project_memory.search",
        digest,
        "ok",
        42ms,
        std::nullopt};
    const Domain::AuditEvent second{
        utcTime(1, 456),
        std::nullopt,
        "write_file",
        std::nullopt,
        "denied",
        7ms,
        std::string{Domain::ErrorCodes::Unauthorized}};
    take(fixture.repository->append(
        first, Support::activeContext("audit-append-first")));
    take(fixture.repository->append(
        second, Support::activeContext("audit-append-second")));

    const auto recent = take(fixture.repository->recent(
        2U, Support::activeContext("audit-recent")));
    require(recent.size() == 2U, "audit round-trip returned the wrong row count");
    require(recent[0].timestamp == second.timestamp &&
                recent[0].tool == second.tool &&
                recent[0].status == second.status &&
                recent[0].duration == second.duration &&
                recent[0].error == second.error &&
                !recent[0].clientId && !recent[0].argumentsDigest,
            "newest audit row did not round-trip");
    require(recent[1].timestamp == first.timestamp &&
                recent[1].clientId == first.clientId &&
                recent[1].tool == first.tool &&
                recent[1].argumentsDigest == first.argumentsDigest &&
                recent[1].status == first.status &&
                recent[1].duration == first.duration &&
                !recent[1].error,
            "older audit row did not round-trip");
    require(take(fixture.repository->recent(
                    0U, Support::activeContext("audit-recent-empty")))
                .empty(),
            "zero-count audit read was not empty");

    fixture.repository->close();
    requireError(
        fixture.repository->append(
            first, Support::activeContext("audit-append-after-close")),
        Domain::ErrorCodes::InvalidRequest);
    take(fixture.database->quickCheck(
        Support::activeContext("audit-shared-database-survives-repository-close")));
    fixture.closeDatabase();

    SqliteDatabase database{fixture.databasePath()};
    require(database.queryInteger("SELECT COUNT(*) FROM audit_events") == 2,
            "durable audit row count changed");
    require(database.queryInteger(
                "SELECT COUNT(*) FROM audit_events WHERE "
                "args_json IS NOT NULL OR arguments_json IS NOT NULL") == 0,
            "audit persistence retained raw argument JSON");
    require(database.queryInteger(
                "SELECT COUNT(*) FROM audit_events WHERE "
                "event_id IS NOT NULL OR mutating IS NOT NULL") == 0,
            "audit persistence fabricated unavailable metadata");
    require(database.queryInteger(
                "SELECT COUNT(*) FROM audit_events WHERE timestamp=occurred_at") == 2,
            "legacy and current audit timestamps diverged");
    require(database.queryText(
                "SELECT occurred_at FROM audit_events ORDER BY id DESC LIMIT 1") ==
                std::optional<std::string>{"2026-08-26T12:00:01.456Z"},
            "audit timestamp precision was not preserved");
    require(database.queryText(
                "SELECT error FROM audit_events ORDER BY id DESC LIMIT 1") ==
                std::optional<std::string>{"unauthorized"} &&
                database.queryText(
                    "SELECT error_code FROM audit_events ORDER BY id DESC LIMIT 1") ==
                    std::optional<std::string>{"unauthorized"},
            "legacy and current audit error codes diverged");
}

void invalidInputContextsAndReadBoundsFailClosed()
{
    Fixture fixture{L"audit-invalid"};
    auto invalidStatus = auditEvent("read_file");
    invalidStatus.status.clear();
    requireError(
        fixture.repository->append(
            invalidStatus, Support::activeContext("audit-invalid-status")),
        Domain::ErrorCodes::InvalidRequest);

    auto oversizedTool = auditEvent(std::string(129U, 't'));
    requireError(
        fixture.repository->append(
            oversizedTool, Support::activeContext("audit-oversized-tool")),
        Domain::ErrorCodes::PayloadTooLarge);

    auto oversizedError = auditEvent("read_file");
    oversizedError.error = std::string(4U * 1024U + 1U, 'e');
    requireError(
        fixture.repository->append(
            oversizedError, Support::activeContext("audit-oversized-error")),
        Domain::ErrorCodes::PayloadTooLarge);

    auto negativeDuration = auditEvent("read_file");
    negativeDuration.duration = -1ms;
    requireError(
        fixture.repository->append(
            negativeDuration, Support::activeContext("audit-negative-duration")),
        Domain::ErrorCodes::InvalidRequest);

    auto beforeEpoch = auditEvent(
        "read_file", Domain::UtcTimePoint{} - 1ms);
    requireError(
        fixture.repository->append(
            beforeEpoch, Support::activeContext("audit-before-epoch")),
        Domain::ErrorCodes::InvalidRequest);

    const auto valid = auditEvent("read_file");
    auto expired = Support::activeContext("audit-expired");
    expired.deadline = std::chrono::steady_clock::now() - 1ms;
    requireError(
        fixture.repository->append(valid, expired),
        Domain::ErrorCodes::DeadlineExceeded);

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = Support::activeContext("audit-cancelled");
    cancelled.cancellation = cancellation.get_token();
    requireError(
        fixture.repository->append(valid, cancelled),
        Domain::ErrorCodes::Cancelled);

    requireError(
        fixture.repository->recent(
            Persistence::WindowsAuditRepository::MaximumRecentEvents + 1U,
            Support::activeContext("audit-read-over-bound")),
        Domain::ErrorCodes::LimitExceeded);
    require(take(fixture.repository->recent(
                    Persistence::WindowsAuditRepository::MaximumRecentEvents,
                    Support::activeContext("audit-read-at-bound")))
                .empty(),
            "failed audit appends changed durable state");
}

void retentionPruneIsAtomicAndBounded()
{
    Fixture fixture{L"audit-retention"};
    fixture.closeDatabase();
    {
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "BEGIN IMMEDIATE;"
            "WITH RECURSIVE sequence(value) AS ("
            "SELECT 1 UNION ALL SELECT value+1 FROM sequence WHERE value<10001) "
            "INSERT INTO audit_events(timestamp,tool,status,occurred_at) "
            "SELECT '2026-08-26T12:00:00Z','seed','ok',"
            "'2026-08-26T12:00:00Z' FROM sequence;"
            "COMMIT;");
        require(database.queryInteger("SELECT COUNT(*) FROM audit_events") == 10'001,
                "audit retention fixture did not seed all rows");
    }

    fixture.open();
    take(fixture.repository->append(
        auditEvent("retention_boundary", utcTime(2)),
        Support::activeContext("audit-retention-append")));
    fixture.closeDatabase();

    SqliteDatabase database{fixture.databasePath()};
    require(database.queryInteger("SELECT COUNT(*) FROM audit_events") ==
                static_cast<std::int64_t>(
                    Persistence::WindowsAuditRepository::MaximumRetainedEvents),
            "audit retention did not enforce the durable row cap");
    require(database.queryInteger("SELECT MIN(id) FROM audit_events") == 3,
            "audit retention pruned the wrong primary-key range");
    require(database.queryText(
                "SELECT tool FROM audit_events ORDER BY id DESC LIMIT 1") ==
                std::optional<std::string>{"retention_boundary"},
            "audit retention pruned the newly committed event");
}

void hostilePersistedRowsFailClosed()
{
    Fixture fixture{L"audit-hostile-row"};
    fixture.closeDatabase();
    {
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "INSERT INTO audit_events("
            "timestamp,tool,args_digest,status,occurred_at) VALUES("
            "'2026-08-26T12:00:00Z','hostile','not-a-digest','ok',"
            "'2026-08-26T12:00:00Z')");
    }
    fixture.open();
    requireError(
        fixture.repository->recent(
            1U, Support::activeContext("audit-hostile-row-read")),
        Domain::ErrorCodes::IntegrityFailure);
}

struct TestCase final {
    const char* name;
    void (*run)();
};

} // namespace

int wmain()
{
    const std::array<TestCase, 4U> tests{{
        {"attachment round-trip and privacy are production bounded",
         attachmentRoundTripAndPrivacyAreProductionBounded},
        {"invalid input contexts and read bounds fail closed",
         invalidInputContextsAndReadBoundsFailClosed},
        {"retention prune is atomic and bounded",
         retentionPruneIsAtomicAndBounded},
        {"hostile persisted rows fail closed", hostilePersistedRowsFailClosed},
    }};

    std::size_t passed{};
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            return 1;
        }
    }
    std::cout << passed << '/' << tests.size()
              << " audit repository tests passed.\n";
    return 0;
}
