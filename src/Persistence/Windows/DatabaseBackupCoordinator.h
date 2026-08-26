#pragma once

#include "ForgeConductor/Persistence/Windows/DatabaseModels.h"
#include "Detail/DatabaseNamespaceLease.h"
#include "Migrations/SchemaMigrator.h"

#include <cstddef>
#include <memory>
#include <string>

namespace ForgeConductor::Persistence::Windows::Detail {
class WinsqliteConnection;
class WinsqliteTransaction;
struct WinsqliteTransactionLifetime;
}

namespace ForgeConductor::Persistence::Windows {

class DatabaseBackupCoordinator final {
public:
    [[nodiscard]] static Domain::Result<DatabaseBackupCoordinator> createMigrationBackup(
        Detail::WinsqliteConnection& readOnlySource,
        Detail::WinsqliteTransaction& admittedTransaction,
        std::shared_ptr<Detail::DatabaseNamespaceLease> sourceNamespace,
        const Migrations::SchemaAssessment& sourceAssessment,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<DatabaseBackupReport> createOperationalBackup(
        Detail::WinsqliteConnection& source,
        std::shared_ptr<Detail::DatabaseNamespaceLease> sourceNamespace,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<std::size_t> cleanupStaleBackupStages(
        std::shared_ptr<Detail::DatabaseNamespaceLease> sourceNamespace,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<Migrations::MigrationBackupReceipt> bindToTransaction(
        Detail::WinsqliteTransaction& admittedTransaction,
        const Detail::DatabaseNamespaceLease& sourceNamespace,
        const Migrations::SchemaAssessment& reassessment) const noexcept;

    [[nodiscard]] const DatabaseBackupReport& report() const noexcept { return report_; }

    DatabaseBackupCoordinator(const DatabaseBackupCoordinator&) = delete;
    DatabaseBackupCoordinator& operator=(const DatabaseBackupCoordinator&) = delete;
    DatabaseBackupCoordinator(DatabaseBackupCoordinator&&) noexcept = default;
    DatabaseBackupCoordinator& operator=(DatabaseBackupCoordinator&&) noexcept = default;

private:
    DatabaseBackupCoordinator(
        Migrations::SchemaAssessment assessment,
        Detail::DatabaseFileIdentity sourceIdentity,
        DatabaseBackupReport report,
        std::string evidenceId,
        std::shared_ptr<Detail::WinsqliteTransactionLifetime> transactionLifetime,
        std::shared_ptr<const Detail::MigrationBackupArtifactLease> backupArtifact) noexcept;

    Migrations::SchemaAssessment assessment_;
    Detail::DatabaseFileIdentity sourceIdentity_;
    DatabaseBackupReport report_;
    std::string evidenceId_;
    std::shared_ptr<Detail::WinsqliteTransactionLifetime> transactionLifetime_;
    std::shared_ptr<const Detail::MigrationBackupArtifactLease> backupArtifact_;
};

} // namespace ForgeConductor::Persistence::Windows
