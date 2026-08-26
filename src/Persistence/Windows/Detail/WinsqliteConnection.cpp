#include "WinsqliteConnection.h"

#include "../../../Infrastructure/Windows/Detail/UtfConversion.h"
#include "AnchoredSqliteVfs.h"
#include "WinsqliteError.h"
#include "WinsqliteOperationGuard.h"
#include "WinsqliteStatement.h"

#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

struct ConnectionLimit final {
    int category;
    int maximum;
};

inline constexpr std::array<ConnectionLimit, 11> ConnectionLimits{{
    {SQLITE_LIMIT_LENGTH, 16 * 1024 * 1024},
    {SQLITE_LIMIT_SQL_LENGTH, 1024 * 1024},
    {SQLITE_LIMIT_COLUMN, 256},
    {SQLITE_LIMIT_EXPR_DEPTH, 100},
    {SQLITE_LIMIT_COMPOUND_SELECT, 32},
    {SQLITE_LIMIT_VDBE_OP, 250'000},
    {SQLITE_LIMIT_FUNCTION_ARG, 32},
    {SQLITE_LIMIT_ATTACHED, 0},
    {SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 4 * 1024},
    {SQLITE_LIMIT_VARIABLE_NUMBER, 512},
    {SQLITE_LIMIT_TRIGGER_DEPTH, 32},
}};

[[nodiscard]] bool asciiEqualsIgnoreCase(
    const char* const value,
    const std::string_view expected) noexcept
{
    if (value == nullptr || std::strlen(value) != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        char current = value[index];
        if (current >= 'A' && current <= 'Z') {
            current = static_cast<char>(current - 'A' + 'a');
        }
        if (current != expected[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int connectionAuthorizer(
    void*,
    const int action,
    const char* const firstArgument,
    const char* const secondArgument,
    const char*,
    const char*) noexcept
{
    if (action == SQLITE_ATTACH || action == SQLITE_DETACH) {
        return SQLITE_DENY;
    }
    if (action == SQLITE_FUNCTION &&
        (asciiEqualsIgnoreCase(firstArgument, "load_extension") ||
         asciiEqualsIgnoreCase(secondArgument, "load_extension"))) {
        return SQLITE_DENY;
    }
    if (action == SQLITE_PRAGMA && secondArgument != nullptr &&
        (asciiEqualsIgnoreCase(firstArgument, "writable_schema") ||
         asciiEqualsIgnoreCase(firstArgument, "trusted_schema") ||
         asciiEqualsIgnoreCase(firstArgument, "journal_mode") ||
         asciiEqualsIgnoreCase(firstArgument, "synchronous") ||
         asciiEqualsIgnoreCase(firstArgument, "foreign_keys"))) {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

[[nodiscard]] Domain::Result<void> configureBooleanOption(
    sqlite3* const database,
    const int option,
    const int requestedValue,
    const std::string_view action,
    const Domain::OperationContext& context) noexcept
{
    int effectiveValue = -1;
    const int result = sqlite3_db_config(database, option, requestedValue, &effectiveValue);
    if (result != SQLITE_OK) {
        return Domain::Result<void>::failure(makeWinsqliteError(
            result,
            action,
            sqlite3_errmsg(database),
            WinsqliteInterruptionReason::None,
            &context));
    }
    if (effectiveValue != requestedValue) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Winsqlite3 did not apply the required defensive connection option."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> configureLimits(sqlite3* const database) noexcept
{
    try {
        for (const auto& limit : ConnectionLimits) {
            static_cast<void>(sqlite3_limit(database, limit.category, limit.maximum));
            const int effective = sqlite3_limit(database, limit.category, -1);
            if (effective < 0 || effective > limit.maximum) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Winsqlite3 did not enforce a required per-connection resource limit."));
            }
        }
        static_cast<void>(sqlite3_limit(database, SQLITE_LIMIT_WORKER_THREADS, 0));
        if (sqlite3_limit(database, SQLITE_LIMIT_WORKER_THREADS, -1) != 0) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Winsqlite3 did not disable auxiliary worker threads for the connection."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Winsqlite3 connection limits could not be configured."));
    }
}

} // namespace

Domain::Result<std::int64_t> WinsqliteConnection::readIntegerPragma(
    const std::shared_ptr<WinsqliteOperationState>& operation,
    const std::string_view sql) noexcept
{
    auto prepared = WinsqliteStatement::prepareInOperation(operation, sql);
    if (!prepared) {
        return Domain::Result<std::int64_t>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto firstStep = statement.step();
    if (!firstStep) {
        return Domain::Result<std::int64_t>::failure(std::move(firstStep).error());
    }
    if (firstStep.value() != WinsqliteStepResult::Row) {
        return Domain::Result<std::int64_t>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 returned no row for a required integer pragma."));
    }
    auto value = statement.columnInt64(0);
    if (!value) {
        return value;
    }
    const std::int64_t copiedValue = value.value();
    auto finalStep = statement.step();
    if (!finalStep) {
        return Domain::Result<std::int64_t>::failure(std::move(finalStep).error());
    }
    if (finalStep.value() != WinsqliteStepResult::Done) {
        return Domain::Result<std::int64_t>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 returned multiple rows for a scalar integer pragma."));
    }
    return Domain::Result<std::int64_t>::success(copiedValue);
}

Domain::Result<std::string> WinsqliteConnection::readTextPragma(
    const std::shared_ptr<WinsqliteOperationState>& operation,
    const std::string_view sql) noexcept
{
    auto prepared = WinsqliteStatement::prepareInOperation(operation, sql);
    if (!prepared) {
        return Domain::Result<std::string>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto firstStep = statement.step();
    if (!firstStep) {
        return Domain::Result<std::string>::failure(std::move(firstStep).error());
    }
    if (firstStep.value() != WinsqliteStepResult::Row) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 returned no row for a required text pragma."));
    }
    auto value = statement.columnText(0, 32U);
    if (!value) {
        return Domain::Result<std::string>::failure(std::move(value).error());
    }
    if (!value.value().has_value()) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 returned null for a required text pragma."));
    }
    std::string copiedValue = std::move(value).value().value();
    auto finalStep = statement.step();
    if (!finalStep) {
        return Domain::Result<std::string>::failure(std::move(finalStep).error());
    }
    if (finalStep.value() != WinsqliteStepResult::Done) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 returned multiple rows for a scalar text pragma."));
    }
    return Domain::Result<std::string>::success(std::move(copiedValue));
}

Domain::Result<void> WinsqliteConnection::configureConnection(
    const std::shared_ptr<WinsqliteOperationState>& operation,
    const WinsqliteConnectionOptions& options,
    const Domain::OperationContext& context) noexcept
{
    sqlite3* const database = WinsqliteOperationGuard::database(operation);
    if (database == nullptr) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 connection disappeared during configuration."));
    }

    const int reportedReadOnly = sqlite3_db_readonly(database, "main");
    const int expectedReadOnly =
        options.openMode == WinsqliteOpenMode::ReadOnlyExisting ? 1 : 0;
    if (reportedReadOnly < 0 || reportedReadOnly != expectedReadOnly) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 did not honor the requested database access mode."));
    }

    int result = sqlite3_extended_result_codes(database, 1);
    if (result != SQLITE_OK) {
        return Domain::Result<void>::failure(makeWinsqliteError(
            result,
            "enable extended result codes",
            sqlite3_errmsg(database),
            WinsqliteInterruptionReason::None,
            &context));
    }

    auto configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_DEFENSIVE, 1,
        "enable defensive connection mode", context);
    if (!configured) {
        return configured;
    }
    configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0,
        "disable trusted schema behavior", context);
    if (!configured) {
        return configured;
    }
    configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0,
        "disable loadable SQL extensions", context);
    if (!configured) {
        return configured;
    }
    configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_DQS_DML, 0,
        "disable double-quoted string literals in DML", context);
    if (!configured) {
        return configured;
    }
    configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_DQS_DDL, 0,
        "disable double-quoted string literals in DDL", context);
    if (!configured) {
        return configured;
    }
    configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1,
        "disable implicit WAL checkpointing during native close", context);
    if (!configured) {
        return configured;
    }
