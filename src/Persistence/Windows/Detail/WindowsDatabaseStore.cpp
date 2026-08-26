#include "WindowsDatabaseStore.h"

#include "../DatabaseBackupCoordinator.h"
#include "../DatabaseQuarantine.h"
#include "../Migrations/CentralMigrations.h"
#include "../Migrations/ProjectMigrations.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "WinsqliteStatement.h"
#include "WinsqliteTransaction.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

constexpr std::size_t MaximumSchemaObjects = 128U;
constexpr std::size_t MaximumSchemaIdentifierBytes = 128U;
constexpr auto IntegrityRecoveryBudget = std::chrono::minutes{10};

using StoreOpenResult = Domain::Result<std::unique_ptr<WindowsDatabaseStore>>;

void appendIntegrityRecoveryFailure(
    Domain::Error& primary,
    const Domain::Error& recovery) noexcept
{
    try {
        primary.message += " Integrity recovery also failed: ";
        primary.message += recovery.message;
        if (!primary.evidenceId.has_value() && recovery.evidenceId.has_value()) {
            primary.evidenceId = recovery.evidenceId;
        }
    } catch (...) {
        // Preserve the original integrity failure under memory pressure.
    }
}

[[nodiscard]] Domain::Result<Domain::OperationContext> beginIntegrityRecovery(
    const Domain::OperationContext& context,
    IDatabaseIntegrityRecoveryObserver* const observer) noexcept
{
    try {
        if (observer != nullptr) {
            observer->onIntegrityFailureDetected();
        }
        return Domain::Result<Domain::OperationContext>::success(Domain::OperationContext{
            context.operationId,
            std::chrono::steady_clock::now() + IntegrityRecoveryBudget,
            std::stop_token{},
            context.correlationId});
    } catch (...) {
        return Domain::Result<Domain::OperationContext>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A bounded integrity-recovery context could not be created."));
    }
}

[[nodiscard]] StoreOpenResult quarantineClosedIntegrityFailureWithRecovery(
    const std::shared_ptr<DatabaseNamespaceLease>& namespaceLease,
    std::unique_ptr<AnchoredSqliteVfs>& vfs,
    Domain::Error error,
    const Domain::OperationContext& recoveryContext) noexcept
{
    if (vfs) {
        auto closed = vfs->close();
        if (!closed) {
            appendIntegrityRecoveryFailure(error, closed.error());
            return StoreOpenResult::failure(std::move(error));
        }
    }
    auto quarantined = DatabaseQuarantine::preserve(
        namespaceLease, error, recoveryContext);
    if (!quarantined) {
        appendIntegrityRecoveryFailure(error, quarantined.error());
        return StoreOpenResult::failure(std::move(error));
    }
    try {
        error.evidenceId = quarantined.value().evidenceId;
        error.message += " The closed database cohort was quarantined at ";
        error.message += quarantined.value().cohortMainPath.value();
        error.message += " with manifest ";
        error.message += quarantined.value().manifestPath.value();
        error.message += '.';
    } catch (...) {
        try {
            error.evidenceId = quarantined.value().evidenceId;
            error.message +=
                " Committed quarantine evidence could not be fully described in the diagnostic.";
        } catch (...) {
            // The primary integrity failure remains valid even if diagnostic enrichment allocates.
        }
    }
    return StoreOpenResult::failure(std::move(error));
}

[[nodiscard]] StoreOpenResult quarantineConnectedIntegrityFailureWithRecovery(
    WinsqliteConnection& connection,
    const std::shared_ptr<DatabaseNamespaceLease>& namespaceLease,
    std::unique_ptr<AnchoredSqliteVfs>& vfs,
    Domain::Error error,
    const Domain::OperationContext& recoveryContext) noexcept
{
    auto closed = connection.closeForQuarantine(recoveryContext);
    if (!closed) {
        appendIntegrityRecoveryFailure(error, closed.error());
        return StoreOpenResult::failure(std::move(error));
    }
    return quarantineClosedIntegrityFailureWithRecovery(
        namespaceLease, vfs, std::move(error), recoveryContext);
}

