#include "ForgeConductor/Persistence/Windows/WindowsClientPresenceRepository.h"
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

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::UtcTimePoint atSeconds(const std::int64_t seconds)
{
    return Domain::UtcTimePoint{std::chrono::seconds{seconds}};
}

[[nodiscard]] Domain::PathText workingDirectory(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::ClientPresenceIdentity identity(
    const std::string_view clientId,
    std::string role,
    const std::string_view deploymentId,
    const std::uint32_t processId)
{
    return Domain::ClientPresenceIdentity{
        parse<Domain::ClientId>(clientId),
        std::move(role),
        parse<Domain::DeploymentId>(deploymentId),
        processId};
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

    [[nodiscard]] std::string text(const std::string_view sql)
    {
        sqlite3_stmt* statement{};
        const auto source = std::string{sql};
        if (::sqlite3_prepare_v2(
                database_,
                source.c_str(),
                static_cast<int>(source.size()),
                &statement,
                nullptr) != SQLITE_OK ||
            statement == nullptr) {
            throw std::runtime_error{"sqlite fixture prepare failed"};
        }
        struct Finalizer final {
            sqlite3_stmt* statement;
            ~Finalizer() noexcept
            {
                static_cast<void>(::sqlite3_finalize(statement));
            }
        } finalizer{statement};
        if (::sqlite3_step(statement) != SQLITE_ROW) {
            throw std::runtime_error{"sqlite fixture query returned no row"};
        }
        const auto* value = ::sqlite3_column_text(statement, 0);
        const int bytes = ::sqlite3_column_bytes(statement, 0);
        if (value == nullptr || bytes < 0) {
            throw std::runtime_error{"sqlite fixture text was invalid"};
        }
        return std::string{
            reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(bytes)};
    }

    [[nodiscard]] std::int64_t integer(const std::string_view sql)
    {
        sqlite3_stmt* statement{};
        const auto source = std::string{sql};
        if (::sqlite3_prepare_v2(
                database_,
                source.c_str(),
                static_cast<int>(source.size()),
                &statement,
                nullptr) != SQLITE_OK ||
            statement == nullptr) {
            throw std::runtime_error{"sqlite fixture prepare failed"};
        }
        struct Finalizer final {
            sqlite3_stmt* statement;
            ~Finalizer() noexcept
            {
                static_cast<void>(::sqlite3_finalize(statement));
            }
        } finalizer{statement};
        if (::sqlite3_step(statement) != SQLITE_ROW ||
            ::sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
            throw std::runtime_error{"sqlite fixture integer query failed"};
        }
        return ::sqlite3_column_int64(statement, 0);
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
            Support::activeContext("presence-database-open")));
        database = std::shared_ptr<Persistence::WindowsCentralDatabase>{
            std::move(opened)};
        repository = take(
            Persistence::WindowsClientPresenceRepository::attach(database));
    }

    void close()
    {
        if (repository) {
            repository->close();
        }
        if (database) {
            take(database->close(
                Support::activeContext("presence-database-close")));
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
    std::shared_ptr<Persistence::WindowsClientPresenceRepository> repository;
};

void replacementRejectsStaleOwnerAndPreservesFirstSeen()
{
    requireError(
        Persistence::WindowsClientPresenceRepository::attach({}),
        Domain::ErrorCodes::InvalidRequest);

    Fixture fixture{L"client-presence-replacement"};
    const auto firstIdentity = identity(
        "client-one", "primary", "deployment-one", 101U);
    const auto replacementIdentity = identity(
        "client-one", "fallback", "deployment-two", 202U);

    take(fixture.repository->upsert(
        Domain::ClientPresenceRegistration{
            firstIdentity,
            workingDirectory("D:\\workspaces\\first"),
            atSeconds(1'700'000'000),
            atSeconds(1'700'000'010)},
        Support::activeContext("presence-register-first")));
    require(
        take(fixture.repository->heartbeat(
            firstIdentity,
            atSeconds(1'700'000'020),
            Support::activeContext("presence-heartbeat-first"))),
        "the first owner heartbeat did not match");
    take(fixture.repository->upsert(
        Domain::ClientPresenceRegistration{
            replacementIdentity,
            workingDirectory("D:\\workspaces\\replacement"),
            atSeconds(1'700'000'030),
            atSeconds(1'700'000'030)},
        Support::activeContext("presence-register-replacement")));

    require(
        !take(fixture.repository->heartbeat(
            firstIdentity,
            atSeconds(1'700'000'040),
            Support::activeContext("presence-stale-heartbeat"))),
        "a stale owner refreshed the replacement row");
    require(
        !take(fixture.repository->remove(
            firstIdentity,
            Support::activeContext("presence-stale-remove"))),
        "a stale owner removed the replacement row");
    require(
        take(fixture.repository->heartbeat(
            replacementIdentity,
            atSeconds(1'700'000'040),
            Support::activeContext("presence-current-heartbeat"))),
        "the replacement owner heartbeat did not match");
    require(
        take(fixture.repository->heartbeat(
            replacementIdentity,
            atSeconds(1'700'000'035),
            Support::activeContext("presence-older-heartbeat"))),
        "an older heartbeat did not recognize the current owner");

    fixture.close();
    {
        SqliteDatabase database{fixture.databasePath()};
        require(
            database.text(
                "SELECT role FROM client_presence WHERE client_id='client-one'") ==
                "fallback",
            "replacement role was not persisted");
        require(
            database.text(
                "SELECT deployment_id FROM client_presence "
                "WHERE client_id='client-one'") == "deployment-two",
            "replacement deployment was not persisted");
        require(
            database.integer(
                "SELECT process_id FROM client_presence "
                "WHERE client_id='client-one'") == 202,
            "replacement process was not persisted");
        require(
            database.text(
                "SELECT working_directory FROM client_presence "
                "WHERE client_id='client-one'") ==
                "D:\\workspaces\\replacement",
            "replacement working directory was not persisted");
        require(
            database.text(
                "SELECT first_seen_at FROM client_presence "
                "WHERE client_id='client-one'") ==
                "2023-11-14T22:13:20.000Z",
            "replacement changed the original first-seen timestamp");
        require(
            database.text(
                "SELECT last_seen_at FROM client_presence "
                "WHERE client_id='client-one'") ==
                "2023-11-14T22:14:00.000Z",
            "an older heartbeat regressed the last-seen timestamp");
    }

    fixture.open();
    require(
        take(fixture.repository->remove(
            replacementIdentity,
            Support::activeContext("presence-current-remove"))),
        "the current owner could not remove its row");
    require(
        !take(fixture.repository->remove(
            replacementIdentity,
            Support::activeContext("presence-repeat-remove"))),
        "removing an absent presence row reported success");
}

void nullableOwnerFieldsParticipateInExactMatching()
{
    Fixture fixture{L"client-presence-null-owner"};
    const Domain::ClientPresenceIdentity owner{
        parse<Domain::ClientId>("manager-client"), "manager", {}, {}};
    const Domain::ClientPresenceIdentity wrongOwner{
        parse<Domain::ClientId>("manager-client"), "manager", {}, 44U};
    take(fixture.repository->upsert(
        Domain::ClientPresenceRegistration{
            owner,
            workingDirectory("D:\\workspaces\\manager"),
            atSeconds(1'700'001'000),
            atSeconds(1'700'001'000)},
        Support::activeContext("presence-null-register")));
    require(
        !take(fixture.repository->heartbeat(
            wrongOwner,
            atSeconds(1'700'001'001),
            Support::activeContext("presence-null-wrong-heartbeat"))),
        "a mismatched nullable owner field refreshed the row");
    require(
        !take(fixture.repository->remove(
            wrongOwner,
            Support::activeContext("presence-null-wrong-remove"))),
        "a mismatched nullable owner field removed the row");
    require(
        take(fixture.repository->heartbeat(
            owner,
            atSeconds(1'700'001'001),
            Support::activeContext("presence-null-heartbeat"))),
        "the exact nullable owner did not heartbeat");
    require(
        take(fixture.repository->remove(
            owner,
            Support::activeContext("presence-null-remove"))),
        "the exact nullable owner did not remove its row");
}

void validationContextsAndCloseFailTyped()
{
    Fixture fixture{L"client-presence-validation"};
    const auto goodIdentity = identity(
        "validated-client", "primary", "validated-deployment", 303U);

    auto emptyRole = goodIdentity;
    emptyRole.role.clear();
    requireError(
        fixture.repository->upsert(
            Domain::ClientPresenceRegistration{
                emptyRole, workingDirectory("D:\\workspaces\\invalid"),
                atSeconds(10), atSeconds(10)},
            Support::activeContext("presence-empty-role")),
        Domain::ErrorCodes::InvalidRequest);

    auto oversizedRole = goodIdentity;
    oversizedRole.role.assign(
        Domain::ClientPresenceLimits::MaximumRoleBytes + 1U, 'r');
    requireError(
        fixture.repository->heartbeat(
            oversizedRole,
            atSeconds(10),
            Support::activeContext("presence-oversized-role")),
        Domain::ErrorCodes::PayloadTooLarge);

    auto invalidUtf8Role = goodIdentity;
    invalidUtf8Role.role = std::string{"\xC3\x28", 2U};
    requireError(
        fixture.repository->remove(
            invalidUtf8Role,
            Support::activeContext("presence-invalid-utf8-role")),
        Domain::ErrorCodes::InvalidRequest);

    auto zeroProcess = goodIdentity;
    zeroProcess.processId = 0U;
    requireError(
        fixture.repository->heartbeat(
            zeroProcess,
            atSeconds(10),
            Support::activeContext("presence-zero-process")),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        fixture.repository->upsert(
            Domain::ClientPresenceRegistration{
                goodIdentity, workingDirectory("D:\\workspaces\\validated"),
                atSeconds(20), atSeconds(19)},
            Support::activeContext("presence-reversed-time")),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        fixture.repository->heartbeat(
            goodIdentity,
            atSeconds(-1),
            Support::activeContext("presence-negative-time")),
        Domain::ErrorCodes::InvalidRequest);

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = Support::activeContext("presence-cancelled");
    cancelled.cancellation = cancellation.get_token();
    requireError(
        fixture.repository->upsert(
            Domain::ClientPresenceRegistration{
                goodIdentity, workingDirectory("D:\\workspaces\\validated"),
                atSeconds(30), atSeconds(30)},
            cancelled),
        Domain::ErrorCodes::Cancelled);

    auto expired = Support::activeContext("presence-expired");
    expired.deadline = std::chrono::steady_clock::now() - 1ms;
    requireError(
        fixture.repository->heartbeat(goodIdentity, atSeconds(31), expired),
        Domain::ErrorCodes::DeadlineExceeded);

    fixture.repository->close();
    requireError(
        fixture.repository->remove(
            goodIdentity,
            Support::activeContext("presence-after-close")),
        Domain::ErrorCodes::InvalidRequest);
    take(fixture.database->quickCheck(
        Support::activeContext("presence-shared-database-open")));
}

struct TestCase final {
    const char* name;
    void (*run)();
};

} // namespace

int wmain()
{
    const std::array<TestCase, 3U> tests{{
        {"replacement rejects stale owner and preserves first seen",
         replacementRejectsStaleOwnerAndPreservesFirstSeen},
        {"nullable owner fields participate in exact matching",
         nullableOwnerFieldsParticipateInExactMatching},
        {"validation contexts and close fail typed",
         validationContextsAndCloseFailTyped},
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
              << " client presence repository tests passed.\n";
    return 0;
}
