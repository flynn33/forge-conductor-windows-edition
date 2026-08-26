#include "PersistenceTestSupport.h"

#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteBackup.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"
#include "Persistence/Windows/Detail/WinsqliteTransaction.h"

#include <winsqlite/winsqlite3.h>

#include <chrono>
#include <atomic>
#include <barrier>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteStatementTestAccess final {
public:
    [[nodiscard]] static Domain::Result<WinsqliteStepResult> stepAfterAdmissionBarrier(
        WinsqliteStatement& statement,
        std::barrier<>& admitted,
        std::barrier<>& release) noexcept
    {
        auto lifetimeLock = statement.lockTransactionLifetime();
        admitted.arrive_and_wait();
        release.arrive_and_wait();
        if (!lifetimeLock) {
            return Domain::Result<WinsqliteStepResult>::failure(
                std::move(lifetimeLock).error());
        }
        auto lock = std::move(lifetimeLock).value();
        static_cast<void>(lock);
        return statement.step();
    }

    [[nodiscard]] static sqlite3_stmt* detachNativeStatement(
        WinsqliteStatement& statement) noexcept
    {
        if (statement.transactionLifetime_ != nullptr) {
            return nullptr;
        }
        statement.rowAvailable_ = false;
        statement.operation_.reset();
        return std::exchange(statement.statement_, nullptr);
    }
};

} // namespace ForgeConductor::Persistence::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;
using namespace std::chrono_literals;
using PersistenceSupport::ScopedTestDirectory;
using PersistenceSupport::activeContext;
using PersistenceDetail::WinsqliteConnection;
using PersistenceDetail::WinsqliteOpenMode;
using PersistenceDetail::WinsqliteStepResult;
using PersistenceDetail::WinsqliteSynchronousMode;

class NativeStatementOwner final {
public:
    explicit NativeStatementOwner(sqlite3_stmt* statement) noexcept
        : statement_{statement}
    {
    }

    ~NativeStatementOwner() noexcept
    {
        if (statement_ != nullptr) {
            static_cast<void>(::sqlite3_finalize(statement_));
        }
    }

    NativeStatementOwner(const NativeStatementOwner&) = delete;
    NativeStatementOwner& operator=(const NativeStatementOwner&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return statement_ != nullptr;
    }

    [[nodiscard]] int finalize() noexcept
    {
        return statement_ != nullptr
            ? ::sqlite3_finalize(std::exchange(statement_, nullptr))
            : SQLITE_MISUSE;
    }

private:
    sqlite3_stmt* statement_{};
};

class KernelEnvironment final {
public:
    [[nodiscard]] static std::unique_ptr<KernelEnvironment> create(
        const std::filesystem::path& directory,
        const std::wstring_view basename)
    {
        const std::wstring canonicalDirectory = std::filesystem::canonical(directory).native();
        const std::wstring migrationLock = std::wstring{basename} + L".migration.lock";
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonicalDirectory, basename, migrationLock));
        auto vfs = take(PersistenceDetail::AnchoredSqliteVfs::create(namespaceLease));
        return std::unique_ptr<KernelEnvironment>{new KernelEnvironment{
            std::move(namespaceLease), std::move(vfs)}};
    }

    ~KernelEnvironment() noexcept
    {
        if (vfs_ != nullptr) {
            static_cast<void>(vfs_->close());
        }
    }

    KernelEnvironment(const KernelEnvironment&) = delete;
    KernelEnvironment& operator=(const KernelEnvironment&) = delete;
    KernelEnvironment(KernelEnvironment&&) = delete;
    KernelEnvironment& operator=(KernelEnvironment&&) = delete;

    [[nodiscard]] PersistenceDetail::WinsqliteConnectionOptions options(
        const WinsqliteOpenMode openMode,
        const WinsqliteSynchronousMode synchronousMode =
            WinsqliteSynchronousMode::Full) const
    {
        return PersistenceDetail::WinsqliteConnectionOptions{
            std::string{vfs_->vfsName()}, openMode, synchronousMode,
            PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
            namespaceLease_};
    }

    [[nodiscard]] const std::wstring& databasePath() const noexcept
    {
        return namespaceLease_->canonicalMainDatabasePath();
    }

    [[nodiscard]] std::size_t openFileCount() const noexcept
    {
        return vfs_->openFileCount();
    }

private:
    KernelEnvironment(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs) noexcept
        : namespaceLease_{std::move(namespaceLease)}, vfs_{std::move(vfs)}
    {
    }

    std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs_;
};

[[nodiscard]] WinsqliteConnection openDatabase(
    const KernelEnvironment& environment,
    const WinsqliteOpenMode openMode,
    const Domain::OperationContext& context,
    const WinsqliteSynchronousMode synchronousMode = WinsqliteSynchronousMode::Full)
{
    return take(WinsqliteConnection::open(
        environment.databasePath(), environment.options(openMode, synchronousMode), context));
}

