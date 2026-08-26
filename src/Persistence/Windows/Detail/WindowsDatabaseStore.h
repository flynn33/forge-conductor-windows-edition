#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Persistence/Windows/DatabaseModels.h"
#include "Infrastructure/Windows/Detail/BoundedSerialExecutor.h"
#include "AnchoredSqliteVfs.h"
#include "WinsqliteConnection.h"
#include "../Migrations/SchemaMigrator.h"

#include <memory>
#include <optional>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {

class WinsqliteConnection;

// Internal persistence extension point. It keeps the native connection behind
// the Persistence.Windows boundary while allowing project-owned repositories to
// run one serialized, namespace-revalidated unit of work on the store's sole
// connection.
class IWindowsDatabaseOperation {
public:
    virtual ~IWindowsDatabaseOperation() noexcept = default;

private:
    friend class WindowsDatabaseStore;

    [[nodiscard]] virtual Domain::Result<void> execute(
        WinsqliteConnection& connection) noexcept = 0;
};

class IDatabaseIntegrityRecoveryObserver {
public:
    virtual ~IDatabaseIntegrityRecoveryObserver() noexcept = default;
    virtual void onIntegrityFailureDetected() noexcept = 0;
};

struct WindowsDatabaseStoreOptions final {
    Migrations::DatabaseKind databaseKind{Migrations::DatabaseKind::Central};
    WinsqliteSynchronousMode synchronousMode{WinsqliteSynchronousMode::Full};
    bool enableProjectFts5{};
    // Non-owning and used synchronously only during open-time recovery.
    IDatabaseIntegrityRecoveryObserver* integrityRecoveryObserver{};
};

class WindowsDatabaseStore final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsDatabaseStore>> open(
        const Domain::PathText& directory,
        std::wstring_view mainBasename,
        std::wstring_view migrationLockBasename,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IClock> clock,
        WindowsDatabaseStoreOptions options,
        const Domain::OperationContext& context) noexcept;

    ~WindowsDatabaseStore() noexcept;

    WindowsDatabaseStore(const WindowsDatabaseStore&) = delete;
    WindowsDatabaseStore& operator=(const WindowsDatabaseStore&) = delete;
    WindowsDatabaseStore(WindowsDatabaseStore&&) = delete;
    WindowsDatabaseStore& operator=(WindowsDatabaseStore&&) = delete;

    [[nodiscard]] Domain::Result<DatabaseSchemaSnapshot> schemaSnapshot(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<DatabaseBackupReport> createOnlineBackup(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<void> runExclusive(
        IWindowsDatabaseOperation& operation,
        std::string_view action,
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<std::pair<std::uint64_t, std::uint64_t>>
    databaseFileSizes(const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<void> close(
        const Domain::OperationContext& context) noexcept;

private:
    friend class WindowsDatabaseStoreTestAccess;

    WindowsDatabaseStore(
        WindowsDatabaseStoreOptions options,
        std::shared_ptr<DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<AnchoredSqliteVfs> vfs,
        WinsqliteConnection connection,
        Contracts::RuntimeOwnershipLease runtimeLease) noexcept;

    [[nodiscard]] Domain::Result<bool> projectFts5Enabled(
        const Domain::OperationContext& context) noexcept;

    Infrastructure::Windows::Detail::BoundedSerialExecutor facadeAdmission_;
    WindowsDatabaseStoreOptions options_;
    std::shared_ptr<DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<AnchoredSqliteVfs> vfs_;
    std::optional<WinsqliteConnection> connection_;
    std::optional<Contracts::RuntimeOwnershipLease> runtimeLease_;
};

} // namespace ForgeConductor::Persistence::Windows::Detail
