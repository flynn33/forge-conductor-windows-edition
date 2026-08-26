#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <memory>
#include <string>
#include <string_view>

namespace ForgeConductor::Persistence::Windows::Detail
{
class MigrationBackupArtifactLease;
class WinsqliteConnection;
class WinsqliteTransaction;
struct WinsqliteTransactionLifetime;

[[nodiscard]] Domain::Result<void> revalidateMigrationBackupArtifact(
    const std::shared_ptr<const MigrationBackupArtifactLease> &artifact) noexcept;
}

namespace ForgeConductor::Persistence::Windows
{
class DatabaseBackupCoordinator;
}

namespace ForgeConductor::Persistence::Windows::Migrations
{

enum class DatabaseKind
{
    Central,
    Project,
};

enum class SchemaLayout
{
    Empty,
    CentralVersion3Minimal,
    CentralVersion3,
    CentralVersion5,
    CentralVersion6,
    ProjectVersion1,
    ProjectVersion2,
    ProjectVersion3,
};

struct SchemaAssessment final
{
    DatabaseKind databaseKind{};
    SchemaLayout layout{};
    int sourceVersion{};
    int targetVersion{};
    bool requiresOnlineBackup{};

    bool operator==(const SchemaAssessment &) const = default;
};

class MigrationBackupReceipt final
{
  public:
    MigrationBackupReceipt(const MigrationBackupReceipt &) = default;
    MigrationBackupReceipt &operator=(const MigrationBackupReceipt &) = default;
    MigrationBackupReceipt(MigrationBackupReceipt &&) noexcept = default;
    MigrationBackupReceipt &operator=(MigrationBackupReceipt &&) noexcept = default;

    [[nodiscard]] std::string_view evidenceId() const noexcept
    {
        return evidenceId_;
    }

  private:
    // The coordinator constructs this only after matching read-only backup
    // evidence to the exact live BEGIN IMMEDIATE transaction generation.
    MigrationBackupReceipt(
                           std::shared_ptr<Detail::WinsqliteTransactionLifetime> transaction,
                           const SchemaAssessment &assessment, std::string evidenceId,
                           std::shared_ptr<const Detail::MigrationBackupArtifactLease>
                               backupArtifact) noexcept;

    [[nodiscard]] Domain::Result<void> revalidateArtifact() const noexcept;

    friend class ForgeConductor::Persistence::Windows::DatabaseBackupCoordinator;
    friend class SchemaMigrator;

    std::weak_ptr<Detail::WinsqliteTransactionLifetime> transaction_;
    SchemaAssessment assessment_{};
    std::string evidenceId_;
    std::shared_ptr<const Detail::MigrationBackupArtifactLease> backupArtifact_;
};

class SchemaMigrator final
{
  public:
    explicit SchemaMigrator(Detail::WinsqliteConnection &connection) noexcept;

    SchemaMigrator(const SchemaMigrator &) = delete;
    SchemaMigrator &operator=(const SchemaMigrator &) = delete;
    SchemaMigrator(SchemaMigrator &&) = delete;
    SchemaMigrator &operator=(SchemaMigrator &&) = delete;

    [[nodiscard]] Domain::Result<SchemaAssessment> assess(
        DatabaseKind databaseKind, const Domain::OperationContext &context) noexcept;

    [[nodiscard]] Domain::Result<SchemaAssessment> assess(
        Detail::WinsqliteTransaction &transaction,
        DatabaseKind databaseKind) noexcept;

    [[nodiscard]] Domain::Result<SchemaAssessment> migrate(
        Detail::WinsqliteTransaction &transaction,
        const SchemaAssessment &priorAssessment,
        const MigrationBackupReceipt *verifiedBackup,
        std::string_view appliedAt) noexcept;

  private:
    Detail::WinsqliteConnection &connection_;
};

} // namespace ForgeConductor::Persistence::Windows::Migrations