[[nodiscard]] std::int64_t queryInteger(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "integer query did not return a row");
    const std::int64_t value = take(statement.columnInt64(0));
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "integer query returned more than one row");
    return value;
}

[[nodiscard]] std::string queryText(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "text query did not return a row");
    auto value = take(statement.columnText(0, 128U));
    require(value.has_value(), "text query unexpectedly returned null");
    std::string copied = std::move(value).value();
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "text query returned more than one row");
    return copied;
}

[[nodiscard]] Domain::OperationContext boundedContext(
    const std::chrono::milliseconds timeout,
    const std::stop_token cancellation = {})
{
    auto context = activeContext("p07-kernel-bounded");
    context.deadline = std::chrono::steady_clock::now() + timeout;
    context.cancellation = cancellation;
    return context;
}

void testOpenModesAndReadOnlyPreservation()
{
    ScopedTestDirectory directory{L"kernel-open-modes"};
    auto environment = KernelEnvironment::create(directory.path(), L"open-modes.sqlite");
    const auto context = activeContext("p07-kernel-open-modes");

    {
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteCreate, context);
        take(connection.execute(
            "CREATE TABLE mode_probe(value INTEGER NOT NULL);"
            "INSERT INTO mode_probe(value) VALUES(7);",
            context));
        take(connection.close(context));
    }
    require(std::filesystem::is_regular_file(environment->databasePath()),
            "read-write-create did not create the main database");
    const std::string beforeReadOnly = PersistenceSupport::readFixture(
        environment->databasePath());

    auto missingVfsOptions = environment->options(WinsqliteOpenMode::ReadWriteExisting);
    missingVfsOptions.vfsName = "forge-unregistered-kernel-test";
    const auto missingVfs = WinsqliteConnection::open(
        environment->databasePath(), missingVfsOptions, context);
    requireError(missingVfs, Domain::ErrorCodes::InvalidRequest,
                 "connection open silently fell back from an unregistered VFS");

    {
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadOnlyExisting, context,
            WinsqliteSynchronousMode::Normal);
        require(queryInteger(connection, "SELECT value FROM mode_probe;", context) == 7,
                "read-only-existing did not read the existing database");
        const auto write = connection.execute(
            "INSERT INTO mode_probe(value) VALUES(8);", context);
        requireError(write, Domain::ErrorCodes::Unauthorized,
                     "read-only-existing allowed a write");
        require(queryInteger(connection, "PRAGMA query_only;", context) == 1,
                "read-only-existing did not enable query_only");
        require(queryInteger(connection, "PRAGMA synchronous;", context) == 2,
                "read-only-existing applied a mutating synchronous policy");
        require(queryText(connection, "PRAGMA journal_mode;", context) == "wal",
                "read-only-existing did not preserve the established journal mode");
        take(connection.close(context));
    }
    const std::string afterReadOnly = PersistenceSupport::readFixture(
        environment->databasePath());
    require(beforeReadOnly == afterReadOnly,
            "read-only assessment changed the main database bytes");

    {
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute("INSERT INTO mode_probe(value) VALUES(8);", context));
        require(queryInteger(connection, "SELECT COUNT(*) FROM mode_probe;", context) == 2,
                "read-write-existing did not preserve and extend existing data");
        take(connection.close(context));
    }

    auto missing = KernelEnvironment::create(directory.path(), L"missing.sqlite");
    const auto missingRead = WinsqliteConnection::open(
        missing->databasePath(),
        missing->options(WinsqliteOpenMode::ReadOnlyExisting), context);
    require(!missingRead, "read-only-existing created a missing database");
    const auto missingWrite = WinsqliteConnection::open(
        missing->databasePath(),
        missing->options(WinsqliteOpenMode::ReadWriteExisting), context);
    require(!missingWrite, "read-write-existing created a missing database");
    require(!std::filesystem::exists(missing->databasePath()),
            "existing-only open modes left a database behind");
    require(environment->openFileCount() == 0U && missing->openFileCount() == 0U,
            "open-mode tests leaked a VFS file owner");
}