[[nodiscard]] StoreOpenResult quarantineClosedIntegrityFailure(
    const std::shared_ptr<DatabaseNamespaceLease>& namespaceLease,
    std::unique_ptr<AnchoredSqliteVfs>& vfs,
    Domain::Error error,
    const Domain::OperationContext& context,
    IDatabaseIntegrityRecoveryObserver* const observer) noexcept
{
    if (error.code != Domain::ErrorCodes::IntegrityFailure) {
        return StoreOpenResult::failure(std::move(error));
    }
    auto recovery = beginIntegrityRecovery(context, observer);
    if (!recovery) {
        appendIntegrityRecoveryFailure(error, recovery.error());
        return StoreOpenResult::failure(std::move(error));
    }
    return quarantineClosedIntegrityFailureWithRecovery(
        namespaceLease, vfs, std::move(error), std::move(recovery).value());
}

[[nodiscard]] StoreOpenResult quarantineConnectedIntegrityFailure(
    WinsqliteConnection& connection,
    const std::shared_ptr<DatabaseNamespaceLease>& namespaceLease,
    std::unique_ptr<AnchoredSqliteVfs>& vfs,
    Domain::Error error,
    const Domain::OperationContext& context,
    IDatabaseIntegrityRecoveryObserver* const observer) noexcept
{
    if (error.code != Domain::ErrorCodes::IntegrityFailure) {
        return StoreOpenResult::failure(std::move(error));
    }
    auto recovery = beginIntegrityRecovery(context, observer);
    if (!recovery) {
        appendIntegrityRecoveryFailure(error, recovery.error());
        return StoreOpenResult::failure(std::move(error));
    }
    return quarantineConnectedIntegrityFailureWithRecovery(
        connection,
        namespaceLease,
        vfs,
        std::move(error),
        std::move(recovery).value());
}

[[nodiscard]] Domain::Result<std::string> formatUtcTimestamp(
    const Contracts::IClock& clock) noexcept
{
    try {
        const std::time_t value = std::chrono::system_clock::to_time_t(clock.utcNow());
        std::tm utc{};
        if (::gmtime_s(&utc, &value) != 0) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The database migration timestamp is outside the supported UTC range."));
        }
        std::array<char, 21U> buffer{};
        const int written = std::snprintf(
            buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
            utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec);
        if (written != 20) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The database migration timestamp could not be formatted."));
        }
        return Domain::Result<std::string>::success(
            std::string{buffer.data(), static_cast<std::size_t>(written)});
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The database migration timestamp could not be created."));
    }
}

[[nodiscard]] Domain::Result<void> runHealthChecks(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    {
        auto prepared = connection.prepare("PRAGMA main.quick_check(1);", context);
        if (!prepared) {
            return Domain::Result<void>::failure(std::move(prepared).error());
        }
        auto statement = std::move(prepared).value();
        auto stepped = statement.step();
        if (!stepped) {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() != WinsqliteStepResult::Row) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The database quick-check returned no result."));
        }
        auto result = statement.columnText(0, 64U);
        if (!result) {
            return Domain::Result<void>::failure(std::move(result).error());
        }
        if (!result.value().has_value() || *result.value() != "ok") {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The database failed its quick-check."));
        }
        stepped = statement.step();
        if (!stepped) {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() != WinsqliteStepResult::Done) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The database quick-check returned multiple rows."));
        }
    }

    {
        auto prepared = connection.prepare("PRAGMA main.foreign_key_check;", context);
        if (!prepared) {
            return Domain::Result<void>::failure(std::move(prepared).error());
        }
        auto statement = std::move(prepared).value();
        auto stepped = statement.step();
        if (!stepped) {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() != WinsqliteStepResult::Done) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The database has a foreign-key violation."));
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<bool> hasProjectFts5(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    auto prepared = connection.prepare(
        "SELECT COUNT(*) FROM main.sqlite_schema "
        "WHERE type = 'table' AND name = 'memory_records_fts';",
        context);
    if (!prepared) {
        return Domain::Result<bool>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto stepped = statement.step();
    if (!stepped) {
        return Domain::Result<bool>::failure(std::move(stepped).error());
    }
    if (stepped.value() != WinsqliteStepResult::Row) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The project FTS schema query returned no row."));
    }
    auto count = statement.columnInt64(0);
    if (!count) {
        return Domain::Result<bool>::failure(std::move(count).error());
    }
    stepped = statement.step();
    if (!stepped) {
        return Domain::Result<bool>::failure(std::move(stepped).error());
    }
    if (stepped.value() != WinsqliteStepResult::Done ||
        (count.value() != 0 && count.value() != 1)) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The project FTS schema query returned an ambiguous result."));
    }
    return Domain::Result<bool>::success(count.value() == 1);
}

