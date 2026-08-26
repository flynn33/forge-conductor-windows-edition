#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"
#include "Persistence/Windows/Detail/WinsqliteTransaction.h"

#include <Windows.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;

using namespace std::chrono_literals;

constexpr DWORD MaximumProtocolWaitMilliseconds = 15'000U;

enum class ExitCode : int {
    Success = 0,
    Usage = 2,
    InvalidNumber = 3,
    NamespaceOpen = 10,
    MigrationLock = 11,
    VfsOpen = 12,
    DatabaseOpen = 13,
    DatabaseOperation = 14,
    DatabaseClose = 15,
    EventOpen = 20,
    EventSignal = 21,
    EventWait = 22,
    OwnerLeak = 23,
    UnexpectedFailure = 90,
};

[[nodiscard]] int exitCode(const ExitCode value) noexcept
{
    return static_cast<int>(value);
}

[[nodiscard]] bool parseBoundedUnsigned(
    const wchar_t* const text,
    const unsigned long minimum,
    const unsigned long maximum,
    unsigned long& value) noexcept
{
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end{};
    errno = 0;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] Domain::OperationContext operationContext(
    const std::chrono::milliseconds timeout,
    const std::string_view correlation)
{
    auto operation = Domain::OperationId::parse(
        "82000000-0000-4000-8000-000000000001");
    auto correlationId = Domain::CorrelationId::parse(correlation);
    if (!operation || !correlationId) {
        throw std::runtime_error{"the fixture operation context is invalid"};
    }
    return Domain::OperationContext{
        std::move(operation).value(),
        std::chrono::steady_clock::now() + timeout,
        {},
        std::move(correlationId).value()};
}

class FixtureDatabase final {
public:
    [[nodiscard]] static std::optional<FixtureDatabase> open(
        const std::wstring_view directory,
        const std::wstring_view mainBasename,
        const std::wstring_view lockBasename,
        const PersistenceDetail::WinsqliteOpenMode openMode,
        const Domain::OperationContext& context)
    {
        auto namespaceResult = PersistenceDetail::DatabaseNamespaceLease::create(
            directory, mainBasename, lockBasename);
        if (!namespaceResult) {
            return std::nullopt;
        }
        auto namespaceLease = std::move(namespaceResult).value();
        auto vfsResult = PersistenceDetail::AnchoredSqliteVfs::create(namespaceLease);
        if (!vfsResult) {
            return std::nullopt;
        }
        auto vfs = std::move(vfsResult).value();
        auto connectionResult = PersistenceDetail::WinsqliteConnection::open(
            namespaceLease->canonicalMainDatabasePath(),
            PersistenceDetail::WinsqliteConnectionOptions{
                std::string{vfs->vfsName()},
                openMode,
                PersistenceDetail::WinsqliteSynchronousMode::Full,
                PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
                namespaceLease},
            context);
        if (!connectionResult) {
            static_cast<void>(vfs->close());
            return std::nullopt;
        }
        return FixtureDatabase{
            std::move(namespaceLease),
            std::move(vfs),
            std::move(connectionResult).value()};
    }

    ~FixtureDatabase() noexcept
    {
        connection_.reset();
        if (vfs_) {
            static_cast<void>(vfs_->close());
        }
    }

    FixtureDatabase(const FixtureDatabase&) = delete;
    FixtureDatabase& operator=(const FixtureDatabase&) = delete;

    FixtureDatabase(FixtureDatabase&&) noexcept = default;
    FixtureDatabase& operator=(FixtureDatabase&&) noexcept = default;

    [[nodiscard]] PersistenceDetail::WinsqliteConnection& connection() noexcept
    {
        return connection_.value();
    }

    [[nodiscard]] ExitCode close(const Domain::OperationContext& context) noexcept
    {
        if (connection_.has_value()) {
            auto closed = connection_->close(context);
            if (!closed) {
                return ExitCode::DatabaseClose;
            }
            connection_.reset();
        }
        if (namespaceLease_->openVfsFileCount() != 0U || vfs_->openFileCount() != 0U) {
            return ExitCode::OwnerLeak;
        }
        auto vfsClosed = vfs_->close();
        if (!vfsClosed) {
            return ExitCode::DatabaseClose;
        }
        vfs_.reset();
        return namespaceLease_->openVfsFileCount() == 0U
                   ? ExitCode::Success
                   : ExitCode::OwnerLeak;
    }

private:
    FixtureDatabase(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs,
        PersistenceDetail::WinsqliteConnection connection) noexcept
        : namespaceLease_{std::move(namespaceLease)},
          vfs_{std::move(vfs)},
          connection_{std::move(connection)}
    {
    }