void testDefensiveConnectionSettingsAndAuthorizer()
{
    ScopedTestDirectory directory{L"kernel-defensive"};
    auto environment = KernelEnvironment::create(directory.path(), L"defensive.sqlite");
    const auto context = activeContext("p07-kernel-defensive");

    {
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteCreate, context,
            WinsqliteSynchronousMode::Normal);
        require(queryInteger(connection, "PRAGMA foreign_keys;", context) == 1,
                "foreign-key enforcement is disabled");
        require(queryText(connection, "PRAGMA journal_mode;", context) == "wal",
                "journal mode is not WAL");
        require(queryInteger(connection, "PRAGMA synchronous;", context) == 1,
                "normal synchronous policy was not applied");
        require(queryInteger(connection, "PRAGMA temp_store;", context) == 2,
                "temporary SQLite storage was not confined to memory");
        require(queryInteger(connection, "PRAGMA trusted_schema;", context) == 0,
                "trusted schema was not disabled");
        require(queryInteger(connection, "PRAGMA writable_schema;", context) == 0,
                "writable schema was not disabled");

        const std::size_t persistentFileOwners = environment->openFileCount();
        take(connection.execute(
            "CREATE TEMP TABLE bounded_temp_sort(value INTEGER NOT NULL);"
            "WITH RECURSIVE counter(value) AS ("
            "VALUES(1) UNION ALL SELECT value + 1 FROM counter WHERE value < 4096) "
            "INSERT INTO bounded_temp_sort(value) SELECT value FROM counter;",
            context));
        require(environment->openFileCount() == persistentFileOwners,
                "a temporary table opened a disk-backed VFS file");
        take(connection.execute(
            "SELECT value FROM bounded_temp_sort "
            "ORDER BY ((value * 1103515245) & 2147483647);",
            context));
        require(environment->openFileCount() == persistentFileOwners,
                "a bounded sort opened a disk-backed VFS file");
        take(connection.execute("DROP TABLE bounded_temp_sort;", context));

        const auto attach = connection.execute(
            "ATTACH DATABASE ':memory:' AS hostile;", context);
        requireError(attach, Domain::ErrorCodes::Unauthorized,
                     "the authorizer allowed ATTACH");
        const auto changeJournal = connection.execute(
            "PRAGMA journal_mode=DELETE;", context);
        requireError(changeJournal, Domain::ErrorCodes::Unauthorized,
                     "the authorizer allowed a journal-mode mutation");
        const auto writableSchema = connection.execute(
            "PRAGMA writable_schema=ON;", context);
        requireError(writableSchema, Domain::ErrorCodes::Unauthorized,
                     "the authorizer allowed writable_schema");
        const auto trustedSchema = connection.execute(
            "PRAGMA trusted_schema=ON;", context);
        requireError(trustedSchema, Domain::ErrorCodes::Unauthorized,
                     "the authorizer allowed trusted_schema");
        const auto disableForeignKeys = connection.execute(
            "PRAGMA foreign_keys=OFF;", context);
        requireError(disableForeignKeys, Domain::ErrorCodes::Unauthorized,
                     "the authorizer allowed foreign-key enforcement to be disabled");
        const auto loadExtension = connection.execute(
            "SELECT load_extension('untrusted-extension');", context);
        requireError(loadExtension, Domain::ErrorCodes::Unauthorized,
                     "the authorizer allowed load_extension");
        const auto doubleQuotedString = connection.execute(
            "SELECT \"this_must_be_an_identifier\";", context);
        require(!doubleQuotedString,
                "double-quoted string literal compatibility remained enabled");
        take(connection.close(context));
    }

    {
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context,
            WinsqliteSynchronousMode::Full);
        require(queryInteger(connection, "PRAGMA synchronous;", context) == 2,
                "full synchronous policy was not applied");
        take(connection.close(context));
    }
    require(environment->openFileCount() == 0U,
            "defensive-setting tests leaked a VFS file owner");
}

