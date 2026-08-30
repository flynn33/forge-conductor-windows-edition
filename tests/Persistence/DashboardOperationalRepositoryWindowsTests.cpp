#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsClientPresenceRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsDashboardOperationalRepository.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "PersistenceTestSupport.h"

#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <cstddef>
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

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::UtcTimePoint atSeconds(const std::int64_t seconds)
{
    return Domain::UtcTimePoint{std::chrono::seconds{seconds}};
}

[[nodiscard]] Domain::PathText workingDirectory(const std::string_view value)
{
    return take(Domain::PathText::create(value));
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
        char* error{};
        const auto source = std::string{sql};
        const int status = ::sqlite3_exec(
            database_, source.c_str(), nullptr, nullptr, &error);
        if (status != SQLITE_OK) {
            const std::string message = error != nullptr
                ? error
                : "unknown sqlite fixture error";
            if (error != nullptr) {
                ::sqlite3_free(error);
            }
            throw std::runtime_error{"sqlite fixture mutation failed: " + message};
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
        auto opened = take(Persistence::WindowsCentralDatabase::open(
            paths,
            diagnostics,
            clock,
            Support::activeContext("dashboard-operational-database-open")));
        database = std::shared_ptr<Persistence::WindowsCentralDatabase>{
            std::move(opened)};
        sessions = take(
            Persistence::WindowsAgentSessionRepository::attach(database, clock));
        presence = take(
            Persistence::WindowsClientPresenceRepository::attach(database));
        operational = take(
            Persistence::WindowsDashboardOperationalRepository::attach(database));
    }

    ~Fixture() noexcept
    {
        try {
            if (operational) {
                operational->close();
            }
            if (presence) {
                presence->close();
            }
            if (sessions) {
                sessions->close();
            }
            if (database) {
                static_cast<void>(database->close(Support::activeContext(
                    "dashboard-operational-database-close")));
            }
        } catch (...) {
        }
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return directory.path() / L"store.sqlite";
    }

    void saveSession(const Domain::AgentSession& session)
    {
        take(sessions->save(
            session,
            Support::activeContext("dashboard-operational-save-session")));
    }

    void savePresence(
        const std::string_view clientId,
        std::string role,
        const std::string_view deploymentId,
        const std::uint32_t processId,
        const std::string_view directoryValue,
        const Domain::UtcTimePoint firstSeenAt,
        const Domain::UtcTimePoint lastSeenAt)
    {
        take(presence->upsert(
            Domain::ClientPresenceRegistration{
                Domain::ClientPresenceIdentity{
                    parse<Domain::ClientId>(clientId),
                    std::move(role),
                    parse<Domain::DeploymentId>(deploymentId),
                    processId},
                workingDirectory(directoryValue),
                firstSeenAt,
                lastSeenAt},
            Support::activeContext("dashboard-operational-save-presence")));
    }

    void closeWriters() noexcept
    {
        if (presence) {
            presence->close();
        }
        if (sessions) {
            sessions->close();
        }
    }

    Support::ScopedTestDirectory directory;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<Persistence::WindowsCentralDatabase> database;
    std::shared_ptr<Persistence::WindowsAgentSessionRepository> sessions;
    std::shared_ptr<Persistence::WindowsClientPresenceRepository> presence;
    std::shared_ptr<Persistence::WindowsDashboardOperationalRepository>
        operational;
};

[[nodiscard]] Domain::AgentSession makeSession(
    const std::string_view sessionId,
    const std::string_view agentId,
    const std::optional<std::string_view> clientId,
    const Domain::SessionStatus status,
    std::optional<std::string> summary,
    const std::int64_t createdAtSeconds,
    const std::int64_t updatedAtSeconds)
{
    std::optional<Domain::ClientId> client;
    if (clientId) {
        client.emplace(parse<Domain::ClientId>(*clientId));
    }
    return Domain::AgentSession{
        parse<Domain::SessionId>(sessionId),
        parse<Domain::AgentId>(agentId),
        std::move(client),
        status,
        std::move(summary),
        atSeconds(createdAtSeconds),
        atSeconds(updatedAtSeconds)};
}

void requireSessionEquals(
    const Domain::AgentSession& actual,
    const Domain::AgentSession& expected)
{
    require(
        actual.id == expected.id &&
            actual.agentId == expected.agentId &&
            actual.clientId == expected.clientId &&
            actual.status == expected.status &&
            actual.summary == expected.summary &&
            actual.createdAt == expected.createdAt &&
            actual.updatedAt == expected.updatedAt,
        "dashboard session projection changed canonical content");
}