[[nodiscard]] Domain::Result<void> enableProjectFts5(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    auto existing = hasProjectFts5(connection, context);
    if (!existing) {
        return Domain::Result<void>::failure(std::move(existing).error());
    }
    if (existing.value()) {
        return Domain::Result<void>::success();
    }

    auto begun = WinsqliteTransaction::beginImmediate(connection, context);
    if (!begun) {
        return Domain::Result<void>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto created = transaction.execute(Migrations::projectFtsSql());
    if (!created) {
        auto error = std::move(created).error();
        static_cast<void>(transaction.rollback());
        return Domain::Result<void>::failure(std::move(error));
    }
    auto backfilled = transaction.execute(
        "INSERT INTO memory_records_fts(id, title, summary, body) "
        "SELECT id, title, summary, COALESCE(body, '') "
        "FROM memory_records WHERE is_tombstone = 0;");
    if (!backfilled) {
        auto error = std::move(backfilled).error();
        static_cast<void>(transaction.rollback());
        return Domain::Result<void>::failure(std::move(error));
    }
    auto committed = transaction.commit();
    if (!committed) {
        static_cast<void>(transaction.rollback());
        return committed;
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::vector<std::pair<std::string, std::string>>>
readSchemaObjects(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
{
    auto prepared = connection.prepare(
        "SELECT type, name FROM main.sqlite_schema "
        "WHERE type IN ('table', 'index', 'trigger') "
        "AND substr(name, 1, 7) <> 'sqlite_' ORDER BY type, name;",
        context);
    if (!prepared) {
        return Domain::Result<std::vector<std::pair<std::string, std::string>>>::failure(
            std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    std::vector<std::pair<std::string, std::string>> objects;
    objects.reserve(32U);
    for (;;) {
        auto stepped = statement.step();
        if (!stepped) {
            return Domain::Result<std::vector<std::pair<std::string, std::string>>>::failure(
                std::move(stepped).error());
        }
        if (stepped.value() == WinsqliteStepResult::Done) {
            break;
        }
        if (objects.size() >= MaximumSchemaObjects) {
            return Domain::Result<std::vector<std::pair<std::string, std::string>>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The database schema exceeds the bounded public snapshot size."));
        }
        auto type = statement.columnText(0, MaximumSchemaIdentifierBytes);
        auto name = statement.columnText(1, MaximumSchemaIdentifierBytes);
        if (!type) {
            return Domain::Result<std::vector<std::pair<std::string, std::string>>>::failure(
                std::move(type).error());
        }
        if (!name) {
            return Domain::Result<std::vector<std::pair<std::string, std::string>>>::failure(
                std::move(name).error());
        }
        if (!type.value().has_value() || !name.value().has_value()) {
            return Domain::Result<std::vector<std::pair<std::string, std::string>>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The database schema contains a null object identity."));
        }
        objects.emplace_back(
            *std::move(type).value(), *std::move(name).value());
    }
    return Domain::Result<std::vector<std::pair<std::string, std::string>>>::success(
        std::move(objects));
}

} // namespace

WindowsDatabaseStore::WindowsDatabaseStore(
    WindowsDatabaseStoreOptions options,
    std::shared_ptr<DatabaseNamespaceLease> namespaceLease,
    std::unique_ptr<AnchoredSqliteVfs> vfs,
    WinsqliteConnection connection,
    Contracts::RuntimeOwnershipLease runtimeLease) noexcept
    : options_{options},
      namespaceLease_{std::move(namespaceLease)},
      vfs_{std::move(vfs)},
      connection_{std::move(connection)},
      runtimeLease_{std::move(runtimeLease)}
{
}

WindowsDatabaseStore::~WindowsDatabaseStore() noexcept
{
    connection_.reset();
    if (vfs_) {
        static_cast<void>(vfs_->close());
        vfs_.reset();
    }
    namespaceLease_.reset();
    runtimeLease_.reset();
}

Domain::Result<std::unique_ptr<WindowsDatabaseStore>> WindowsDatabaseStore::open(
    const Domain::PathText& directory,
    const std::wstring_view mainBasename,
    const std::wstring_view migrationLockBasename,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IClock> clock,
    const WindowsDatabaseStoreOptions options,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (!runtimeDiagnostics || !clock ||
            (options.databaseKind != Migrations::DatabaseKind::Central &&
             options.databaseKind != Migrations::DatabaseKind::Project)) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Database construction requires diagnostics, a clock, and a valid store kind."));
        }
        auto runtimeLeaseResult = runtimeDiagnostics->acquire(
            Contracts::RuntimeOwnerKind::OpenDatabase, context);
        if (!runtimeLeaseResult) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(runtimeLeaseResult).error());
        }
        auto runtimeLease = std::move(runtimeLeaseResult).value();

        auto directoryWide = Infrastructure::Windows::Detail::strictUtf8ToUtf16(
            directory.value());
        if (!directoryWide) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(directoryWide).error());
        }
        auto namespaceResult = DatabaseNamespaceLease::create(
            directoryWide.value(), mainBasename, migrationLockBasename);
        if (!namespaceResult) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(namespaceResult).error());
        }
        auto namespaceLease = std::move(namespaceResult).value();
        auto migrationLockResult = namespaceLease->acquireMigrationLock(context);
        if (!migrationLockResult) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(migrationLockResult).error());
        }
        auto migrationLock = std::move(migrationLockResult).value();
        static_cast<void>(migrationLock);

        auto staleStagesCleaned = DatabaseBackupCoordinator::cleanupStaleBackupStages(
            namespaceLease, context);
        if (!staleStagesCleaned) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(staleStagesCleaned).error());
        }

        auto stableNamespace = namespaceLease->revalidate();
        if (!stableNamespace) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(stableNamespace).error());
        }

        auto vfsResult = AnchoredSqliteVfs::create(namespaceLease);
        if (!vfsResult) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(vfsResult).error());
        }
        auto vfs = std::move(vfsResult).value();
        auto exists = namespaceLease->accessLeaf(
            DatabaseLeafRole::Main, DatabaseLeafAccess::Exists);
        if (!exists) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(exists).error());
        }

        std::optional<WinsqliteConnection> migrationReadSource;
        std::optional<DatabaseBackupCoordinator> migrationBackup;
        Migrations::SchemaAssessment assessment{};
        if (exists.value()) {
            WinsqliteConnectionOptions readOnlyOptions{
                std::string{vfs->vfsName()},
                WinsqliteOpenMode::ReadOnlyExisting,
                options.synchronousMode,
                WinsqliteJournalMode::WriteAheadLog,
                namespaceLease};
            auto readOnlyResult = WinsqliteConnection::open(
                namespaceLease->canonicalMainDatabasePath(),
                readOnlyOptions,
                context);
            if (!readOnlyResult) {
                return quarantineClosedIntegrityFailure(
                    namespaceLease,
                    vfs,
                    std::move(readOnlyResult).error(),
                    context,
                    options.integrityRecoveryObserver);
            }
            auto readOnly = std::move(readOnlyResult).value();
            stableNamespace = namespaceLease->revalidate();
            if (!stableNamespace) {
                return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                    std::move(stableNamespace).error());
            }
            Migrations::SchemaMigrator assessor{readOnly};
            auto assessed = assessor.assess(options.databaseKind, context);
            if (!assessed) {
                return quarantineConnectedIntegrityFailure(
                    readOnly,
                    namespaceLease,
                    vfs,
                    std::move(assessed).error(),
                    context,
                    options.integrityRecoveryObserver);
            }
            assessment = assessed.value();
            stableNamespace = namespaceLease->revalidate();
            if (!stableNamespace) {
                return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                    std::move(stableNamespace).error());
            }
            if (assessment.requiresOnlineBackup) {
                migrationReadSource.emplace(std::move(readOnly));
            } else {
                auto closed = readOnly.close(context);
                if (!closed) {
                    return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                        std::move(closed).error());
                }
            }
            stableNamespace = namespaceLease->revalidate();
            if (!stableNamespace) {
                return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                    std::move(stableNamespace).error());
            }
        }

        WinsqliteConnectionOptions writableOptions{
            std::string{vfs->vfsName()},
            exists.value() ? WinsqliteOpenMode::ReadWriteExisting
                           : WinsqliteOpenMode::ReadWriteCreate,
            options.synchronousMode,
            WinsqliteJournalMode::WriteAheadLog,
            namespaceLease};
        auto writableResult = WinsqliteConnection::open(
            namespaceLease->canonicalMainDatabasePath(), writableOptions, context);
        if (!writableResult) {
            return quarantineClosedIntegrityFailure(
                namespaceLease,
                vfs,
                std::move(writableResult).error(),
                context,
                options.integrityRecoveryObserver);
        }
        auto writable = std::move(writableResult).value();
        stableNamespace = namespaceLease->revalidate();
        if (!stableNamespace) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(stableNamespace).error());
        }

        auto transactionResult = WinsqliteTransaction::beginImmediate(writable, context);
        if (!transactionResult) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(transactionResult).error());
        }
        auto transaction = std::move(transactionResult).value();
        const auto failAfterTransaction = [&](Domain::Error error) -> StoreOpenResult {
            const bool recoveringIntegrity =
                error.code == Domain::ErrorCodes::IntegrityFailure;
            std::optional<Domain::OperationContext> recoveryContext;
            if (recoveringIntegrity) {
                auto recovery = beginIntegrityRecovery(
                    context, options.integrityRecoveryObserver);
                if (!recovery) {
                    appendIntegrityRecoveryFailure(error, recovery.error());
                } else {
                    recoveryContext.emplace(std::move(recovery).value());
                }
            }
            const Domain::OperationContext& cleanupContext =
                recoveryContext.has_value() ? *recoveryContext : context;
            bool connectionReleaseFailed = false;
            if (transaction.isActive()) {
                auto rolledBack = transaction.rollback();
                if (!rolledBack) {
                    appendIntegrityRecoveryFailure(error, rolledBack.error());
                    if (!recoveringIntegrity) {
                        return StoreOpenResult::failure(std::move(error));
                    }
                }
            }
            if (migrationReadSource.has_value()) {
                auto closed = recoveringIntegrity
                    ? migrationReadSource->closeForQuarantine(cleanupContext)
                    : migrationReadSource->close(cleanupContext);
                if (!closed) {
                    appendIntegrityRecoveryFailure(error, closed.error());
                    if (!recoveringIntegrity) {
                        return StoreOpenResult::failure(std::move(error));
                    }
                    connectionReleaseFailed = true;
                } else {
                    migrationReadSource.reset();
                }
            }
            if (recoveringIntegrity) {
                auto writableClosed = writable.closeForQuarantine(cleanupContext);
                if (!writableClosed) {
                    appendIntegrityRecoveryFailure(error, writableClosed.error());
                    connectionReleaseFailed = true;
                }
                if (connectionReleaseFailed) {
                    return StoreOpenResult::failure(std::move(error));
                }
                return quarantineClosedIntegrityFailureWithRecovery(
                    namespaceLease, vfs, std::move(error), cleanupContext);
            }
            return StoreOpenResult::failure(std::move(error));
        };

        Migrations::SchemaMigrator migrator{writable};
        auto writableAssessment = migrator.assess(transaction, options.databaseKind);
        if (!writableAssessment) {
            return failAfterTransaction(std::move(writableAssessment).error());
        }
        if (exists.value() && writableAssessment.value() != assessment) {
            return failAfterTransaction(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The database schema changed between read-only assessment and migration.",
                    true));
        }

        std::optional<Migrations::MigrationBackupReceipt> receipt;
        if (migrationReadSource.has_value()) {
            auto backup = DatabaseBackupCoordinator::createMigrationBackup(
                migrationReadSource.value(),
                transaction,
                namespaceLease,
                writableAssessment.value(),
                context);
            if (!backup) {
                return failAfterTransaction(std::move(backup).error());
            }
            migrationBackup.emplace(std::move(backup).value());
            auto bound = migrationBackup->bindToTransaction(
                transaction, *namespaceLease, writableAssessment.value());
            if (!bound) {
                return failAfterTransaction(std::move(bound).error());
            }
            receipt.emplace(std::move(bound).value());
            auto closed = migrationReadSource->close(context);
            if (!closed) {
                return failAfterTransaction(std::move(closed).error());
            }
            migrationReadSource.reset();
        }
        auto appliedAt = formatUtcTimestamp(*clock);
        if (!appliedAt) {
            return failAfterTransaction(std::move(appliedAt).error());
        }
        stableNamespace = namespaceLease->revalidate();
        if (!stableNamespace) {
            return failAfterTransaction(std::move(stableNamespace).error());
        }
        auto migrated = migrator.migrate(
            transaction,
            writableAssessment.value(),
            receipt.has_value() ? &receipt.value() : nullptr,
            appliedAt.value());
        if (!migrated) {
            return failAfterTransaction(std::move(migrated).error());
        }
        stableNamespace = namespaceLease->revalidate();
        if (!stableNamespace) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(stableNamespace).error());
        }

        if (options.databaseKind == Migrations::DatabaseKind::Project &&
            options.enableProjectFts5) {
            auto enabled = enableProjectFts5(writable, context);
            if (!enabled) {
                return quarantineConnectedIntegrityFailure(
                    writable,
                    namespaceLease,
                    vfs,
                    std::move(enabled).error(),
                    context,
                    options.integrityRecoveryObserver);
            }
            auto verified = migrator.assess(options.databaseKind, context);
            if (!verified) {
                return quarantineConnectedIntegrityFailure(
                    writable,
                    namespaceLease,
                    vfs,
                    std::move(verified).error(),
                    context,
                    options.integrityRecoveryObserver);
            }
            stableNamespace = namespaceLease->revalidate();
            if (!stableNamespace) {
                return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                    std::move(stableNamespace).error());
            }
        }
        stableNamespace = namespaceLease->revalidate();
        if (!stableNamespace) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(stableNamespace).error());
        }
        auto healthy = runHealthChecks(writable, context);
        if (!healthy) {
            return quarantineConnectedIntegrityFailure(
                writable,
                namespaceLease,
                vfs,
                std::move(healthy).error(),
                context,
                options.integrityRecoveryObserver);
        }
        stableNamespace = namespaceLease->revalidate();
        if (!stableNamespace) {
            return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
                std::move(stableNamespace).error());
        }

        return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::success(
            std::unique_ptr<WindowsDatabaseStore>{new WindowsDatabaseStore{
                options,
                std::move(namespaceLease),
                std::move(vfs),
                std::move(writable),
                std::move(runtimeLease)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<WindowsDatabaseStore>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Windows database store could not be opened safely."));
    }
}

Domain::Result<bool> WindowsDatabaseStore::projectFts5Enabled(
    const Domain::OperationContext& context) noexcept
{
    if (!connection_.has_value()) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The database is closed."));
    }
    if (options_.databaseKind != Migrations::DatabaseKind::Project) {
        return Domain::Result<bool>::success(false);
    }
    return hasProjectFts5(connection_.value(), context);
}