void testTypedStatementsAndBounds()
{
    ScopedTestDirectory directory{L"kernel-statements"};
    auto environment = KernelEnvironment::create(directory.path(), L"statements.sqlite");
    const auto context = activeContext("p07-kernel-statements");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(connection.execute(
        "CREATE TABLE typed_values("
        "id INTEGER PRIMARY KEY, text_value TEXT NOT NULL, integer_value INTEGER NOT NULL, "
        "double_value REAL NOT NULL, null_value TEXT);",
        context));

    {
        auto insert = take(connection.prepare(
            "INSERT INTO typed_values(text_value, integer_value, double_value, null_value) "
            "VALUES(?1, ?2, ?3, ?4);",
            context));
        require(insert.parameterCount() == 4, "typed insert parameter count is wrong");
        const auto invalidIndex = insert.bindInt64(5, 1);
        requireError(invalidIndex, Domain::ErrorCodes::InvalidRequest,
                     "out-of-range bind index was accepted");

        take(insert.bindText(1, ""));
        take(insert.bindInt64(2, 9'007'199'254'740'991LL));
        take(insert.bindDouble(3, 3.25));
        take(insert.bindNull(4));
        require(take(insert.step()) == WinsqliteStepResult::Done,
                "first typed insert did not complete");

        take(insert.reset());
        std::string oversized(16U * 1024U * 1024U + 1U, 'x');
        const auto oversizedBind = insert.bindText(1, oversized);
        requireError(oversizedBind, Domain::ErrorCodes::PayloadTooLarge,
                     "oversized text binding was accepted");
        take(insert.bindText(1, "0123456789"));
        take(insert.bindInt64(2, -42));
        take(insert.bindDouble(3, -0.5));
        take(insert.bindNull(4));
        require(take(insert.step()) == WinsqliteStepResult::Done,
                "second typed insert did not complete");
    }

    {
        auto select = take(connection.prepare(
            "SELECT text_value, integer_value, double_value, null_value "
            "FROM typed_values ORDER BY id;",
            context));
        require(select.columnCount() == 4, "typed select column count is wrong");
        const auto beforeRow = select.columnText(0, 16U);
        requireError(beforeRow, Domain::ErrorCodes::InvalidRequest,
                     "column access before Row was accepted");

        require(take(select.step()) == WinsqliteStepResult::Row,
                "first typed row is missing");
        auto empty = take(select.columnText(0, 1U));
        require(empty.has_value() && empty->empty(),
                "empty text was rebound as SQL null");
        require(take(select.columnInt64(1)) == 9'007'199'254'740'991LL,
                "64-bit integer binding changed value");
        require(std::abs(take(select.columnDouble(2)) - 3.25) < 0.000001,
                "double binding changed value");
        require(take(select.columnIsNull(3)), "null binding changed value");
        auto nullText = take(select.columnText(3, 1U));
        require(!nullText.has_value(),
                "null text column was materialized as an empty string");

        require(take(select.step()) == WinsqliteStepResult::Row,
                "second typed row is missing");
        const auto boundedText = select.columnText(0, 5U);
        requireError(boundedText, Domain::ErrorCodes::PayloadTooLarge,
                     "bounded column read returned oversized text");
        auto completeText = take(select.columnText(0, 10U));
        require(completeText.has_value() && completeText.value() == "0123456789",
                "bounded text column changed value");
        const auto wrongType = select.columnInt64(0);
        requireError(wrongType, Domain::ErrorCodes::IntegrityFailure,
                     "typed column access silently coerced text to integer");
        require(take(select.step()) == WinsqliteStepResult::Done,
                "typed select returned an unexpected row");
    }

    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "typed-statement tests leaked a VFS file owner");
}

void testCompleteSqlScriptExecution()
{
    ScopedTestDirectory directory{L"kernel-script"};
    auto environment = KernelEnvironment::create(directory.path(), L"script.sqlite");
    const auto context = activeContext("p07-kernel-script");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);

    take(connection.execute(
        "CREATE TABLE script_values(value INTEGER NOT NULL);"
        "INSERT INTO script_values(value) VALUES(10);"
        "-- the complete-script executor must consume comments too\n"
        "INSERT INTO script_values(value) VALUES(20);",
        context));
    require(queryInteger(connection, "SELECT SUM(value) FROM script_values;", context) == 30,
            "multi-statement SQL did not execute completely");

    const auto multiplePreparedStatements = connection.prepare(
        "SELECT 1; SELECT 2;", context);
    requireError(multiplePreparedStatements, Domain::ErrorCodes::InvalidRequest,
                 "prepared SQL accepted more than one statement");

    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        const auto invalidTail = transaction.execute(
            "INSERT INTO script_values(value) VALUES(40); trailing garbage");
        require(!invalidTail, "invalid trailing SQL bytes were ignored");
        take(transaction.rollback());
    }
    require(queryInteger(connection, "SELECT SUM(value) FROM script_values;", context) == 30,
            "rollback did not undo statements preceding invalid trailing SQL");

    const auto commentsOnly = connection.execute("-- no executable statement\n", context);
    requireError(commentsOnly, Domain::ErrorCodes::InvalidRequest,
                 "a comments-only SQL script was accepted");
    take(connection.close(context));
}