#ifdef SQLITE_DBCONFIG_WRITABLE_SCHEMA
    configured = configureBooleanOption(
        database, SQLITE_DBCONFIG_WRITABLE_SCHEMA, 0,
        "disable writable schema mode", context);
    if (!configured) {
        return configured;
    }
#endif
    result = sqlite3_enable_load_extension(database, 0);
    if (result != SQLITE_OK) {
        return Domain::Result<void>::failure(makeWinsqliteError(
            result,
            "disable the load-extension interface",
            sqlite3_errmsg(database),
            WinsqliteInterruptionReason::None,
            &context));
    }

    configured = configureLimits(database);
    if (!configured) {
        return configured;
    }

    configured = WinsqliteStatement::executeScriptInOperation(
        operation, "PRAGMA foreign_keys=ON;");
    if (!configured) {
        return configured;
    }
    auto foreignKeys = readIntegerPragma(operation, "PRAGMA foreign_keys;");
    if (!foreignKeys) {
        return Domain::Result<void>::failure(std::move(foreignKeys).error());
    }
    if (foreignKeys.value() != 1) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 did not enable foreign-key enforcement."));
    }

    configured = WinsqliteStatement::executeScriptInOperation(
        operation, "PRAGMA temp_store=MEMORY;");
    if (!configured) {
        return configured;
    }
    auto temporaryStore = readIntegerPragma(operation, "PRAGMA temp_store;");
    if (!temporaryStore) {
        return Domain::Result<void>::failure(std::move(temporaryStore).error());
    }
    if (temporaryStore.value() != 2) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Winsqlite3 did not confine temporary storage to memory."));
    }

    if (options.openMode == WinsqliteOpenMode::ReadOnlyExisting) {
        configured = WinsqliteStatement::executeScriptInOperation(
            operation, "PRAGMA query_only=ON;");
        if (!configured) {
            return configured;
        }
    } else {
        const std::string_view journalModeSql =
            options.journalMode == WinsqliteJournalMode::WriteAheadLog
                ? "PRAGMA journal_mode=WAL;"
                : "PRAGMA journal_mode=DELETE;";
        configured = WinsqliteStatement::executeScriptInOperation(
            operation, journalModeSql);
        if (!configured) {
            return configured;
        }
        configured = WinsqliteStatement::executeScriptInOperation(
            operation,
            options.synchronousMode == WinsqliteSynchronousMode::Full
                ? "PRAGMA synchronous=FULL;"
                : "PRAGMA synchronous=NORMAL;");
        if (!configured) {
            return configured;
        }

        auto journalMode = readTextPragma(operation, "PRAGMA journal_mode;");
        if (!journalMode) {
            return Domain::Result<void>::failure(std::move(journalMode).error());
        }
        const std::string_view expectedJournalMode =
            options.journalMode == WinsqliteJournalMode::WriteAheadLog
                ? "wal"
                : "delete";
        if (journalMode.value() != expectedJournalMode) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Winsqlite3 did not enter the required journal mode."));
        }

        auto synchronous = readIntegerPragma(operation, "PRAGMA synchronous;");
        if (!synchronous) {
            return Domain::Result<void>::failure(std::move(synchronous).error());
        }
        const std::int64_t expectedSynchronous =
            options.synchronousMode == WinsqliteSynchronousMode::Full ? 2 : 1;
        if (synchronous.value() != expectedSynchronous) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Winsqlite3 did not apply the required synchronous policy."));
        }
    }

    result = sqlite3_set_authorizer(database, &connectionAuthorizer, nullptr);
    if (result != SQLITE_OK) {
        return Domain::Result<void>::failure(makeWinsqliteError(
            result,
            "install the defensive SQL authorizer",
            sqlite3_errmsg(database),
            WinsqliteInterruptionReason::None,
            &context));
    }

    return Domain::Result<void>::success();
}