    std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs_;
    std::optional<PersistenceDetail::WinsqliteConnection> connection_;
};

[[nodiscard]] int signalReadyAndWait(
    const wchar_t* const readyEventName,
    const wchar_t* const releaseEventName,
    const DWORD waitMilliseconds) noexcept
{
    InfrastructureDetail::UniqueHandle ready{
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName)};
    InfrastructureDetail::UniqueHandle release{
        ::OpenEventW(SYNCHRONIZE, FALSE, releaseEventName)};
    if (!ready || !release) {
        return exitCode(ExitCode::EventOpen);
    }
    if (::SetEvent(ready.get()) == FALSE) {
        return exitCode(ExitCode::EventSignal);
    }
    return ::WaitForSingleObject(release.get(), waitMilliseconds) == WAIT_OBJECT_0
               ? exitCode(ExitCode::Success)
               : exitCode(ExitCode::EventWait);
}

[[nodiscard]] int waitForStart(
    const wchar_t* const startEventName,
    const DWORD waitMilliseconds) noexcept
{
    InfrastructureDetail::UniqueHandle start{
        ::OpenEventW(SYNCHRONIZE, FALSE, startEventName)};
    if (!start) {
        return exitCode(ExitCode::EventOpen);
    }
    return ::WaitForSingleObject(start.get(), waitMilliseconds) == WAIT_OBJECT_0
               ? exitCode(ExitCode::Success)
               : exitCode(ExitCode::EventWait);
}

[[nodiscard]] int holdMigrationLock(
    wchar_t** const arguments,
    const DWORD waitMilliseconds)
{
    auto namespaceResult = PersistenceDetail::DatabaseNamespaceLease::create(
        arguments[2], arguments[3], arguments[4]);
    if (!namespaceResult) {
        return exitCode(ExitCode::NamespaceOpen);
    }
    auto namespaceLease = std::move(namespaceResult).value();
    const auto context = operationContext(
        std::chrono::milliseconds{waitMilliseconds}, "p07-fixture-hold-lock");
    auto lockResult = namespaceLease->acquireMigrationLock(context);
    if (!lockResult) {
        return exitCode(ExitCode::MigrationLock);
    }
    auto lock = std::move(lockResult).value();
    static_cast<void>(lock);
    return signalReadyAndWait(arguments[5], arguments[6], waitMilliseconds);
}

[[nodiscard]] int initializeDatabase(
    wchar_t** const arguments,
    const unsigned long initializerId,
    const DWORD waitMilliseconds)
{
    const int waitResult = waitForStart(arguments[5], waitMilliseconds);
    if (waitResult != exitCode(ExitCode::Success)) {
        return waitResult;
    }

    auto namespaceResult = PersistenceDetail::DatabaseNamespaceLease::create(
        arguments[2], arguments[3], arguments[4]);
    if (!namespaceResult) {
        return exitCode(ExitCode::NamespaceOpen);
    }
    auto namespaceLease = std::move(namespaceResult).value();
    const auto context = operationContext(
        std::chrono::milliseconds{waitMilliseconds}, "p07-fixture-initialize");
    auto lockResult = namespaceLease->acquireMigrationLock(context);
    if (!lockResult) {
        return exitCode(ExitCode::MigrationLock);
    }
    auto migrationLock = std::move(lockResult).value();
    static_cast<void>(migrationLock);

    auto database = FixtureDatabase::open(
        arguments[2], arguments[3], arguments[4],
        PersistenceDetail::WinsqliteOpenMode::ReadWriteCreate, context);
    if (!database) {
        return exitCode(ExitCode::DatabaseOpen);
    }
    auto created = database->connection().execute(
        "CREATE TABLE IF NOT EXISTS process_initializers("
        "initializer_id INTEGER PRIMARY KEY);",
        context);
    if (!created) {
        return exitCode(ExitCode::DatabaseOperation);
    }
    {
        auto prepared = database->connection().prepare(
            "INSERT OR REPLACE INTO process_initializers(initializer_id) VALUES(?1);",
            context);
        if (!prepared) {
            return exitCode(ExitCode::DatabaseOperation);
        }
        auto statement = std::move(prepared).value();
        if (!statement.bindInt64(1, static_cast<std::int64_t>(initializerId))) {
            return exitCode(ExitCode::DatabaseOperation);
        }
        auto stepped = statement.step();
        if (!stepped || stepped.value() != PersistenceDetail::WinsqliteStepResult::Done) {
            return exitCode(ExitCode::DatabaseOperation);
        }
    }
    const ExitCode closed = database->close(context);
    if (closed != ExitCode::Success) {
        return exitCode(closed);
    }
    return exitCode(ExitCode::Success);
}