Domain::Result<DatabaseSchemaSnapshot> WindowsDatabaseStore::schemaSnapshot(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto admitted = facadeAdmission_.acquire(
            context, "Create the database schema snapshot");
        if (!admitted) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(
                std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);

        if (!connection_.has_value()) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The database is closed."));
        }
        auto stableNamespace = namespaceLease_->revalidate();
        if (!stableNamespace) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(
                std::move(stableNamespace).error());
        }
        Migrations::SchemaMigrator migrator{connection_.value()};
        auto assessment = migrator.assess(options_.databaseKind, context);
        if (!assessment) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(
                std::move(assessment).error());
        }
        auto fts = projectFts5Enabled(context);
        if (!fts) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(std::move(fts).error());
        }
        auto objects = readSchemaObjects(connection_.value(), context);
        if (!objects) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(
                std::move(objects).error());
        }

        DatabaseSchemaSnapshot snapshot{};
        snapshot.kind = options_.databaseKind == Migrations::DatabaseKind::Central
                            ? DatabaseStoreKind::Central
                            : DatabaseStoreKind::Project;
        snapshot.physicalVersion = assessment.value().sourceVersion;
        snapshot.sourceCompatibilityVersion =
            options_.databaseKind == Migrations::DatabaseKind::Central
                ? Migrations::CentralSourceVersion
                : Migrations::ProjectSourceVersion;
        snapshot.fts5Enabled = fts.value();
        for (auto& [type, name] : objects.value()) {
            if (type == "table") {
                snapshot.tables.push_back(std::move(name));
            } else if (type == "index") {
                snapshot.indexes.push_back(std::move(name));
            } else if (type == "trigger") {
                snapshot.triggers.push_back(std::move(name));
            }
        }
        stableNamespace = namespaceLease_->revalidate();
        if (!stableNamespace) {
            return Domain::Result<DatabaseSchemaSnapshot>::failure(
                std::move(stableNamespace).error());
        }
        return Domain::Result<DatabaseSchemaSnapshot>::success(std::move(snapshot));
    } catch (...) {
        return Domain::Result<DatabaseSchemaSnapshot>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded database schema snapshot could not be created."));
    }
}

