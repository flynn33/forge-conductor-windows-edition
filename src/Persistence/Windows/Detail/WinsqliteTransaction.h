#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "WinsqliteStatement.h"

#include <memory>
#include <string_view>
#include <utility>

namespace ForgeConductor::Persistence::Windows {
class DatabaseBackupCoordinator;
namespace Migrations {
class SchemaMigrator;
}
}

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteConnection;
class DatabaseNamespaceLease;
struct WinsqliteOperationState;
class WinsqliteTransaction final {
public:
    [[nodiscard]] static Domain::Result<WinsqliteTransaction> beginImmediate(
        WinsqliteConnection& connection,
        const Domain::OperationContext& context) noexcept;

    ~WinsqliteTransaction() noexcept;

    WinsqliteTransaction(const WinsqliteTransaction&) = delete;
    WinsqliteTransaction& operator=(const WinsqliteTransaction&) = delete;

    WinsqliteTransaction(WinsqliteTransaction&& other) noexcept;
    WinsqliteTransaction& operator=(WinsqliteTransaction&& other) noexcept;

    [[nodiscard]] Domain::Result<WinsqliteStatement> prepare(std::string_view sql) noexcept;
    [[nodiscard]] Domain::Result<void> execute(std::string_view sql) noexcept;
    [[nodiscard]] Domain::Result<void> commit() noexcept;
    [[nodiscard]] Domain::Result<void> rollback() noexcept;

    [[nodiscard]] bool isActive() const noexcept;

private:
    explicit WinsqliteTransaction(
        std::shared_ptr<WinsqliteOperationState> operation,
        std::shared_ptr<WinsqliteTransactionLifetime> lifetime,
        const void* connectionIdentity,
        const DatabaseNamespaceLease* namespaceAuthority) noexcept
        : operation_{std::move(operation)},
          lifetime_{std::move(lifetime)},
          connectionIdentity_{connectionIdentity},
          namespaceAuthority_{namespaceAuthority}, active_{true}
    {
    }

    [[nodiscard]] Domain::Result<void> executeToDone(std::string_view sql) noexcept;
    void rollbackNoexcept() noexcept;

    friend class ForgeConductor::Persistence::Windows::DatabaseBackupCoordinator;
    friend class ForgeConductor::Persistence::Windows::Migrations::SchemaMigrator;

    std::shared_ptr<WinsqliteOperationState> operation_;
    std::shared_ptr<WinsqliteTransactionLifetime> lifetime_;
    const void* connectionIdentity_{};
    const DatabaseNamespaceLease* namespaceAuthority_{};
    bool active_{};
};

} // namespace ForgeConductor::Persistence::Windows::Detail
