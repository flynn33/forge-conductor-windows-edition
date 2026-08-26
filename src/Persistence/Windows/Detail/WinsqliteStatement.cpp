#include "WinsqliteStatement.h"

#include "WinsqliteOperationGuard.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

inline constexpr std::size_t MaximumSqlBytes = 1024U * 1024U;
inline constexpr std::size_t MaximumValueBytes = 16U * 1024U * 1024U;

[[nodiscard]] Domain::Result<void> invalidStatementState(const char* const message) noexcept
{
    return Domain::Result<void>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
}

[[nodiscard]] bool onlyTrailingWhitespace(const char* tail, const char* const end) noexcept
{
    if (tail == nullptr) {
        return true;
    }
    while (tail < end) {
        switch (*tail) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
        case '\f':
        case '\v':
            ++tail;
            break;
        default:
            return false;
        }
    }
    return true;
}

} // namespace

WinsqliteStatement::WinsqliteStatement(
    sqlite3_stmt* const statement,
    std::shared_ptr<WinsqliteOperationState> operation,
    std::shared_ptr<WinsqliteTransactionLifetime> transactionLifetime) noexcept
    : statement_{statement}, operation_{std::move(operation)},
      transactionLifetime_{std::move(transactionLifetime)}
{
    registerTransactionStatement();
}

Domain::Result<WinsqliteStatement> WinsqliteStatement::prepareInOperation(
    const std::shared_ptr<WinsqliteOperationState>& operation,
    const std::string_view sql,
    std::shared_ptr<WinsqliteTransactionLifetime> transactionLifetime) noexcept
{
    try {
        auto contextCheck = WinsqliteOperationGuard::check(operation, "prepare SQL");
        if (!contextCheck) {
            return Domain::Result<WinsqliteStatement>::failure(
                std::move(contextCheck).error());
        }
        if (sql.empty()) {
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A prepared Winsqlite3 statement cannot be empty."));
        }
        if (sql.size() > MaximumSqlBytes ||
            sql.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Prepared SQL exceeds the 1048576-byte connection limit."));
        }
        if (sql.find('\0') != std::string_view::npos) {
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Prepared SQL cannot contain an embedded null byte."));
        }

        sqlite3* const database = WinsqliteOperationGuard::database(operation);
        if (database == nullptr) {
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The prepared statement has no open Winsqlite3 connection."));
        }

        sqlite3_stmt* statement = nullptr;
        const char* tail = nullptr;
        const int result = sqlite3_prepare_v3(
            database,
            sql.data(),
            static_cast<int>(sql.size()),
            SQLITE_PREPARE_PERSISTENT,
            &statement,
            &tail);
        if (result != SQLITE_OK) {
            Domain::Error error = WinsqliteOperationGuard::error(
                operation, result, "prepare SQL");
            if (statement != nullptr) {
                static_cast<void>(sqlite3_finalize(statement));
            }
            return Domain::Result<WinsqliteStatement>::failure(
                std::move(error));
        }
        if (statement == nullptr) {
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Prepared SQL did not contain a statement."));
        }
        if (tail == nullptr || tail < sql.data() || tail > sql.data() + sql.size()) {
            static_cast<void>(sqlite3_finalize(statement));
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Winsqlite3 returned an invalid prepared-statement tail."));
        }
        if (!onlyTrailingWhitespace(tail, sql.data() + sql.size())) {
            static_cast<void>(sqlite3_finalize(statement));
            return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A prepared Winsqlite3 operation must contain exactly one SQL statement."));
        }

        return Domain::Result<WinsqliteStatement>::success(
            WinsqliteStatement{
                statement, operation, std::move(transactionLifetime)});
    } catch (...) {
        return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 statement could not be prepared."));
    }
}