void testTransactionCommitAndRollbackOwnership()
{
    ScopedTestDirectory directory{L"kernel-transactions"};
    auto environment = KernelEnvironment::create(directory.path(), L"transactions.sqlite");
    const auto context = activeContext("p07-kernel-transactions");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(connection.execute(
        "CREATE TABLE transaction_values(value INTEGER NOT NULL);", context));

    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        {
            auto insert = take(transaction.prepare(
                "INSERT INTO transaction_values(value) VALUES(?1);"));
            take(insert.bindInt64(1, 1));
            require(take(insert.step()) == WinsqliteStepResult::Done,
                    "transaction statement did not complete");
        }
        take(transaction.commit());
        require(!transaction.isActive(), "committed transaction remained active");
    }
    require(queryInteger(connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
            "committed transaction was not durable on the connection");

    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        take(transaction.execute(
            "INSERT INTO transaction_values(value) VALUES(2);"
            "INSERT INTO transaction_values(value) VALUES(3);"));
        take(transaction.rollback());
    }
    require(queryInteger(connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
            "explicit rollback retained transaction writes");

    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        auto escaped = take(transaction.prepare(
            "INSERT INTO transaction_values(value) VALUES(?1);"));
        take(transaction.commit());
        const auto queryStarted = std::chrono::steady_clock::now();
        require(queryInteger(
                    connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
                "the committed transaction did not release its connection admission lock");
        require(std::chrono::steady_clock::now() - queryStarted < 1s,
                "an escaped statement delayed connection reuse after commit");
        requireError(
            escaped.bindInt64(1, 5), Domain::ErrorCodes::InvalidRequest,
            "a transaction statement accepted binding after commit");
        requireError(
            escaped.step(), Domain::ErrorCodes::InvalidRequest,
            "a transaction statement executed in autocommit mode after commit");
        requireError(
            escaped.reset(), Domain::ErrorCodes::InvalidRequest,
            "a transaction statement reset after commit");
    }
    require(queryInteger(connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
            "a post-commit transaction statement escaped into autocommit mode");

    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        auto escaped = take(transaction.prepare(
            "INSERT INTO transaction_values(value) VALUES(?1);"));
        take(escaped.bindInt64(1, 6));
        take(transaction.rollback());
        const auto queryStarted = std::chrono::steady_clock::now();
        require(queryInteger(
                    connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
                "the rolled-back transaction did not release its connection admission lock");
        require(std::chrono::steady_clock::now() - queryStarted < 1s,
                "an escaped statement delayed connection reuse after rollback");
        requireError(
            escaped.bindInt64(1, 7), Domain::ErrorCodes::InvalidRequest,
            "a transaction statement accepted binding after rollback");
        requireError(
            escaped.step(), Domain::ErrorCodes::InvalidRequest,
            "a transaction statement executed in autocommit mode after rollback");
        requireError(
            escaped.reset(), Domain::ErrorCodes::InvalidRequest,
            "a transaction statement reset after rollback");
    }
    require(queryInteger(connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
            "a post-rollback transaction statement escaped into autocommit mode");

    std::stop_source cancellation;
    auto cancellableContext = context;
    cancellableContext.cancellation = cancellation.get_token();
    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, cancellableContext));
        take(transaction.execute("INSERT INTO transaction_values(value) VALUES(4);"));
        cancellation.request_stop();
    }
    require(queryInteger(connection, "SELECT COUNT(*) FROM transaction_values;", context) == 1,
            "RAII rollback failed after its operation context was cancelled");

    take(connection.close(context));
}

void testConcurrentTransactionStatementEndSerialization()
{
    ScopedTestDirectory directory{L"kernel-transaction-concurrency"};
    auto environment = KernelEnvironment::create(
        directory.path(), L"transaction-concurrency.sqlite");
    const auto context = activeContext("p07-kernel-transaction-concurrency");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(connection.execute(
        "CREATE TABLE concurrent_values(value INTEGER NOT NULL);", context));

    const auto runEndRace = [&](const bool commit, const std::int64_t value) {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        auto statement = take(transaction.prepare(
            "INSERT INTO concurrent_values(value) VALUES(?1);"));
        take(statement.bindInt64(1, value));

        std::barrier admitted{2};
        std::barrier release{2};
        std::optional<Domain::Result<WinsqliteStepResult>> stepResult;
        std::optional<Domain::Result<void>> endResult;
        std::atomic<bool> endStarted{};
        std::atomic<bool> endFinished{};
        std::jthread stepper{[&] {
            stepResult.emplace(
                PersistenceDetail::WinsqliteStatementTestAccess::
                    stepAfterAdmissionBarrier(statement, admitted, release));
        }};
        admitted.arrive_and_wait();
        std::jthread ender{[&] {
            endStarted.store(true, std::memory_order_release);
            endResult.emplace(commit ? transaction.commit() : transaction.rollback());
            endFinished.store(true, std::memory_order_release);
        }};
        while (!endStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        ::Sleep(20U);
        const bool endWaitedForStep = !endFinished.load(std::memory_order_acquire);
        release.arrive_and_wait();
        stepper.join();
        ender.join();

        require(endWaitedForStep,
                "commit or rollback did not serialize behind an admitted statement step");
        require(stepResult.has_value() &&
                    take(std::move(stepResult.value())) == WinsqliteStepResult::Done,
                "the admitted transaction statement did not finish before transaction end");
        require(endResult.has_value(), "the concurrent transaction end returned no result");
        take(std::move(endResult.value()));
        requireError(
            statement.step(), Domain::ErrorCodes::InvalidRequest,
            "an admitted statement remained usable after concurrent transaction end");
    };

    runEndRace(true, 1);
    require(queryInteger(connection, "SELECT COUNT(*) FROM concurrent_values;", context) == 1,
            "a serialized concurrent commit did not retain the admitted write");
    runEndRace(false, 2);
    require(queryInteger(connection, "SELECT COUNT(*) FROM concurrent_values;", context) == 1,
            "a serialized concurrent rollback retained the admitted write");

    for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            connection, context));
        auto source = take(transaction.prepare("SELECT 1;"));
        auto destination = take(transaction.prepare("SELECT 2;"));
        std::barrier start{3};
        std::optional<Domain::Result<void>> endResult;
        std::jthread mover{[&] {
            start.arrive_and_wait();
            destination = std::move(source);
        }};
        std::jthread ender{[&] {
            start.arrive_and_wait();
            endResult.emplace((attempt % 2U) == 0U
                                  ? transaction.commit()
                                  : transaction.rollback());
        }};
        start.arrive_and_wait();
        mover.join();
        ender.join();
        require(endResult.has_value(), "the move-versus-end race returned no result");
        take(std::move(endResult.value()));
        requireError(
            destination.step(), Domain::ErrorCodes::InvalidRequest,
            "a moved statement escaped concurrent transaction end");
        const auto reuseStarted = std::chrono::steady_clock::now();
        require(queryInteger(connection, "SELECT COUNT(*) FROM concurrent_values;", context) == 1,
                "move-versus-end retained the connection admission lock");
        require(std::chrono::steady_clock::now() - reuseStarted < 1s,
                "move-versus-end delayed immediate connection reuse");
    }

    take(connection.close(context));
}