Domain::Result<void> WindowsDatabaseStore::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    auto admitted = facadeAdmission_.acquire(context, "Run the database quick-check");
    if (!admitted) {
        return Domain::Result<void>::failure(std::move(admitted).error());
    }
    auto admission = std::move(admitted).value();
    static_cast<void>(admission);

    if (!connection_.has_value()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The database is closed."));
    }
    auto stableNamespace = namespaceLease_->revalidate();
    if (!stableNamespace) {
        return stableNamespace;
    }
    auto healthy = runHealthChecks(connection_.value(), context);
    if (!healthy) {
        return healthy;
    }
    return namespaceLease_->revalidate();
}

Domain::Result<DatabaseBackupReport> WindowsDatabaseStore::createOnlineBackup(
    const Domain::OperationContext& context) noexcept
{
    auto admitted = facadeAdmission_.acquire(context, "Create the online database backup");
    if (!admitted) {
        return Domain::Result<DatabaseBackupReport>::failure(
            std::move(admitted).error());
    }
    auto admission = std::move(admitted).value();
    static_cast<void>(admission);

    if (!connection_.has_value() || !namespaceLease_) {
        return Domain::Result<DatabaseBackupReport>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The database is closed."));
    }
    auto stableNamespace = namespaceLease_->revalidate();
    if (!stableNamespace) {
        return Domain::Result<DatabaseBackupReport>::failure(
            std::move(stableNamespace).error());
    }
    auto backup = DatabaseBackupCoordinator::createOperationalBackup(
        connection_.value(), namespaceLease_, context);
    if (!backup) {
        return backup;
    }
    stableNamespace = namespaceLease_->revalidate();
    if (!stableNamespace) {
        return Domain::Result<DatabaseBackupReport>::failure(
            std::move(stableNamespace).error());
    }
    return backup;
}

