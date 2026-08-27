#include "ForgeConductor/Persistence/Windows/WindowsForgeStatusRepository.h"
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
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

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
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    require(!result, "an operation expected to fail succeeded");
    require(
        result.error().code == expectedCode,
        "an operation returned the wrong error code");
}

class SqliteDatabase final {
public:
    explicit SqliteDatabase(const std::filesystem::path& path)
    {
        const int opened = ::sqlite3_open16(path.c_str(), &database_);
        if (opened != SQLITE_OK || database_ == nullptr) {
            throw std::runtime_error{"sqlite fixture open failed"};
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
                "sqlite fixture execute failed: " +
                (ownedMessage.empty()
                     ? std::string{::sqlite3_errmsg(database_)}
                     : ownedMessage)};
        }
    }

private:
    sqlite3* database_{};
};

struct Fixture final {
    explicit Fixture(const std::wstring_view label)
        : directory{label},
          clock{std::make_shared<Support::FixedClock>(
              Domain::UtcTimePoint{}, std::chrono::steady_clock::now())},
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
            close();
        } catch (...) {
        }
    }

    void open()
    {
        auto opened = take(Persistence::WindowsCentralDatabase::open(
            paths,
            diagnostics,
            clock,
            Support::activeContext("forge-status-database-open")));
        database = std::shared_ptr<Persistence::WindowsCentralDatabase>{
            std::move(opened)};
        repository = take(
            Persistence::WindowsForgeStatusRepository::attach(database));
    }

    void close()
    {
        if (repository) {
            repository->close();
        }
        if (database) {
            take(database->close(
                Support::activeContext("forge-status-database-close")));
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
    std::shared_ptr<Persistence::WindowsForgeStatusRepository> repository;
};

void statusProjectionUsesOnlyCanonicalBoundedRows()
{
    requireError(
        Persistence::WindowsForgeStatusRepository::attach({}),
        Domain::ErrorCodes::InvalidRequest);

    Fixture fixture{L"forge-status-projection"};
    const auto empty = take(fixture.repository->snapshot(
        Support::activeContext("forge-status-empty")));
    require(empty.presenceCount == 0U, "empty presence count was not zero");
    require(empty.openSessionIds.empty(), "empty session projection was not empty");

    fixture.close();
    {
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "INSERT INTO client_presence("
            "client_id,role,deployment_id,process_id,first_seen_at,last_seen_at) "
            "VALUES('primary-client','primary','deploy-primary',101,"
            "'2026-08-27T12:00:00Z','2026-08-27T12:00:01Z'),"
            "('fallback-client','fallback','deploy-fallback',102,"
            "'2026-08-27T12:00:00Z','2026-08-27T12:00:01Z');"
            "INSERT INTO presence(client_id,host_kind,pid,cwd,last_heartbeat) "
            "VALUES('legacy-only','legacy',103,'D:/legacy',"
            "'2026-08-27T12:00:01Z');"
            "INSERT INTO agent_sessions("
            "id,agent_id,status,created_at,updated_at) VALUES"
            "('11111111-1111-4111-8111-111111111111','explore','open',"
            "'2026-08-27T12:00:03Z','2026-08-27T12:00:03Z'),"
            "('33333333-3333-4333-8333-333333333333','explore','active',"
            "'2026-08-27T12:00:02Z','2026-08-27T12:00:02Z'),"
            "('22222222-2222-4222-8222-222222222222','explore','running',"
            "'2026-08-27T12:00:02Z','2026-08-27T12:00:02Z'),"
            "('44444444-4444-4444-8444-444444444444','explore','started',"
            "'2026-08-27T12:00:01Z','2026-08-27T12:00:01Z'),"
            "('ffffffff-ffff-4fff-8fff-ffffffffffff','explore','closed',"
            "'2026-08-27T12:00:04Z','2026-08-27T12:00:04Z'),"
            "('eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee','explore','hostile',"
            "'2026-08-27T12:00:05Z','2026-08-27T12:00:05Z');");
    }
    fixture.open();

    const auto projected = take(fixture.repository->snapshot(
        Support::activeContext("forge-status-projected")));
    require(
        projected.presenceCount == 2U,
        "legacy presence was counted or client presence was omitted");
    require(projected.openSessionIds.size() == 4U, "open-session filter changed");
    require(
        projected.openSessionIds[0].value() ==
            "11111111-1111-4111-8111-111111111111" &&
        projected.openSessionIds[1].value() ==
            "33333333-3333-4333-8333-333333333333" &&
        projected.openSessionIds[2].value() ==
            "22222222-2222-4222-8222-222222222222" &&
        projected.openSessionIds[3].value() ==
            "44444444-4444-4444-8444-444444444444",
        "open sessions were not deterministically ordered");
}