void testCancellationAndDeadlineCallbacks()
{
    ScopedTestDirectory directory{L"kernel-cancellation"};
    auto environment = KernelEnvironment::create(directory.path(), L"cancellation.sqlite");
    const auto context = activeContext("p07-kernel-cancellation");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);

    std::stop_source preCancelled;
    preCancelled.request_stop();
    auto cancelledContext = context;
    cancelledContext.cancellation = preCancelled.get_token();
    const auto cancelledPrepare = connection.prepare("SELECT 1;", cancelledContext);
    requireError(cancelledPrepare, Domain::ErrorCodes::Cancelled,
                 "pre-cancelled statement preparation was admitted");

    auto expiredContext = context;
    expiredContext.deadline = std::chrono::steady_clock::now();
    const auto expiredExecute = connection.execute("SELECT 1;", expiredContext);
    requireError(expiredExecute, Domain::ErrorCodes::DeadlineExceeded,
                 "expired SQL execution was admitted");

    std::stop_source runningCancellation;
    auto runningContext = context;
    runningContext.cancellation = runningCancellation.get_token();
    std::jthread canceller{[&runningCancellation] {
        std::this_thread::sleep_for(10ms);
        runningCancellation.request_stop();
    }};
    const auto interrupted = connection.execute(
        "WITH RECURSIVE counter(value) AS ("
        "VALUES(0) UNION ALL SELECT value + 1 FROM counter WHERE value < 1000000000) "
        "SELECT MAX(value) FROM counter;",
        runningContext);
    canceller.join();
    requireError(interrupted, Domain::ErrorCodes::Cancelled,
                 "the progress callback did not interrupt cancelled SQL");

    const auto deadlineContext = boundedContext(10ms);
    const auto deadlineInterrupted = connection.execute(
        "WITH RECURSIVE counter(value) AS ("
        "VALUES(0) UNION ALL SELECT value + 1 FROM counter WHERE value < 1000000000) "
        "SELECT MAX(value) FROM counter;",
        deadlineContext);
    requireError(deadlineInterrupted, Domain::ErrorCodes::DeadlineExceeded,
                 "the progress callback did not interrupt expired SQL");
    require(queryInteger(connection, "SELECT 1;", context) == 1,
            "an interrupted statement made the connection unusable");

    take(connection.close(context));
}