[[nodiscard]] int writeWalAndWait(
    wchar_t** const arguments,
    const unsigned long marker,
    const DWORD waitMilliseconds)
{
    const auto context = operationContext(
        std::chrono::milliseconds{waitMilliseconds}, "p07-fixture-wal-writer");
    auto database = FixtureDatabase::open(
        arguments[2], arguments[3], arguments[4],
        PersistenceDetail::WinsqliteOpenMode::ReadWriteCreate, context);
    if (!database) {
        return exitCode(ExitCode::DatabaseOpen);
    }
    auto configured = database->connection().execute(
        "PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE IF NOT EXISTS process_wal_probe("
        "marker INTEGER PRIMARY KEY);",
        context);
    if (!configured) {
        return exitCode(ExitCode::DatabaseOperation);
    }
    {
        auto prepared = database->connection().prepare(
            "INSERT OR REPLACE INTO process_wal_probe(marker) VALUES(?1);", context);
        if (!prepared) {
            return exitCode(ExitCode::DatabaseOperation);
        }
        auto statement = std::move(prepared).value();
        if (!statement.bindInt64(1, static_cast<std::int64_t>(marker))) {
            return exitCode(ExitCode::DatabaseOperation);
        }
        auto stepped = statement.step();
        if (!stepped || stepped.value() != PersistenceDetail::WinsqliteStepResult::Done) {
            return exitCode(ExitCode::DatabaseOperation);
        }
    }

    const int waitResult = signalReadyAndWait(
        arguments[5], arguments[6], waitMilliseconds);
    if (waitResult != exitCode(ExitCode::Success)) {
        return waitResult;
    }
    const ExitCode closed = database->close(context);
    if (closed != ExitCode::Success) {
        return exitCode(closed);
    }
    return exitCode(ExitCode::Success);
}

[[nodiscard]] int holdWriteLock(
    wchar_t** const arguments,
    const DWORD waitMilliseconds)
{
    const auto context = operationContext(
        std::chrono::milliseconds{waitMilliseconds}, "p07-fixture-write-lock");
    auto database = FixtureDatabase::open(
        arguments[2], arguments[3], arguments[4],
        PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting, context);
    if (!database) {
        return exitCode(ExitCode::DatabaseOpen);
    }
    auto transactionResult = PersistenceDetail::WinsqliteTransaction::beginImmediate(
        database->connection(), context);
    if (!transactionResult) {
        return exitCode(ExitCode::DatabaseOperation);
    }
    auto transaction = std::move(transactionResult).value();
    const int waitResult = signalReadyAndWait(
        arguments[5], arguments[6], waitMilliseconds);
    if (waitResult != exitCode(ExitCode::Success)) {
        return waitResult;
    }
    if (!transaction.rollback()) {
        return exitCode(ExitCode::DatabaseOperation);
    }
    const ExitCode closed = database->close(context);
    if (closed != ExitCode::Success) {
        return exitCode(closed);
    }
    return exitCode(ExitCode::Success);
}