WinsqliteConnection::State::~State() noexcept
{
    if (database_ != nullptr) {
        // The destructor has neither an operation deadline nor an error channel.
        // Connection close never owns WAL checkpoint work; fallback destruction
        // performs only native release and must never touch a poisoned WAL.
        static_cast<void>(sqlite3_close_v2(database_));
        database_ = nullptr;
    }
}

Domain::Result<WinsqliteConnection> WinsqliteConnection::open(
    const std::wstring_view databasePath,
    const WinsqliteConnectionOptions& options,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The database open operation was cancelled before it started."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The database open deadline expired before it started."));
        }
        if (databasePath.empty() || databasePath.size() > 32'768U ||
            databasePath.find(L'\0') != std::wstring_view::npos) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 database path must contain 1 through 32768 non-null code units."));
        }
        if (options.vfsName.empty() || options.vfsName.size() > 128U ||
            options.vfsName.find('\0') != std::string::npos) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A registered non-default SQLite VFS name must contain 1 through 128 bytes."));
        }
        if (!options.namespaceAuthority ||
            options.namespaceAuthority->canonicalMainDatabasePath() != databasePath) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 connection requires the exact anchored database namespace."));
        }

        sqlite3_vfs* const requestedVfs = sqlite3_vfs_find(options.vfsName.c_str());
        sqlite3_vfs* const defaultVfs = sqlite3_vfs_find(nullptr);
        if (requestedVfs == nullptr) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The requested application-owned SQLite VFS is not registered."));
        }
        if (requestedVfs == defaultVfs) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The default SQLite VFS cannot satisfy the anchored database-open contract."));
        }
        if (!AnchoredSqliteVfs::registrationBelongsTo(
                options.vfsName, *options.namespaceAuthority)) {
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The requested SQLite VFS is not owned by the supplied database namespace."));
        }

        auto utf8Path = Infrastructure::Windows::Detail::strictUtf16ToUtf8(databasePath);
        if (!utf8Path) {
            return Domain::Result<WinsqliteConnection>::failure(std::move(utf8Path).error());
        }

        int accessFlags = 0;
        bool readOnly = false;
        switch (options.openMode) {
        case WinsqliteOpenMode::ReadOnlyExisting:
            accessFlags = SQLITE_OPEN_READONLY;
            readOnly = true;
            break;
        case WinsqliteOpenMode::ReadWriteExisting:
            accessFlags = SQLITE_OPEN_READWRITE;
            break;
        case WinsqliteOpenMode::ReadWriteCreate:
            accessFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            break;
        default:
            return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Winsqlite3 open mode is invalid."));
        }

        sqlite3* database = nullptr;
        const int openFlags = accessFlags | SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_EXRESCODE;
        const int openResult = sqlite3_open_v2(
            utf8Path.value().c_str(), &database, openFlags, options.vfsName.c_str());
        if (openResult != SQLITE_OK) {
            Domain::Error error = makeWinsqliteError(
                openResult,
                "open the database through its application-owned VFS",
                database != nullptr ? sqlite3_errmsg(database) : sqlite3_errstr(openResult),
                WinsqliteInterruptionReason::None,
                &context);
            if (database != nullptr) {
                static_cast<void>(sqlite3_close_v2(database));
            }
            return Domain::Result<WinsqliteConnection>::failure(std::move(error));
        }

        std::shared_ptr<State> state;
        try {
            state = std::make_shared<State>(
                database, readOnly, options.namespaceAuthority);
        } catch (...) {
            static_cast<void>(sqlite3_close_v2(database));
            throw;
        }
        WinsqliteConnection connection{std::move(state)};

        auto guard = WinsqliteOperationGuard::acquire(connection, context);
        if (!guard) {
            return Domain::Result<WinsqliteConnection>::failure(std::move(guard).error());
        }
        auto operation = guard.value().shareState();
        auto configured = configureConnection(operation, options, context);
        if (!configured) {
            return Domain::Result<WinsqliteConnection>::failure(
                std::move(configured).error());
        }

        return Domain::Result<WinsqliteConnection>::success(std::move(connection));
    } catch (...) {
        return Domain::Result<WinsqliteConnection>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 connection could not be opened with bounded resources."));
    }
}