void testBusyTimeoutAndInterruptionMapping()
{
    ScopedTestDirectory directory{L"kernel-busy"};
    auto environment = KernelEnvironment::create(directory.path(), L"busy.sqlite");
    const auto context = activeContext("p07-kernel-busy");
    auto owner = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(owner.execute("CREATE TABLE busy_values(value INTEGER NOT NULL);", context));
    auto contender = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);

    auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
        owner, context));
    take(transaction.execute("INSERT INTO busy_values(value) VALUES(1);"));

    std::stop_source busyCancellation;
    auto cancellationContext = boundedContext(5s, busyCancellation.get_token());
    std::jthread canceller{[&busyCancellation] {
        std::this_thread::sleep_for(10ms);
        busyCancellation.request_stop();
    }};
    const auto cancelled = PersistenceDetail::WinsqliteTransaction::beginImmediate(
        contender, cancellationContext);
    canceller.join();
    requireError(cancelled, Domain::ErrorCodes::Cancelled,
                 "busy-handler cancellation was not mapped to cancelled");

    const auto deadline = PersistenceDetail::WinsqliteTransaction::beginImmediate(
        contender, boundedContext(25ms));
    requireError(deadline, Domain::ErrorCodes::DeadlineExceeded,
                 "busy-handler deadline was not mapped to deadline_exceeded");

    const auto busyStarted = std::chrono::steady_clock::now();
    const auto busy = PersistenceDetail::WinsqliteTransaction::beginImmediate(
        contender, boundedContext(5s));
    const auto busyElapsed = std::chrono::steady_clock::now() - busyStarted;
    requireError(busy, Domain::ErrorCodes::DatabaseBusy,
                 "three-second busy bound was not mapped to database_busy");
    require(busyElapsed >= 2500ms && busyElapsed < 4500ms,
            "database busy handling did not respect its bounded retry window");

    take(transaction.rollback());
    {
        auto recovered = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            contender, context));
        take(recovered.rollback());
    }
    take(contender.close(context));
    take(owner.close(context));
}

void testOnlineBackupPrimitive()
{
    ScopedTestDirectory directory{L"kernel-backup"};
    auto sourceEnvironment = KernelEnvironment::create(
        directory.path(), L"backup-source.sqlite");
    auto destinationEnvironment = KernelEnvironment::create(
        directory.path(), L"backup-destination.sqlite");
    const auto context = activeContext("p07-kernel-backup");
    {
        auto writableSource = openDatabase(
            *sourceEnvironment, WinsqliteOpenMode::ReadWriteCreate, context);
        take(writableSource.execute(
            "CREATE TABLE backup_values(value TEXT NOT NULL);"
            "INSERT INTO backup_values(value) VALUES('alpha');"
            "INSERT INTO backup_values(value) VALUES('beta');",
            context));
        take(writableSource.close(context));
    }
    auto source = openDatabase(
        *sourceEnvironment, WinsqliteOpenMode::ReadOnlyExisting, context);
    auto destination = openDatabase(
        *destinationEnvironment, WinsqliteOpenMode::ReadWriteCreate, context);

    const auto sameConnection = PersistenceDetail::WinsqliteBackup::begin(
        source, source, context);
    requireError(sameConnection, Domain::ErrorCodes::InvalidRequest,
                 "online backup accepted the same source and destination connection");

    {
        auto backup = take(PersistenceDetail::WinsqliteBackup::begin(
            source, destination, context));
        const auto invalidChunk = backup.step(0);
        requireError(invalidChunk, Domain::ErrorCodes::InvalidRequest,
                     "online backup accepted a zero-page chunk");
        const auto progress = take(backup.runToCompletion(1));
        require(progress.complete && backup.isComplete(),
                "online backup did not report completion");
        require(progress.remainingPages == 0 && backup.remainingPages() == 0,
                "online backup retained remaining pages after completion");
        require(progress.totalPages > 0 && backup.totalPages() == progress.totalPages,
                "online backup did not expose a bounded page count");
        take(backup.finish());
    }

    require(queryInteger(destination, "SELECT COUNT(*) FROM backup_values;", context) == 2,
            "online backup did not copy source rows");
    require(queryText(destination,
                      "SELECT value FROM backup_values ORDER BY value LIMIT 1;", context) ==
                "alpha",
            "online backup changed copied text");
    take(destination.close(context));
    take(source.close(context));
    require(sourceEnvironment->openFileCount() == 0U &&
                destinationEnvironment->openFileCount() == 0U,
            "online backup leaked a VFS file owner");
}

void testExplicitCloseWithOutstandingOwnership()
{
    ScopedTestDirectory directory{L"kernel-close"};
    auto environment = KernelEnvironment::create(directory.path(), L"close.sqlite");
    const auto context = activeContext("p07-kernel-close");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);

    {
        auto outstanding = take(connection.prepare("SELECT 1;", context));
        const auto closeStarted = std::chrono::steady_clock::now();
        const auto busyClose = connection.close(boundedContext(5s));
        const auto closeElapsed = std::chrono::steady_clock::now() - closeStarted;
        requireError(busyClose, Domain::ErrorCodes::DatabaseBusy,
                     "explicit close ignored outstanding statement ownership");
        require(closeElapsed >= 2500ms && closeElapsed < 4500ms,
                "explicit close did not use the bounded owner wait");
        require(take(outstanding.step()) == WinsqliteStepResult::Row,
                "failed close invalidated the outstanding statement");
    }

    take(connection.close(context));
    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "successful explicit close retained VFS files");
}