Domain::Result<void> WinsqliteStatement::executeScriptInOperation(
    const std::shared_ptr<WinsqliteOperationState>& operation,
    const std::string_view sql) noexcept
{
    try {
        auto contextCheck = WinsqliteOperationGuard::check(operation, "execute SQL");
        if (!contextCheck) {
            return contextCheck;
        }
        if (sql.empty()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 SQL script cannot be empty."));
        }
        if (sql.size() > MaximumSqlBytes ||
            sql.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A Winsqlite3 SQL script exceeds the 1048576-byte connection limit."));
        }
        if (sql.find('\0') != std::string_view::npos) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 SQL script cannot contain an embedded null byte."));
        }

        sqlite3* const database = WinsqliteOperationGuard::database(operation);
        if (database == nullptr) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The SQL script has no open Winsqlite3 connection."));
        }

        const char* cursor = sql.data();
        const char* const end = sql.data() + sql.size();
        bool compiledStatement = false;
        while (cursor < end) {
            contextCheck = WinsqliteOperationGuard::check(operation, "execute SQL");
            if (!contextCheck) {
                return contextCheck;
            }

            sqlite3_stmt* statement = nullptr;
            const char* tail = nullptr;
            const auto remaining = static_cast<int>(end - cursor);
            const int prepareResult = sqlite3_prepare_v3(
                database, cursor, remaining, 0, &statement, &tail);
            if (prepareResult != SQLITE_OK) {
                Domain::Error error = WinsqliteOperationGuard::error(
                    operation, prepareResult, "compile the complete SQL script");
                if (statement != nullptr) {
                    static_cast<void>(sqlite3_finalize(statement));
                }
                return Domain::Result<void>::failure(std::move(error));
            }
            if (tail == nullptr || tail <= cursor || tail > end) {
                if (statement != nullptr) {
                    static_cast<void>(sqlite3_finalize(statement));
                }
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Winsqlite3 did not advance while compiling the complete SQL script."));
            }
            cursor = tail;

            if (statement == nullptr) {
                continue;
            }
            compiledStatement = true;
            int stepResult = SQLITE_OK;
            do {
                stepResult = sqlite3_step(statement);
            } while (stepResult == SQLITE_ROW);

            std::optional<Domain::Error> stepError;
            if (stepResult != SQLITE_DONE) {
                stepError.emplace(WinsqliteOperationGuard::error(
                    operation, stepResult, "execute the complete SQL script"));
            }
            const int finalizeResult = sqlite3_finalize(statement);
            statement = nullptr;
            if (stepError.has_value()) {
                return Domain::Result<void>::failure(std::move(stepError.value()));
            }
            if (finalizeResult != SQLITE_OK) {
                return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                    operation, finalizeResult, "finalize the complete SQL script"));
            }
        }

        if (!compiledStatement) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 SQL script must contain at least one statement."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The complete bounded Winsqlite3 SQL script could not be executed."));
    }
}

WinsqliteStatement::~WinsqliteStatement() noexcept
{
    finalize();
}

WinsqliteStatement::WinsqliteStatement(WinsqliteStatement&& other) noexcept
{
    auto transactionLifetime = other.transactionLifetime_;
    std::unique_lock<std::recursive_mutex> lifetimeLock;
    if (transactionLifetime != nullptr) {
        lifetimeLock = std::unique_lock<std::recursive_mutex>{
            transactionLifetime->mutex};
    }
    statement_ = std::exchange(other.statement_, nullptr);
    operation_ = std::move(other.operation_);
    transactionLifetime_ = std::move(other.transactionLifetime_);
    rowAvailable_ = std::exchange(other.rowAvailable_, false);
    replaceTransactionRegistration(other);
}

WinsqliteStatement& WinsqliteStatement::operator=(WinsqliteStatement&& other) noexcept
{
    if (this != &other) {
        finalize();
        auto transactionLifetime = other.transactionLifetime_;
        std::unique_lock<std::recursive_mutex> lifetimeLock;
        if (transactionLifetime != nullptr) {
            lifetimeLock = std::unique_lock<std::recursive_mutex>{
                transactionLifetime->mutex};
        }
        statement_ = std::exchange(other.statement_, nullptr);
        operation_ = std::move(other.operation_);
        transactionLifetime_ = std::move(other.transactionLifetime_);
        rowAvailable_ = std::exchange(other.rowAvailable_, false);
        replaceTransactionRegistration(other);
    }
    return *this;
}

Domain::Result<void> WinsqliteStatement::validateTransactionLifetime() const noexcept
{
    if (transactionLifetime_ != nullptr && !transactionLifetime_->active) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "A transaction-owned statement cannot be used after its transaction ended."));
    }
    return Domain::Result<void>::success();
}