[[nodiscard]] int insertProjectGenerationTag(
    wchar_t** const arguments,
    const unsigned long marker,
    const DWORD waitMilliseconds)
{
    InfrastructureDetail::UniqueHandle start{
        ::OpenEventW(SYNCHRONIZE, FALSE, arguments[5])};
    InfrastructureDetail::UniqueHandle attempted{
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[6])};
    InfrastructureDetail::UniqueHandle done{
        ::OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[7])};
    if (!start || !attempted || !done) {
        return exitCode(ExitCode::EventOpen);
    }
    if (::WaitForSingleObject(start.get(), waitMilliseconds) != WAIT_OBJECT_0) {
        return exitCode(ExitCode::EventWait);
    }
    const auto context = operationContext(
        std::chrono::milliseconds{waitMilliseconds},
        "p07-fixture-project-generation-writer");
    auto database = FixtureDatabase::open(
        arguments[2], arguments[3], arguments[4],
        PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting, context);
    if (!database) {
        return exitCode(ExitCode::DatabaseOpen);
    }
    {
        auto prepared = database->connection().prepare(
            "INSERT INTO memory_tags(id, name) VALUES(?1, ?2);", context);
        if (!prepared) {
            return exitCode(ExitCode::DatabaseOperation);
        }
        auto statement = std::move(prepared).value();
        const std::string tag = "process-generation-" + std::to_string(marker);
        if (!statement.bindInt64(1, static_cast<std::int64_t>(marker)) ||
            !statement.bindText(2, tag)) {
            return exitCode(ExitCode::DatabaseOperation);
        }
        if (::SetEvent(attempted.get()) == FALSE) {
            return exitCode(ExitCode::EventSignal);
        }
        auto stepped = statement.step();
        if (!stepped || stepped.value() != PersistenceDetail::WinsqliteStepResult::Done) {
            return exitCode(ExitCode::DatabaseOperation);
        }
    }
    const ExitCode closed = database->close(context);
    if (closed != ExitCode::Success) {
        return exitCode(closed);
    }
    if (::SetEvent(done.get()) == FALSE) {
        return exitCode(ExitCode::EventSignal);
    }
    return exitCode(ExitCode::Success);
}

} // namespace

int wmain(const int argumentCount, wchar_t** const arguments)
{
    try {
        if (arguments == nullptr || argumentCount < 2 || arguments[1] == nullptr) {
            return exitCode(ExitCode::Usage);
        }
        const std::wstring_view mode{arguments[1]};
        unsigned long waitValue{};

        if (mode == L"--hold-lock" && argumentCount == 8) {
            if (!parseBoundedUnsigned(
                    arguments[7], 1U, MaximumProtocolWaitMilliseconds, waitValue)) {
                return exitCode(ExitCode::InvalidNumber);
            }
            return holdMigrationLock(arguments, static_cast<DWORD>(waitValue));
        }
        if (mode == L"--initialize" && argumentCount == 8) {
            unsigned long initializerId{};
            if (!parseBoundedUnsigned(arguments[6], 1U, 1'000'000U, initializerId) ||
                !parseBoundedUnsigned(
                    arguments[7], 1U, MaximumProtocolWaitMilliseconds, waitValue)) {
                return exitCode(ExitCode::InvalidNumber);
            }
            return initializeDatabase(
                arguments, initializerId, static_cast<DWORD>(waitValue));
        }
        if (mode == L"--wal-writer" && argumentCount == 9) {
            unsigned long marker{};
            if (!parseBoundedUnsigned(arguments[7], 1U, 1'000'000U, marker) ||
                !parseBoundedUnsigned(
                    arguments[8], 1U, MaximumProtocolWaitMilliseconds, waitValue)) {
                return exitCode(ExitCode::InvalidNumber);
            }
            return writeWalAndWait(arguments, marker, static_cast<DWORD>(waitValue));
        }
        if (mode == L"--hold-write-lock" && argumentCount == 8) {
            if (!parseBoundedUnsigned(
                    arguments[7], 1U, MaximumProtocolWaitMilliseconds, waitValue)) {
                return exitCode(ExitCode::InvalidNumber);
            }
            return holdWriteLock(arguments, static_cast<DWORD>(waitValue));
        }
        if (mode == L"--project-generation-writer" && argumentCount == 10) {
            unsigned long marker{};
            if (!parseBoundedUnsigned(arguments[8], 3U, 1'000'000U, marker) ||
                !parseBoundedUnsigned(
                    arguments[9], 1U, MaximumProtocolWaitMilliseconds, waitValue)) {
                return exitCode(ExitCode::InvalidNumber);
            }
            return insertProjectGenerationTag(
                arguments, marker, static_cast<DWORD>(waitValue));
        }
        return exitCode(ExitCode::Usage);
    } catch (...) {
        return exitCode(ExitCode::UnexpectedFailure);
    }
}