void maximumPlusOneFailsWithoutTruncation()
{
    Fixture fixture{L"forge-status-bound"};
    fixture.close();
    {
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "WITH RECURSIVE sequence(value) AS ("
            "SELECT 1 UNION ALL SELECT value+1 FROM sequence WHERE value<10000) "
            "INSERT INTO agent_sessions(id,agent_id,status,created_at,updated_at) "
            "SELECT printf('00000000-0000-4000-8000-%012x',value),"
            "'explore','open','2026-08-27T12:00:00Z',"
            "'2026-08-27T12:00:00Z' FROM sequence;");
    }
    fixture.open();
    const auto atBound = take(fixture.repository->snapshot(
        Support::activeContext("forge-status-at-bound")));
    require(
        atBound.openSessionIds.size() ==
            Domain::ForgeStatusLimits::MaximumOpenSessionIds,
        "the exact open-session bound was not returned");

    fixture.close();
    {
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "INSERT INTO agent_sessions("
            "id,agent_id,status,created_at,updated_at) VALUES("
            "'ffffffff-ffff-4fff-8fff-ffffffffffff','explore','open',"
            "'2026-08-27T12:00:01Z','2026-08-27T12:00:01Z');");
    }
    fixture.open();
    requireError(
        fixture.repository->snapshot(
            Support::activeContext("forge-status-over-bound")),
        Domain::ErrorCodes::LimitExceeded);
}

void hostileRowsContextsAndCloseFailTyped()
{
    Fixture fixture{L"forge-status-hostile"};

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = Support::activeContext("forge-status-cancelled");
    cancelled.cancellation = cancellation.get_token();
    requireError(
        fixture.repository->snapshot(cancelled),
        Domain::ErrorCodes::Cancelled);

    auto expired = Support::activeContext("forge-status-expired");
    expired.deadline = std::chrono::steady_clock::now() - 1ms;
    requireError(
        fixture.repository->snapshot(expired),
        Domain::ErrorCodes::DeadlineExceeded);

    fixture.close();
    {
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "INSERT INTO agent_sessions("
            "id,agent_id,status,created_at,updated_at) VALUES("
            "'not-a-uuid','explore','open','2026-08-27T12:00:00Z',"
            "'2026-08-27T12:00:00Z');");
    }
    fixture.open();
    requireError(
        fixture.repository->snapshot(
            Support::activeContext("forge-status-hostile-row")),
        Domain::ErrorCodes::IntegrityFailure);

    auto closedRepository = fixture.repository;
    closedRepository->close();
    requireError(
        closedRepository->snapshot(
            Support::activeContext("forge-status-after-close")),
        Domain::ErrorCodes::InvalidRequest);
    take(fixture.database->quickCheck(
        Support::activeContext("forge-status-shared-database-open")));
}

struct TestCase final {
    const char* name;
    void (*run)();
};

} // namespace

int wmain()
{
    const std::array<TestCase, 3U> tests{{
        {"status projection uses only canonical bounded rows",
         statusProjectionUsesOnlyCanonicalBoundedRows},
        {"maximum plus one fails without truncation",
         maximumPlusOneFailsWithoutTruncation},
        {"hostile rows contexts and close fail typed",
         hostileRowsContextsAndCloseFailTyped},
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
              << " Forge status repository tests passed.\n";
    return 0;
}