Domain::Result<std::unique_lock<std::recursive_mutex>>
WinsqliteStatement::lockTransactionLifetime() const noexcept
{
    try {
        std::unique_lock<std::recursive_mutex> lock;
        if (transactionLifetime_ != nullptr) {
            lock = std::unique_lock<std::recursive_mutex>{transactionLifetime_->mutex};
            if (!transactionLifetime_->active) {
                return Domain::Result<std::unique_lock<std::recursive_mutex>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "A transaction-owned statement cannot be used after its transaction ended."));
            }
        }
        return Domain::Result<std::unique_lock<std::recursive_mutex>>::success(
            std::move(lock));
    } catch (...) {
        return Domain::Result<std::unique_lock<std::recursive_mutex>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The transaction statement lifetime could not be serialized."));
    }
}

Domain::Result<void> WinsqliteStatement::validateParameterIndex(
    const int parameterIndex) const noexcept
{
    try {
        if (statement_ == nullptr || operation_ == nullptr) {
            return invalidStatementState(
                "The Winsqlite3 statement is finalized or was moved.");
        }
        auto activeTransaction = validateTransactionLifetime();
        if (!activeTransaction) {
            return activeTransaction;
        }
        const int count = sqlite3_bind_parameter_count(statement_);
        if (parameterIndex < 1 || parameterIndex > count) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Winsqlite3 parameter index is outside the prepared statement bounds."));
        }
        return WinsqliteOperationGuard::check(operation_, "bind a SQL parameter");
    } catch (...) {
        return invalidStatementState(
            "The Winsqlite3 parameter index could not be validated.");
    }
}

Domain::Result<void> WinsqliteStatement::validateColumnIndex(
    const int columnIndex) const noexcept
{
    try {
        if (statement_ == nullptr || operation_ == nullptr) {
            return invalidStatementState(
                "The Winsqlite3 statement is finalized or was moved.");
        }
        auto activeTransaction = validateTransactionLifetime();
        if (!activeTransaction) {
            return activeTransaction;
        }
        if (!rowAvailable_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Winsqlite3 column can be read only while a result row is current."));
        }
        const int count = sqlite3_column_count(statement_);
        if (columnIndex < 0 || columnIndex >= count) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Winsqlite3 column index is outside the current row bounds."));
        }
        return WinsqliteOperationGuard::check(operation_, "read a SQL result column");
    } catch (...) {
        return invalidStatementState(
            "The Winsqlite3 column index could not be validated.");
    }
}

Domain::Result<void> WinsqliteStatement::bindText(
    const int parameterIndex,
    const std::string_view value) noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<void>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateParameterIndex(parameterIndex);
        if (!validIndex) {
            return validIndex;
        }
        if (value.size() > MaximumValueBytes ||
            value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A Winsqlite3 text parameter exceeds the 16777216-byte value limit."));
        }
        const char* const bytes = value.empty() ? "" : value.data();
        const int result = sqlite3_bind_text(
            statement_, parameterIndex, bytes, static_cast<int>(value.size()),
            SQLITE_TRANSIENT);
        if (result != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation_, result, "bind a text parameter"));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return invalidStatementState("The Winsqlite3 text parameter could not be bound.");
    }
}

Domain::Result<void> WinsqliteStatement::bindInt64(
    const int parameterIndex,
    const std::int64_t value) noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<void>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateParameterIndex(parameterIndex);
        if (!validIndex) {
            return validIndex;
        }
        const int result = sqlite3_bind_int64(statement_, parameterIndex, value);
        if (result != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation_, result, "bind an integer parameter"));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return invalidStatementState("The Winsqlite3 integer parameter could not be bound.");
    }
}

Domain::Result<void> WinsqliteStatement::bindDouble(
    const int parameterIndex,
    const double value) noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<void>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateParameterIndex(parameterIndex);
        if (!validIndex) {
            return validIndex;
        }
        const int result = sqlite3_bind_double(statement_, parameterIndex, value);
        if (result != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation_, result, "bind a floating-point parameter"));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return invalidStatementState(
            "The Winsqlite3 floating-point parameter could not be bound.");
    }
}

Domain::Result<void> WinsqliteStatement::bindNull(const int parameterIndex) noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<void>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateParameterIndex(parameterIndex);
        if (!validIndex) {
            return validIndex;
        }
        const int result = sqlite3_bind_null(statement_, parameterIndex);
        if (result != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation_, result, "bind a null parameter"));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return invalidStatementState("The Winsqlite3 null parameter could not be bound.");
    }
}