WinsqliteConnection::~WinsqliteConnection() noexcept = default;

WinsqliteConnection::WinsqliteConnection(WinsqliteConnection&& other) noexcept
    : state_{std::move(other.state_)}
{
}

WinsqliteConnection& WinsqliteConnection::operator=(WinsqliteConnection&& other) noexcept
{
    if (this != &other) {
        state_ = std::move(other.state_);
    }
    return *this;
}

bool WinsqliteConnection::belongsToNamespace(
    const DatabaseNamespaceLease& namespaceAuthority) const noexcept
{
    return state_ != nullptr &&
           state_->namespaceAuthority_.get() == std::addressof(namespaceAuthority);
}

Domain::Result<WinsqliteStatement> WinsqliteConnection::prepare(
    const std::string_view sql,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto guard = WinsqliteOperationGuard::acquire(*this, context);
        if (!guard) {
            return Domain::Result<WinsqliteStatement>::failure(std::move(guard).error());
        }
        auto operation = guard.value().releaseState();
        return WinsqliteStatement::prepareInOperation(operation, sql);
    } catch (...) {
        return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 statement operation could not be created."));
    }
}

Domain::Result<void> WinsqliteConnection::execute(
    const std::string_view sql,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto guard = WinsqliteOperationGuard::acquire(*this, context);
        if (!guard) {
            return Domain::Result<void>::failure(std::move(guard).error());
        }
        return WinsqliteStatement::executeScriptInOperation(guard.value().shareState(), sql);
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 SQL script could not be executed."));
    }
}

