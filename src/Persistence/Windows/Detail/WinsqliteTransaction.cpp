#include "WinsqliteTransaction.h"

#include "WinsqliteConnection.h"
#include "WinsqliteOperationGuard.h"
#include "WinsqliteStatement.h"

#include <winsqlite/winsqlite3.h>

#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error inactiveTransactionError() noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "The Winsqlite3 transaction is no longer active.");
}

} // namespace

Domain::Result<WinsqliteTransaction> WinsqliteTransaction::beginImmediate(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto guard = WinsqliteOperationGuard::acquire(connection, context);
        if (!guard) {
            return Domain::Result<WinsqliteTransaction>::failure(std::move(guard).error());
        }
        auto lifetime = std::make_shared<WinsqliteTransactionLifetime>();
        auto operation = guard.value().releaseState();
        auto begun = WinsqliteStatement::executeScriptInOperation(
            operation, "BEGIN IMMEDIATE;");
        if (!begun) {
            return Domain::Result<WinsqliteTransaction>::failure(std::move(begun).error());
        }
        return Domain::Result<WinsqliteTransaction>::success(
            WinsqliteTransaction{
                std::move(operation), std::move(lifetime), connection.state_.get(),
                connection.state_->namespaceAuthority_.get()});
    } catch (...) {
        return Domain::Result<WinsqliteTransaction>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The immediate Winsqlite3 transaction could not be started."));
    }
}

WinsqliteTransaction::~WinsqliteTransaction() noexcept
{
    rollbackNoexcept();
}

WinsqliteTransaction::WinsqliteTransaction(WinsqliteTransaction&& other) noexcept
    : operation_{std::move(other.operation_)},
      lifetime_{std::move(other.lifetime_)},
      connectionIdentity_{std::exchange(other.connectionIdentity_, nullptr)},
      namespaceAuthority_{std::exchange(other.namespaceAuthority_, nullptr)},
      active_{std::exchange(other.active_, false)}
{
}

