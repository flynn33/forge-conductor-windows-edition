#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct sqlite3_stmt;

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteConnection;
struct WinsqliteOperationState;
class WinsqliteTransaction;
class WinsqliteStatement;
class WinsqliteStatementTestAccess;

struct WinsqliteTransactionLifetime final {
    std::recursive_mutex mutex;
    WinsqliteStatement* firstStatement{};
    bool active{true};
};

enum class WinsqliteStepResult { Row, Done };

class WinsqliteStatement final {
public:
    ~WinsqliteStatement() noexcept;

    WinsqliteStatement(const WinsqliteStatement&) = delete;
    WinsqliteStatement& operator=(const WinsqliteStatement&) = delete;

    WinsqliteStatement(WinsqliteStatement&& other) noexcept;
    WinsqliteStatement& operator=(WinsqliteStatement&& other) noexcept;

    [[nodiscard]] Domain::Result<void> bindText(
        int parameterIndex,
        std::string_view value) noexcept;
    [[nodiscard]] Domain::Result<void> bindInt64(
        int parameterIndex,
        std::int64_t value) noexcept;
    [[nodiscard]] Domain::Result<void> bindDouble(
        int parameterIndex,
        double value) noexcept;
    [[nodiscard]] Domain::Result<void> bindNull(int parameterIndex) noexcept;

    [[nodiscard]] Domain::Result<WinsqliteStepResult> step() noexcept;
    [[nodiscard]] Domain::Result<void> reset(bool clearBindings = true) noexcept;

    [[nodiscard]] int parameterCount() const noexcept;
    [[nodiscard]] int columnCount() const noexcept;
    [[nodiscard]] Domain::Result<bool> columnIsNull(int columnIndex) const noexcept;
    [[nodiscard]] Domain::Result<std::int64_t> columnInt64(int columnIndex) const noexcept;
    [[nodiscard]] Domain::Result<double> columnDouble(int columnIndex) const noexcept;
    [[nodiscard]] Domain::Result<std::optional<std::string>> columnText(
        int columnIndex,
        std::size_t maximumBytes) const noexcept;

private:
    WinsqliteStatement(
        sqlite3_stmt* statement,
        std::shared_ptr<WinsqliteOperationState> operation,
        std::shared_ptr<WinsqliteTransactionLifetime> transactionLifetime = {}) noexcept;

    [[nodiscard]] static Domain::Result<WinsqliteStatement> prepareInOperation(
        const std::shared_ptr<WinsqliteOperationState>& operation,
        std::string_view sql,
        std::shared_ptr<WinsqliteTransactionLifetime> transactionLifetime = {}) noexcept;
    [[nodiscard]] static Domain::Result<void> executeScriptInOperation(
        const std::shared_ptr<WinsqliteOperationState>& operation,
        std::string_view sql) noexcept;

    [[nodiscard]] Domain::Result<void> validateParameterIndex(int parameterIndex) const noexcept;
    [[nodiscard]] Domain::Result<void> validateColumnIndex(int columnIndex) const noexcept;
    [[nodiscard]] Domain::Result<void> validateTransactionLifetime() const noexcept;
    [[nodiscard]] Domain::Result<std::unique_lock<std::recursive_mutex>>
    lockTransactionLifetime() const noexcept;
    void registerTransactionStatement() noexcept;
    void replaceTransactionRegistration(WinsqliteStatement& other) noexcept;
    void unregisterTransactionStatement() noexcept;
    void finalizeForTransactionEnd() noexcept;
    void finalize() noexcept;

    friend class WinsqliteConnection;
    friend class WinsqliteTransaction;
    friend class WinsqliteStatementTestAccess;

    sqlite3_stmt* statement_{};
    std::shared_ptr<WinsqliteOperationState> operation_;
    std::shared_ptr<WinsqliteTransactionLifetime> transactionLifetime_;
    WinsqliteStatement* transactionPrevious_{};
    WinsqliteStatement* transactionNext_{};
    bool rowAvailable_{};
};

} // namespace ForgeConductor::Persistence::Windows::Detail