Domain::Result<WinsqliteStepResult> WinsqliteStatement::step() noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            rowAvailable_ = false;
            return Domain::Result<WinsqliteStepResult>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        if (statement_ == nullptr || operation_ == nullptr) {
            return Domain::Result<WinsqliteStepResult>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Winsqlite3 statement is finalized or was moved."));
        }
        auto activeTransaction = validateTransactionLifetime();
        if (!activeTransaction) {
            rowAvailable_ = false;
            return Domain::Result<WinsqliteStepResult>::failure(
                std::move(activeTransaction).error());
        }
        auto contextCheck = WinsqliteOperationGuard::check(operation_, "step SQL");
        if (!contextCheck) {
            rowAvailable_ = false;
            return Domain::Result<WinsqliteStepResult>::failure(
                std::move(contextCheck).error());
        }

        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
            rowAvailable_ = true;
            return Domain::Result<WinsqliteStepResult>::success(WinsqliteStepResult::Row);
        }
        rowAvailable_ = false;
        if (result == SQLITE_DONE) {
            return Domain::Result<WinsqliteStepResult>::success(WinsqliteStepResult::Done);
        }
        return Domain::Result<WinsqliteStepResult>::failure(
            WinsqliteOperationGuard::error(operation_, result, "step SQL"));
    } catch (...) {
        rowAvailable_ = false;
        return Domain::Result<WinsqliteStepResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 statement could not be stepped."));
    }
}

Domain::Result<void> WinsqliteStatement::reset(const bool clearBindings) noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<void>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        if (statement_ == nullptr || operation_ == nullptr) {
            return invalidStatementState(
                "The Winsqlite3 statement is finalized or was moved.");
        }
        auto activeTransaction = validateTransactionLifetime();
        if (!activeTransaction) {
            return activeTransaction;
        }
        auto contextCheck = WinsqliteOperationGuard::check(operation_, "reset SQL");
        if (!contextCheck) {
            return contextCheck;
        }
        rowAvailable_ = false;
        const int resetResult = sqlite3_reset(statement_);
        if (resetResult != SQLITE_OK) {
            return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                operation_, resetResult, "reset SQL"));
        }
        if (clearBindings) {
            const int clearResult = sqlite3_clear_bindings(statement_);
            if (clearResult != SQLITE_OK) {
                return Domain::Result<void>::failure(WinsqliteOperationGuard::error(
                    operation_, clearResult, "clear SQL parameter bindings"));
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return invalidStatementState("The Winsqlite3 statement could not be reset.");
    }
}

int WinsqliteStatement::parameterCount() const noexcept
{
    auto lifetimeLock = lockTransactionLifetime();
    return lifetimeLock && statement_ != nullptr
               ? sqlite3_bind_parameter_count(statement_)
               : 0;
}

int WinsqliteStatement::columnCount() const noexcept
{
    auto lifetimeLock = lockTransactionLifetime();
    return lifetimeLock && statement_ != nullptr
               ? sqlite3_column_count(statement_)
               : 0;
}

Domain::Result<bool> WinsqliteStatement::columnIsNull(const int columnIndex) const noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<bool>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateColumnIndex(columnIndex);
        if (!validIndex) {
            return Domain::Result<bool>::failure(std::move(validIndex).error());
        }
        return Domain::Result<bool>::success(
            sqlite3_column_type(statement_, columnIndex) == SQLITE_NULL);
    } catch (...) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 null column could not be read."));
    }
}

Domain::Result<std::int64_t> WinsqliteStatement::columnInt64(
    const int columnIndex) const noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<std::int64_t>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateColumnIndex(columnIndex);
        if (!validIndex) {
            return Domain::Result<std::int64_t>::failure(std::move(validIndex).error());
        }
        if (sqlite3_column_type(statement_, columnIndex) != SQLITE_INTEGER) {
            return Domain::Result<std::int64_t>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The Winsqlite3 result column is not an integer."));
        }
        return Domain::Result<std::int64_t>::success(
            sqlite3_column_int64(statement_, columnIndex));
    } catch (...) {
        return Domain::Result<std::int64_t>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 integer column could not be read."));
    }
}

Domain::Result<double> WinsqliteStatement::columnDouble(const int columnIndex) const noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<double>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateColumnIndex(columnIndex);
        if (!validIndex) {
            return Domain::Result<double>::failure(std::move(validIndex).error());
        }
        const int columnType = sqlite3_column_type(statement_, columnIndex);
        if (columnType != SQLITE_FLOAT && columnType != SQLITE_INTEGER) {
            return Domain::Result<double>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The Winsqlite3 result column is not numeric."));
        }
        return Domain::Result<double>::success(sqlite3_column_double(statement_, columnIndex));
    } catch (...) {
        return Domain::Result<double>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 floating-point column could not be read."));
    }
}