Domain::Result<void> WinsqliteConnection::close(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (state_ == nullptr) {
            return Domain::Result<void>::success();
        }
        auto guard = WinsqliteOperationGuard::acquireForClose(*this, context);
        if (!guard) {
            return Domain::Result<void>::failure(std::move(guard).error());
        }
        auto operation = guard.value().shareState();
        sqlite3* const database = WinsqliteOperationGuard::database(operation);
        if (database == nullptr) {
            return Domain::Result<void>::success();
        }

        const int closeResult = sqlite3_close(database);
        if (closeResult != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation, closeResult, "close the database"));
        }
        WinsqliteOperationGuard::markClosed(operation);
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 connection could not be closed deterministically."));
    }
}

Domain::Result<void> WinsqliteConnection::closeForQuarantine(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (state_ == nullptr) {
            return Domain::Result<void>::success();
        }
        auto guard = WinsqliteOperationGuard::acquireForClose(*this, context);
        if (!guard) {
            return Domain::Result<void>::failure(std::move(guard).error());
        }
        auto operation = guard.value().shareState();
        sqlite3* const database = WinsqliteOperationGuard::database(operation);
        if (database == nullptr) {
            return Domain::Result<void>::success();
        }
        WinsqliteOperationGuard::markPoisoned(operation);
        const int closeResult = sqlite3_close(database);
        if (closeResult != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation, closeResult,
                "close the database for integrity quarantine"));
        }
        WinsqliteOperationGuard::markClosed(operation);
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 connection could not be released for integrity quarantine."));
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