Domain::Result<void> WindowsDatabaseStore::runExclusive(
    IWindowsDatabaseOperation& operation,
    const std::string_view action,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto admitted = facadeAdmission_.acquire(context, action);
        if (!admitted) {
            return Domain::Result<void>::failure(std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);

        if (!connection_.has_value() || !namespaceLease_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The database is closed."));
        }
        auto stableNamespace = namespaceLease_->revalidate();
        if (!stableNamespace) {
            return stableNamespace;
        }
        auto completed = operation.execute(connection_.value());
        if (!completed) {
            return completed;
        }
        return namespaceLease_->revalidate();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The serialized database operation could not be completed."));
    }
}

Domain::Result<std::pair<std::uint64_t, std::uint64_t>>
WindowsDatabaseStore::databaseFileSizes(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto admitted = facadeAdmission_.acquire(context, "Inspect database cohort sizes");
        if (!admitted) {
            return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
                std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);
        if (!connection_.has_value() || !namespaceLease_) {
            return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The database is closed."));
        }
        auto stable = namespaceLease_->revalidate();
        if (!stable) {
            return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
                std::move(stable).error());
        }

        const auto readSize = [&](const DatabaseLeafRole role)
            -> Domain::Result<std::uint64_t> {
            auto exists = namespaceLease_->accessLeaf(role, DatabaseLeafAccess::Exists);
            if (!exists) {
                return Domain::Result<std::uint64_t>::failure(
                    std::move(exists).error());
            }
            if (!exists.value()) {
                return Domain::Result<std::uint64_t>::success(0U);
            }
            auto leaf = namespaceLease_->openLeaf(
                role,
                DatabaseLeafDisposition::OpenExisting,
                FILE_READ_ATTRIBUTES);
            if (!leaf) {
                return Domain::Result<std::uint64_t>::failure(
                    std::move(leaf).error());
            }
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(leaf.value().nativeHandle(), &size) == FALSE ||
                size.QuadPart < 0) {
                return Domain::Result<std::uint64_t>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The database cohort file size could not be inspected."));
            }
            return Domain::Result<std::uint64_t>::success(
                static_cast<std::uint64_t>(size.QuadPart));
        };

        auto mainBytes = readSize(DatabaseLeafRole::Main);
        if (!mainBytes) {
            return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
                std::move(mainBytes).error());
        }
        auto walBytes = readSize(DatabaseLeafRole::Wal);
        if (!walBytes) {
            return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
                std::move(walBytes).error());
        }
        stable = namespaceLease_->revalidate();
        if (!stable) {
            return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
                std::move(stable).error());
        }
        return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
            {mainBytes.value(), walBytes.value()});
    } catch (...) {
        return Domain::Result<std::pair<std::uint64_t, std::uint64_t>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The database cohort sizes could not be inspected."));
    }
}