void testQuarantineClosePoisonsBeforeFallbackRelease()
{
    ScopedTestDirectory directory{L"kernel-quarantine-close"};
    auto environment = KernelEnvironment::create(
        directory.path(), L"quarantine-close.sqlite");
    const auto context = activeContext("p07-kernel-quarantine-close");
    std::optional<WinsqliteConnection> connection{openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context)};
    take(connection->execute(
        "PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE quarantine_close_probe(value INTEGER NOT NULL);"
        "INSERT INTO quarantine_close_probe(value) VALUES(41);",
        context));

    const std::filesystem::path walPath{
        environment->databasePath() + L"-wal"};
    const std::filesystem::path sharedMemoryPath{
        environment->databasePath() + L"-shm"};
    require(std::filesystem::is_regular_file(walPath) &&
                std::filesystem::is_regular_file(sharedMemoryPath),
            "the quarantine-close fixture did not retain WAL/SHM");
    const std::string walBytes = PersistenceSupport::readFixture(walPath);
    const std::string sharedMemoryBytes =
        PersistenceSupport::readFixture(sharedMemoryPath);

    auto statement = take(connection->prepare(
        "SELECT value FROM quarantine_close_probe;", context));
    NativeStatementOwner orphan{
        PersistenceDetail::WinsqliteStatementTestAccess::detachNativeStatement(
            statement)};
    require(static_cast<bool>(orphan),
            "the quarantine-close test could not detach its native statement");

    const auto failedClose = connection->closeForQuarantine(context);
    requireError(failedClose, Domain::ErrorCodes::DatabaseBusy,
                 "quarantine close did not expose native outstanding ownership");
    require(failedClose.error().message.find(
                "close the database for integrity quarantine") != std::string::npos,
            "quarantine close returned the wrong native-close diagnostic");
    const auto poisoned = connection->execute("SELECT 1;", context);
    requireError(poisoned, Domain::ErrorCodes::IntegrityFailure,
                 "failed quarantine close did not poison the connection");

    connection.reset();
    require(environment->openFileCount() > 0U,
            "fallback close released VFS ownership before the orphan finalized");
    require(std::filesystem::is_regular_file(walPath) &&
                PersistenceSupport::readFixture(walPath) == walBytes,
            "fallback close checkpointed, deleted, or changed the retained WAL");
    require(orphan.finalize() == SQLITE_OK,
            "the detached native statement could not be finalized");
    require(environment->openFileCount() == 0U,
            "finalizing the orphan did not complete deferred native close");
    require(std::filesystem::is_regular_file(sharedMemoryPath) &&
                PersistenceSupport::readFixture(sharedMemoryPath) ==
                    sharedMemoryBytes,
            "deferred native close deleted or changed retained shared memory");

    auto reopened = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    require(queryInteger(
                reopened,
                "SELECT value FROM quarantine_close_probe;", context) == 41,
            "the deferred-close database did not reopen with its WAL-resident row");
    take(reopened.close(context));
    require(environment->openFileCount() == 0U,
            "the reopened quarantine-close fixture retained a VFS owner");
}

} // namespace

void registerWinsqliteKernelTests(TestRegistry& tests)
{
    addTest(tests, "persistence.kernel.open-modes", testOpenModesAndReadOnlyPreservation);
    addTest(tests, "persistence.kernel.defensive-settings",
            testDefensiveConnectionSettingsAndAuthorizer);
    addTest(tests, "persistence.kernel.typed-statements", testTypedStatementsAndBounds);
    addTest(tests, "persistence.kernel.complete-sql-script", testCompleteSqlScriptExecution);
    addTest(tests, "persistence.kernel.transactions", testTransactionCommitAndRollbackOwnership);
    addTest(tests, "persistence.kernel.transaction-concurrency",
            testConcurrentTransactionStatementEndSerialization);
    addTest(tests, "persistence.kernel.cancellation-deadline",
            testCancellationAndDeadlineCallbacks);
    addTest(tests, "persistence.kernel.busy-timeout", testBusyTimeoutAndInterruptionMapping);
    addTest(tests, "persistence.kernel.online-backup", testOnlineBackupPrimitive);
    addTest(tests, "persistence.kernel.explicit-close",
            testExplicitCloseWithOutstandingOwnership);
    addTest(tests, "persistence.kernel.quarantine-close-fallback",
            testQuarantineClosePoisonsBeforeFallbackRelease);
}

} // namespace ForgeConductor::Tests