Domain::Result<std::optional<std::string>> WinsqliteStatement::columnText(
    const int columnIndex,
    const std::size_t maximumBytes) const noexcept
{
    try {
        auto lifetimeLockResult = lockTransactionLifetime();
        if (!lifetimeLockResult) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(lifetimeLockResult).error());
        }
        auto lifetimeLock = std::move(lifetimeLockResult).value();
        static_cast<void>(lifetimeLock);
        auto validIndex = validateColumnIndex(columnIndex);
        if (!validIndex) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(validIndex).error());
        }
        const int columnType = sqlite3_column_type(statement_, columnIndex);
        if (columnType == SQLITE_NULL) {
            return Domain::Result<std::optional<std::string>>::success(std::nullopt);
        }
        if (columnType != SQLITE_TEXT) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The Winsqlite3 result column is not text."));
        }

        const auto* const text = sqlite3_column_text(statement_, columnIndex);
        const int byteCount = sqlite3_column_bytes(statement_, columnIndex);
        if (byteCount < 0) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Winsqlite3 returned an invalid negative text-column length."));
        }
        const auto boundedMaximum = (std::min)(maximumBytes, MaximumValueBytes);
        if (static_cast<std::size_t>(byteCount) > boundedMaximum) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The Winsqlite3 text column exceeds the caller's bounded response limit."));
        }

        if (text == nullptr && byteCount != 0) {
            return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Winsqlite3 returned no storage for a nonempty text column."));
        }
        const char* const bytes = text != nullptr
            ? reinterpret_cast<const char*>(text)
            : "";
        return Domain::Result<std::optional<std::string>>::success(
            std::string{bytes, static_cast<std::size_t>(byteCount)});
    } catch (...) {
        return Domain::Result<std::optional<std::string>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded Winsqlite3 text column could not be materialized."));
    }
}

void WinsqliteStatement::registerTransactionStatement() noexcept
{
    if (transactionLifetime_ == nullptr) {
        return;
    }
    std::scoped_lock lock{transactionLifetime_->mutex};
    transactionNext_ = transactionLifetime_->firstStatement;
    if (transactionNext_ != nullptr) {
        transactionNext_->transactionPrevious_ = this;
    }
    transactionLifetime_->firstStatement = this;
}

void WinsqliteStatement::replaceTransactionRegistration(
    WinsqliteStatement& other) noexcept
{
    if (transactionLifetime_ == nullptr) {
        return;
    }
    transactionPrevious_ = std::exchange(other.transactionPrevious_, nullptr);
    transactionNext_ = std::exchange(other.transactionNext_, nullptr);
    if (transactionPrevious_ != nullptr) {
        transactionPrevious_->transactionNext_ = this;
    } else {
        transactionLifetime_->firstStatement = this;
    }
    if (transactionNext_ != nullptr) {
        transactionNext_->transactionPrevious_ = this;
    }
}

void WinsqliteStatement::unregisterTransactionStatement() noexcept
{
    if (transactionLifetime_ == nullptr) {
        return;
    }
    if (transactionPrevious_ != nullptr) {
        transactionPrevious_->transactionNext_ = transactionNext_;
    } else if (transactionLifetime_->firstStatement == this) {
        transactionLifetime_->firstStatement = transactionNext_;
    }
    if (transactionNext_ != nullptr) {
        transactionNext_->transactionPrevious_ = transactionPrevious_;
    }
    transactionPrevious_ = nullptr;
    transactionNext_ = nullptr;
}

void WinsqliteStatement::finalizeForTransactionEnd() noexcept
{
    if (statement_ != nullptr) {
        static_cast<void>(sqlite3_finalize(statement_));
        statement_ = nullptr;
    }
    rowAvailable_ = false;
    operation_.reset();
}

void WinsqliteStatement::finalize() noexcept
{
    auto transactionLifetime = transactionLifetime_;
    std::unique_lock<std::recursive_mutex> lifetimeLock;
    if (transactionLifetime != nullptr) {
        lifetimeLock = std::unique_lock<std::recursive_mutex>{
            transactionLifetime->mutex};
    }
    finalizeForTransactionEnd();
    unregisterTransactionStatement();
    transactionLifetime_.reset();
}

} // namespace ForgeConductor::Persistence::Windows::Detail