Domain::Result<void> WindowsDatabaseStore::close(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto admitted = facadeAdmission_.acquire(context, "Close the database");
        if (!admitted) {
            return Domain::Result<void>::failure(std::move(admitted).error());
        }
        auto admission = std::move(admitted).value();
        static_cast<void>(admission);

        if (!connection_.has_value() && !vfs_ && !namespaceLease_) {
            runtimeLease_.reset();
            return Domain::Result<void>::success();
        }
        std::optional<Domain::Error> boundaryError;
        if (namespaceLease_) {
            auto stableNamespace = namespaceLease_->revalidate();
            if (!stableNamespace) {
                boundaryError.emplace(std::move(stableNamespace).error());
            }
        }
        if (connection_.has_value()) {
            auto closed = connection_->close(context);
            if (!closed) {
                return closed;
            }
            connection_.reset();
        }
        if (namespaceLease_) {
            auto stableNamespace = namespaceLease_->revalidate();
            if (!stableNamespace && !boundaryError.has_value()) {
                boundaryError.emplace(std::move(stableNamespace).error());
            }
        }
        if (vfs_) {
            auto closed = vfs_->close();
            if (!closed) {
                return closed;
            }
            vfs_.reset();
        }
        if (namespaceLease_) {
            auto stableNamespace = namespaceLease_->revalidate();
            if (!stableNamespace && !boundaryError.has_value()) {
                boundaryError.emplace(std::move(stableNamespace).error());
            }
        }
        namespaceLease_.reset();
        runtimeLease_.reset();
        if (boundaryError.has_value()) {
            return Domain::Result<void>::failure(std::move(boundaryError).value());
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Windows database store could not be closed deterministically."));
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