WinsqliteTransaction& WinsqliteTransaction::operator=(
    WinsqliteTransaction&& other) noexcept
{
    if (this != &other) {
        rollbackNoexcept();
        operation_ = std::move(other.operation_);
        lifetime_ = std::move(other.lifetime_);
        connectionIdentity_ = std::exchange(other.connectionIdentity_, nullptr);
        namespaceAuthority_ = std::exchange(other.namespaceAuthority_, nullptr);
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

Domain::Result<WinsqliteStatement> WinsqliteTransaction::prepare(
    const std::string_view sql) noexcept
{
    try {
        auto lifetime = lifetime_;
        if (!active_ || operation_ == nullptr || lifetime == nullptr) {
            return Domain::Result<WinsqliteStatement>::failure(inactiveTransactionError());
        }
        std::unique_lock lock{lifetime->mutex};
        if (!lifetime->active) {
            return Domain::Result<WinsqliteStatement>::failure(inactiveTransactionError());
        }
        return WinsqliteStatement::prepareInOperation(
            operation_, sql, std::move(lifetime));
    } catch (...) {
        return Domain::Result<WinsqliteStatement>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 transaction statement could not be prepared."));
    }
}

Domain::Result<void> WinsqliteTransaction::executeToDone(
    const std::string_view sql) noexcept
{
    if (!active_ || operation_ == nullptr || lifetime_ == nullptr ||
        !lifetime_->active) {
        return Domain::Result<void>::failure(inactiveTransactionError());
    }
    return WinsqliteStatement::executeScriptInOperation(operation_, sql);
}

Domain::Result<void> WinsqliteTransaction::execute(const std::string_view sql) noexcept
{
    try {
        auto lifetime = lifetime_;
        if (!active_ || operation_ == nullptr || lifetime == nullptr) {
            return Domain::Result<void>::failure(inactiveTransactionError());
        }
        std::unique_lock lock{lifetime->mutex};
        if (!lifetime->active) {
            return Domain::Result<void>::failure(inactiveTransactionError());
        }
        return executeToDone(sql);
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The complete Winsqlite3 transaction script could not be executed."));
    }
}

Domain::Result<void> WinsqliteTransaction::commit() noexcept
{
    try {
        auto lifetime = lifetime_;
        if (!active_ || operation_ == nullptr || lifetime == nullptr) {
            return Domain::Result<void>::failure(inactiveTransactionError());
        }
        std::unique_lock lock{lifetime->mutex};
        if (!lifetime->active) {
            return Domain::Result<void>::failure(inactiveTransactionError());
        }
        for (WinsqliteStatement* statement = lifetime->firstStatement;
             statement != nullptr;
             statement = statement->transactionNext_) {
            statement->finalizeForTransactionEnd();
        }
        auto committed = executeToDone("COMMIT;");
        if (!committed) {
            return committed;
        }
        active_ = false;
        lifetime->active = false;
        lifetime_.reset();
        operation_.reset();
        connectionIdentity_ = nullptr;
        namespaceAuthority_ = nullptr;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 transaction could not be committed."));
    }
}

Domain::Result<void> WinsqliteTransaction::rollback() noexcept
{
    try {
        auto lifetime = lifetime_;
        if (!active_ || operation_ == nullptr || lifetime == nullptr) {
            return Domain::Result<void>::failure(inactiveTransactionError());
        }
        std::unique_lock lock{lifetime->mutex};
        if (!lifetime->active) {
            return Domain::Result<void>::failure(inactiveTransactionError());
        }
        for (WinsqliteStatement* statement = lifetime->firstStatement;
             statement != nullptr;
             statement = statement->transactionNext_) {
            statement->finalizeForTransactionEnd();
        }
        const int result = WinsqliteOperationGuard::executeCleanupSql(
            operation_, "ROLLBACK;");
        if (result != SQLITE_OK) {
            Domain::Error error = WinsqliteOperationGuard::error(
                operation_, result, "roll back the transaction");
            WinsqliteOperationGuard::markPoisoned(operation_);
            active_ = false;
            lifetime->active = false;
            lifetime_.reset();
            operation_.reset();
            connectionIdentity_ = nullptr;
            namespaceAuthority_ = nullptr;
            return Domain::Result<void>::failure(std::move(error));
        }
        active_ = false;
        lifetime->active = false;
        lifetime_.reset();
        operation_.reset();
        connectionIdentity_ = nullptr;
        namespaceAuthority_ = nullptr;
        return Domain::Result<void>::success();
    } catch (...) {
        WinsqliteOperationGuard::markPoisoned(operation_);
        active_ = false;
        if (lifetime_ != nullptr) {
            lifetime_->active = false;
        }
        lifetime_.reset();
        operation_.reset();
        connectionIdentity_ = nullptr;
        namespaceAuthority_ = nullptr;
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Winsqlite3 transaction could not be rolled back safely."));
    }
}

void WinsqliteTransaction::rollbackNoexcept() noexcept
{
    try {
        auto lifetime = lifetime_;
        std::unique_lock<std::recursive_mutex> lock;
        if (lifetime != nullptr) {
            lock = std::unique_lock<std::recursive_mutex>{lifetime->mutex};
        }
        if (active_ && operation_ != nullptr) {
            if (lifetime != nullptr) {
                for (WinsqliteStatement* statement = lifetime->firstStatement;
                     statement != nullptr;
                     statement = statement->transactionNext_) {
                    statement->finalizeForTransactionEnd();
                }
            }
            const int result = WinsqliteOperationGuard::executeCleanupSql(
                operation_, "ROLLBACK;");
            if (result != SQLITE_OK) {
                WinsqliteOperationGuard::markPoisoned(operation_);
            }
        }
        if (lifetime != nullptr) {
            lifetime->active = false;
        }
    } catch (...) {
        WinsqliteOperationGuard::markPoisoned(operation_);
    }
    active_ = false;
    lifetime_.reset();
    operation_.reset();
    connectionIdentity_ = nullptr;
    namespaceAuthority_ = nullptr;
}

bool WinsqliteTransaction::isActive() const noexcept
{
    try {
        auto lifetime = lifetime_;
        if (!active_ || lifetime == nullptr) {
            return false;
        }
        std::scoped_lock lock{lifetime->mutex};
        return lifetime->active;
    } catch (...) {
        return false;
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