void canonicalSnapshotHasDeterministicOrderingAndContent()
{
    Fixture fixture{L"dashboard-operational-canonical"};
    const auto oldestOpen = makeSession(
        "51000000-0000-4000-8000-000000000001",
        "implement",
        "client-session-old",
        Domain::SessionStatus::Open,
        std::nullopt,
        1'700'000'100,
        1'700'000'110);
    const auto middleClosed = makeSession(
        "51000000-0000-4000-8000-000000000002",
        "review",
        std::nullopt,
        Domain::SessionStatus::Closed,
        std::optional<std::string>{"review complete"},
        1'700'000'200,
        1'700'000'210);
    const auto tiedNewestRunning = makeSession(
        "51000000-0000-4000-8000-000000000003",
        "test",
        "client-session-new",
        Domain::SessionStatus::Running,
        std::nullopt,
        1'700'000'300,
        1'700'000'310);
    const auto tiedNewestActive = makeSession(
        "51000000-0000-4000-8000-000000000004",
        "research",
        "client-session-newest",
        Domain::SessionStatus::Active,
        std::nullopt,
        1'700'000'300,
        1'700'000'320);
    fixture.saveSession(middleClosed);
    fixture.saveSession(oldestOpen);
    fixture.saveSession(tiedNewestRunning);
    fixture.saveSession(tiedNewestActive);

    fixture.savePresence(
        "presence-old",
        "fallback",
        "deployment-old",
        101U,
        "D:\\workspaces\\old",
        atSeconds(1'700'000'100),
        atSeconds(1'700'000'400));
    fixture.savePresence(
        "presence-new-a",
        "primary",
        "deployment-new-a",
        202U,
        "D:\\workspaces\\new-a",
        atSeconds(1'700'000'200),
        atSeconds(1'700'000'500));
    fixture.savePresence(
        "presence-new-z",
        "manager",
        "deployment-new-z",
        303U,
        "D:\\workspaces\\new-z",
        atSeconds(1'700'000'300),
        atSeconds(1'700'000'500));

    const auto snapshot = take(fixture.operational->snapshot(
        3U,
        4U,
        3U,
        Support::activeContext("dashboard-operational-canonical-snapshot")));
    require(snapshot.openSessions.size() == 3U,
            "canonical snapshot returned the wrong open-session count");
    requireSessionEquals(snapshot.openSessions[0], tiedNewestActive);
    requireSessionEquals(snapshot.openSessions[1], tiedNewestRunning);
    requireSessionEquals(snapshot.openSessions[2], oldestOpen);
    require(snapshot.recentSessions.size() == 4U,
            "canonical snapshot returned the wrong recent-session count");
    requireSessionEquals(snapshot.recentSessions[0], tiedNewestActive);
    requireSessionEquals(snapshot.recentSessions[1], tiedNewestRunning);
    requireSessionEquals(snapshot.recentSessions[2], middleClosed);
    requireSessionEquals(snapshot.recentSessions[3], oldestOpen);

    require(snapshot.presence.size() == 3U,
            "canonical snapshot returned the wrong presence count");
    const auto& newestPresence = snapshot.presence[0];
    require(
        newestPresence.clientId == "presence-new-z" &&
            newestPresence.hostKind == "manager" &&
            newestPresence.processId == 303U &&
            newestPresence.workingDirectory ==
                workingDirectory("D:\\workspaces\\new-z") &&
            newestPresence.lastHeartbeat == atSeconds(1'700'000'500),
        "canonical snapshot changed the newest presence content");
    const auto& tiedPresence = snapshot.presence[1];
    require(
        tiedPresence.clientId == "presence-new-a" &&
            tiedPresence.hostKind == "primary" &&
            tiedPresence.processId == 202U &&
            tiedPresence.workingDirectory ==
                workingDirectory("D:\\workspaces\\new-a") &&
            tiedPresence.lastHeartbeat == atSeconds(1'700'000'500),
        "canonical snapshot changed the tied presence content");
    const auto& oldestPresence = snapshot.presence[2];
    require(
        oldestPresence.clientId == "presence-old" &&
            oldestPresence.hostKind == "fallback" &&
            oldestPresence.processId == 101U &&
            oldestPresence.workingDirectory ==
                workingDirectory("D:\\workspaces\\old") &&
            oldestPresence.lastHeartbeat == atSeconds(1'700'000'400),
        "canonical snapshot changed the oldest presence content");
}

void completeCollectionsFailRatherThanTruncate()
{
    Fixture fixture{L"dashboard-operational-complete-bounds"};
    fixture.saveSession(makeSession(
        "52000000-0000-4000-8000-000000000001",
        "implement",
        "open-client-one",
        Domain::SessionStatus::Open,
        std::nullopt,
        1'700'001'000,
        1'700'001'000));
    fixture.saveSession(makeSession(
        "52000000-0000-4000-8000-000000000002",
        "test",
        "open-client-two",
        Domain::SessionStatus::Active,
        std::nullopt,
        1'700'001'001,
        1'700'001'001));
    fixture.savePresence(
        "bounded-presence-one",
        "primary",
        "bounded-deployment-one",
        301U,
        "D:\\bounded\\one",
        atSeconds(1'700'001'000),
        atSeconds(1'700'001'000));
    fixture.savePresence(
        "bounded-presence-two",
        "secondary",
        "bounded-deployment-two",
        302U,
        "D:\\bounded\\two",
        atSeconds(1'700'001'001),
        atSeconds(1'700'001'001));

    requireError(
        fixture.operational->snapshot(
            1U,
            8U,
            2U,
            Support::activeContext("dashboard-operational-open-overflow")),
        Domain::ErrorCodes::LimitExceeded);
    requireError(
        fixture.operational->snapshot(
            2U,
            8U,
            1U,
            Support::activeContext("dashboard-operational-presence-overflow")),
        Domain::ErrorCodes::LimitExceeded);
}

void recentSessionsAreIntentionallyTruncated()
{
    Fixture fixture{L"dashboard-operational-recent-limit"};
    const std::array sessions{
        makeSession(
            "53000000-0000-4000-8000-000000000001",
            "implement",
            std::nullopt,
            Domain::SessionStatus::Closed,
            std::optional<std::string>{"oldest"},
            1'700'002'000,
            1'700'002'000),
        makeSession(
            "53000000-0000-4000-8000-000000000002",
            "review",
            std::nullopt,
            Domain::SessionStatus::Completed,
            std::optional<std::string>{"middle"},
            1'700'002'001,
            1'700'002'001),
        makeSession(
            "53000000-0000-4000-8000-000000000003",
            "test",
            std::nullopt,
            Domain::SessionStatus::Failed,
            std::optional<std::string>{"newest"},
            1'700'002'002,
            1'700'002'002)};
    for (const auto& session : sessions) {
        fixture.saveSession(session);
    }

    const auto snapshot = take(fixture.operational->snapshot(
        1U,
        2U,
        1U,
        Support::activeContext("dashboard-operational-recent-truncation")));
    require(snapshot.openSessions.empty(),
            "recent-only fixture unexpectedly returned an open session");
    require(snapshot.recentSessions.size() == 2U,
            "recent projection did not honor its intentional limit");
    requireSessionEquals(snapshot.recentSessions[0], sessions[2]);
    requireSessionEquals(snapshot.recentSessions[1], sessions[1]);
}

void aggregateTextBudgetsFailBeforeRetainingUnboundedRows()
{
    static_assert(
        Persistence::WindowsDashboardOperationalLimits::
            MaximumSessionTextBytes == 1024U * 1024U);
    static_assert(
        Persistence::WindowsDashboardOperationalLimits::
            MaximumPresenceTextBytes == 512U * 1024U);

    {
        Fixture fixture{L"dashboard-operational-session-text-bound"};
        fixture.closeWriters();
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "WITH RECURSIVE sequence(value) AS ("
            "SELECT 1 UNION ALL SELECT value+1 FROM sequence WHERE value<66) "
            "INSERT INTO agent_sessions("
            "id,agent_id,status,summary,created_at,updated_at) "
            "SELECT printf('55000000-0000-4000-8000-%012x',value),"
            "'implement','open',lower(hex(zeroblob(8000))),"
            "'2026-08-30T12:00:00.000Z','2026-08-30T12:00:00.000Z' "
            "FROM sequence;");
        requireError(
            fixture.operational->snapshot(
                100U,
                1U,
                1U,
                Support::activeContext(
                    "dashboard-operational-session-text-overflow")),
            Domain::ErrorCodes::LimitExceeded);
    }

    {
        Fixture fixture{L"dashboard-operational-presence-text-bound"};
        fixture.closeWriters();
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "WITH RECURSIVE sequence(value) AS ("
            "SELECT 1 UNION ALL SELECT value+1 FROM sequence WHERE value<17) "
            "INSERT INTO client_presence("
            "client_id,role,process_id,working_directory,"
            "first_seen_at,last_seen_at) "
            "SELECT printf('presence-text-%02d',value),'manager',value,"
            "lower(hex(zeroblob(16384))),"
            "'2026-08-30T12:00:00.000Z','2026-08-30T12:00:00.000Z' "
            "FROM sequence;");
        requireError(
            fixture.operational->snapshot(
                1U,
                1U,
                20U,
                Support::activeContext(
                    "dashboard-operational-presence-text-overflow")),
            Domain::ErrorCodes::LimitExceeded);
    }
}

void hostilePersistedRowsFailIntegrityChecks()
{
    {
        Fixture fixture{L"dashboard-operational-hostile-session"};
        const auto session = makeSession(
            "54000000-0000-4000-8000-000000000001",
            "implement",
            "hostile-session-client",
            Domain::SessionStatus::Open,
            std::nullopt,
            1'700'003'000,
            1'700'003'000);
        fixture.saveSession(session);
        fixture.closeWriters();
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "UPDATE agent_sessions SET status='hostile' WHERE id='" +
            session.id.value() + "';");
        requireError(
            fixture.operational->snapshot(
                4U,
                4U,
                4U,
                Support::activeContext(
                    "dashboard-operational-hostile-session-snapshot")),
            Domain::ErrorCodes::IntegrityFailure);
    }

    {
        Fixture fixture{L"dashboard-operational-hostile-presence"};
        fixture.savePresence(
            "hostile-presence-client",
            "primary",
            "hostile-presence-deployment",
            401U,
            "D:\\hostile\\presence",
            atSeconds(1'700'003'100),
            atSeconds(1'700'003'100));
        fixture.closeWriters();
        SqliteDatabase database{fixture.databasePath()};
        database.execute(
            "UPDATE client_presence SET working_directory='',"
            "last_seen_at='not-a-timestamp' "
            "WHERE client_id='hostile-presence-client';");
        requireError(
            fixture.operational->snapshot(
                4U,
                4U,
                4U,
                Support::activeContext(
                    "dashboard-operational-hostile-presence-snapshot")),
            Domain::ErrorCodes::IntegrityFailure);
    }
}

void contextsAttachAndCloseFailTypedWithoutClosingSharedDatabase()
{
    requireError(
        Persistence::WindowsDashboardOperationalRepository::attach({}),
        Domain::ErrorCodes::InvalidRequest);

    Fixture fixture{L"dashboard-operational-lifecycle"};
    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = Support::activeContext(
        "dashboard-operational-cancelled");
    cancelled.cancellation = cancellation.get_token();
    requireError(
        fixture.operational->snapshot(1U, 1U, 1U, cancelled),
        Domain::ErrorCodes::Cancelled);

    auto expired = Support::activeContext("dashboard-operational-expired");
    expired.deadline = std::chrono::steady_clock::now() - 1ms;
    requireError(
        fixture.operational->snapshot(1U, 1U, 1U, expired),
        Domain::ErrorCodes::DeadlineExceeded);

    fixture.operational->close();
    fixture.operational->close();
    requireError(
        fixture.operational->snapshot(
            1U,
            1U,
            1U,
            Support::activeContext("dashboard-operational-after-close")),
        Domain::ErrorCodes::InvalidRequest);
    take(fixture.database->quickCheck(
        Support::activeContext("dashboard-operational-shared-database-open")));
}

struct TestCase final {
    const char* name;
    void (*run)();
};

} // namespace

int wmain()
{
    const std::array<TestCase, 6U> tests{{
        {"canonical snapshot has deterministic ordering and content",
         canonicalSnapshotHasDeterministicOrderingAndContent},
        {"complete collections fail rather than truncate",
         completeCollectionsFailRatherThanTruncate},
        {"recent sessions are intentionally truncated",
         recentSessionsAreIntentionallyTruncated},
        {"aggregate text budgets fail before retaining unbounded rows",
         aggregateTextBudgetsFailBeforeRetainingUnboundedRows},
        {"hostile persisted rows fail integrity checks",
         hostilePersistedRowsFailIntegrityChecks},
        {"contexts attach and close fail typed without closing shared database",
         contextsAttachAndCloseFailTypedWithoutClosingSharedDatabase},
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
              << " dashboard operational repository tests passed.\n";
    return 0;
}
