#include "DatabaseBackupCoordinator.h"

#include "Detail/AnchoredSqliteVfs.h"
#include "Detail/WinsqliteBackup.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "Detail/WinsqliteTransaction.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows {

namespace Detail {

class MigrationBackupArtifactLease final {
public:
    MigrationBackupArtifactLease(
        std::shared_ptr<DatabaseNamespaceLease> backupNamespace,
        DatabaseLeafLease identityLease) noexcept
        : backupNamespace_{std::move(backupNamespace)},
          identityLease_{std::move(identityLease)}
    {
    }

    MigrationBackupArtifactLease(const MigrationBackupArtifactLease&) = delete;
    MigrationBackupArtifactLease& operator=(const MigrationBackupArtifactLease&) = delete;
    MigrationBackupArtifactLease(MigrationBackupArtifactLease&&) = delete;
    MigrationBackupArtifactLease& operator=(MigrationBackupArtifactLease&&) = delete;

private:
    friend Domain::Result<void> revalidateMigrationBackupArtifact(
        const std::shared_ptr<const MigrationBackupArtifactLease>& artifact) noexcept;

    std::shared_ptr<DatabaseNamespaceLease> backupNamespace_;
    DatabaseLeafLease identityLease_;
};

Domain::Result<void> revalidateMigrationBackupArtifact(
    const std::shared_ptr<const MigrationBackupArtifactLease>& artifact) noexcept
{
    try {
        if (!artifact || !artifact->backupNamespace_ || !artifact->identityLease_ ||
            artifact->identityLease_.role() != DatabaseLeafRole::Main) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::MigrationFailed,
                "The verified migration-backup artifact lease is unavailable."));
        }
        return artifact->backupNamespace_->revalidateRetainedLeaf(
            artifact->identityLease_);
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::MigrationFailed,
            "The verified migration-backup artifact could not be revalidated."));
    }
}

} // namespace Detail

namespace {

using Detail::AnchoredSqliteVfs;
using Detail::DatabaseLeafAccess;
using Detail::DatabaseLeafDisposition;
using Detail::DatabaseLeafLease;
using Detail::DatabaseLeafRole;
using Detail::DatabaseMigrationLock;
using Detail::DatabaseNamespaceLease;
using Detail::WinsqliteBackup;
using Detail::WinsqliteConnection;
using Detail::WinsqliteConnectionOptions;
using Detail::WinsqliteJournalMode;
using Detail::WinsqliteOpenMode;
using Detail::WinsqliteStepResult;
using Detail::WinsqliteSynchronousMode;

struct BackupNames final {
    std::wstring stageMain;
    std::wstring stageLock;
    std::wstring finalMain;
    std::wstring finalLock;
};

struct BackupOutcome final {
    DatabaseBackupReport report;
    Detail::DatabaseFileIdentity sourceIdentity;
    std::shared_ptr<const Detail::MigrationBackupArtifactLease> backupArtifact;
};

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The database operation was cancelled before Forge Conductor could " +
                    std::string{action} + "."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The database operation deadline expired before Forge Conductor could " +
                    std::string{action} + "."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The database operation context could not be validated."));
    }
}

[[nodiscard]] Domain::Result<BackupNames> makeBackupNames(
    const DatabaseNamespaceLease& sourceNamespace,
    const Domain::OperationContext& context,
    const bool migration) noexcept
{
    try {
        const std::string& operation = context.operationId.value();
        std::wstring operationWide{operation.begin(), operation.end()};
        std::wstring finalMain = sourceNamespace.leafName(DatabaseLeafRole::Main);
        finalMain += migration ? L".pre-migration." : L".backup.";
        finalMain += operationWide;
        finalMain += L".sqlite";
        std::wstring stageMain = finalMain + L".stage";
        std::wstring finalLock = L"backup-" + operationWide + L".lock";
        std::wstring stageLock = L"backup-stage-" + operationWide + L".lock";

        if (finalMain.size() > DatabaseNamespaceLease::MaximumLeafNameCharacters ||
            stageMain.size() > DatabaseNamespaceLease::MaximumLeafNameCharacters ||
            finalLock.size() > DatabaseNamespaceLease::MaximumLeafNameCharacters ||
            stageLock.size() > DatabaseNamespaceLease::MaximumLeafNameCharacters) {
            return Domain::Result<BackupNames>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The bounded database backup leaf names exceed the Windows namespace limit."));
        }
        return Domain::Result<BackupNames>::success(BackupNames{
            std::move(stageMain), std::move(stageLock),
            std::move(finalMain), std::move(finalLock)});
    } catch (...) {
        return Domain::Result<BackupNames>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded database backup leaf names could not be created."));
    }
}

[[nodiscard]] Domain::Result<void> quickCheck(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context) noexcept
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
            "The online database backup quick-check returned no result."));
    }
    auto result = statement.columnText(0, 64U);
    if (!result) {
        return Domain::Result<void>::failure(std::move(result).error());
    }
    if (!result.value().has_value() || *result.value() != "ok") {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The online database backup failed its quick-check."));
    }
    stepped = statement.step();
    if (!stepped) {
        return Domain::Result<void>::failure(std::move(stepped).error());
    }
    if (stepped.value() != WinsqliteStepResult::Done) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The online database backup quick-check returned multiple rows."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> cleanupStage(
    const std::shared_ptr<DatabaseNamespaceLease>& stageNamespace,
    const Domain::OperationContext& context) noexcept
{
    const std::array stages{stageNamespace};
    auto cleaned = DatabaseNamespaceLease::cleanupClosedStages(stages, context);
    if (!cleaned) {
        return Domain::Result<void>::failure(std::move(cleaned).error());
    }
    return Domain::Result<void>::success();
}

void cleanupOwnedPublishedArtifactAfterFailure(
    const std::shared_ptr<DatabaseNamespaceLease>& artifactNamespace,
    const Detail::DatabaseFileIdentity& artifactIdentity,
    Domain::Error& originalError) noexcept
{
    try {
        auto cleaned = artifactNamespace->deleteClosedLeaf(
            DatabaseLeafRole::Main, artifactIdentity);
        if (!cleaned) {
            originalError.message +=
                " Exact cleanup of the owned unverified backup artifact also failed: ";
            originalError.message += cleaned.error().message;
        }
    } catch (...) {
        // Preserve the operation's original typed failure even if diagnostic text
        // cannot be extended under memory pressure.
    }
}

class PublishedArtifactRollback final {
public:
    PublishedArtifactRollback(
        std::shared_ptr<DatabaseNamespaceLease> artifactNamespace,
        const Detail::DatabaseFileIdentity artifactIdentity) noexcept
        : artifactNamespace_{std::move(artifactNamespace)},
          artifactIdentity_{artifactIdentity}
    {
    }

    ~PublishedArtifactRollback() noexcept
    {
        if (active_ && artifactNamespace_) {
            static_cast<void>(artifactNamespace_->deleteClosedLeaf(
                DatabaseLeafRole::Main, artifactIdentity_));
        }
    }

    PublishedArtifactRollback(const PublishedArtifactRollback&) = delete;
    PublishedArtifactRollback& operator=(const PublishedArtifactRollback&) = delete;
    PublishedArtifactRollback(PublishedArtifactRollback&&) = delete;
    PublishedArtifactRollback& operator=(PublishedArtifactRollback&&) = delete;

    void dismiss() noexcept { active_ = false; }

private:
    std::shared_ptr<DatabaseNamespaceLease> artifactNamespace_;
    Detail::DatabaseFileIdentity artifactIdentity_{};
    bool active_{true};
};

[[nodiscard]] Domain::Result<void> cleanupLockedStage(
    const std::shared_ptr<DatabaseNamespaceLease>& stageNamespace,
    DatabaseMigrationLock& stageLock,
    const Domain::OperationContext& originalContext) noexcept
{
    try {
        const Domain::OperationContext cleanupContext{
            originalContext.operationId,
            std::chrono::steady_clock::now() + std::chrono::seconds{5},
            {},
            originalContext.correlationId};
        auto cleaned = stageNamespace->cleanupClosedStageWithLock(
            stageLock, cleanupContext);
        if (!cleaned) {
            return Domain::Result<void>::failure(std::move(cleaned).error());
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The locked backup stage could not be cleaned safely."));
    }
}

void cleanupLockedStageAfterFailure(
    const std::shared_ptr<DatabaseNamespaceLease>& stageNamespace,
    DatabaseMigrationLock& stageLock,
    const Domain::OperationContext& originalContext,
    Domain::Error& originalError) noexcept
{
    try {
        auto cleaned = cleanupLockedStage(
            stageNamespace, stageLock, originalContext);
        if (!cleaned) {
            originalError.message +=
                " The caller-locked backup stage cleanup also failed: ";
            originalError.message += cleaned.error().message;
        }
    } catch (...) {
        // Preserve the operation's original typed failure under memory pressure.
    }
}

[[nodiscard]] Domain::Result<std::int64_t> writeAndVerifyStage(
    WinsqliteConnection& source,
    const std::shared_ptr<DatabaseNamespaceLease>& stageNamespace,
    const Domain::OperationContext& context) noexcept
{
    auto stableStage = stageNamespace->revalidateCohort();
    if (!stableStage) {
        return Domain::Result<std::int64_t>::failure(std::move(stableStage).error());
    }
    auto stageVfsResult = AnchoredSqliteVfs::create(stageNamespace);
    if (!stageVfsResult) {
        return Domain::Result<std::int64_t>::failure(std::move(stageVfsResult).error());
    }
    auto stageVfs = std::move(stageVfsResult).value();
    WinsqliteConnectionOptions destinationOptions{
        std::string{stageVfs->vfsName()},
        WinsqliteOpenMode::ReadWriteCreate,
        WinsqliteSynchronousMode::Full,
        WinsqliteJournalMode::Delete,
        stageNamespace};
    auto destinationResult = WinsqliteConnection::open(
        stageNamespace->canonicalMainDatabasePath(), destinationOptions, context);
    if (!destinationResult) {
        return Domain::Result<std::int64_t>::failure(std::move(destinationResult).error());
    }
    auto destination = std::move(destinationResult).value();

    std::int64_t pageCount{};
    {
        auto backupResult = WinsqliteBackup::begin(source, destination, context);
        if (!backupResult) {
            return Domain::Result<std::int64_t>::failure(std::move(backupResult).error());
        }
        auto backup = std::move(backupResult).value();
        auto completed = backup.runToCompletion(128);
        if (!completed) {
            return Domain::Result<std::int64_t>::failure(std::move(completed).error());
        }
        if (!completed.value().complete || completed.value().remainingPages != 0 ||
            completed.value().totalPages < 1) {
            return Domain::Result<std::int64_t>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The online database backup did not report a complete bounded copy."));
        }
        pageCount = completed.value().totalPages;
    }

    auto checked = quickCheck(destination, context);
    if (!checked) {
        return Domain::Result<std::int64_t>::failure(std::move(checked).error());
    }
    auto closed = destination.close(context);
    if (!closed) {
        return Domain::Result<std::int64_t>::failure(std::move(closed).error());
    }
    // sqlite3_backup copies the source header, including its WAL journal mode,
    // over the destination's initial configuration. Reopen once through the
    // same anchored VFS to convert the verified copy into a standalone DELETE-
    // journal artifact before it can be published without sidecars.
    WinsqliteConnectionOptions standaloneOptions{
        std::string{stageVfs->vfsName()},
        WinsqliteOpenMode::ReadWriteExisting,
        WinsqliteSynchronousMode::Full,
        WinsqliteJournalMode::Delete,
        stageNamespace};
    auto standaloneResult = WinsqliteConnection::open(
        stageNamespace->canonicalMainDatabasePath(), standaloneOptions, context);
    if (!standaloneResult) {
        return Domain::Result<std::int64_t>::failure(
            std::move(standaloneResult).error());
    }
    auto standalone = std::move(standaloneResult).value();
    checked = quickCheck(standalone, context);
    if (!checked) {
        return Domain::Result<std::int64_t>::failure(std::move(checked).error());
    }
    closed = standalone.close(context);
    if (!closed) {
        return Domain::Result<std::int64_t>::failure(std::move(closed).error());
    }
    closed = stageVfs->close();
    if (!closed) {
        return Domain::Result<std::int64_t>::failure(std::move(closed).error());
    }
    stableStage = stageNamespace->revalidateCohort();
    if (!stableStage) {
        return Domain::Result<std::int64_t>::failure(std::move(stableStage).error());
    }
    return Domain::Result<std::int64_t>::success(pageCount);
}

[[nodiscard]] Domain::Result<std::shared_ptr<const Detail::MigrationBackupArtifactLease>>
verifyPublishedBackup(
    const std::shared_ptr<DatabaseNamespaceLease>& finalNamespace,
    DatabaseLeafLease publishedLeaf,
    const Domain::OperationContext& context) noexcept
{
    if (!publishedLeaf || publishedLeaf.role() != DatabaseLeafRole::Main) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Published-backup verification requires its retained exact main leaf."));
    }
    auto validArtifact = finalNamespace->revalidateRetainedLeaf(publishedLeaf);
    if (!validArtifact) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(validArtifact).error());
    }
    auto finalVfsResult = AnchoredSqliteVfs::createFrozenReadOnlyVerification(
        finalNamespace, publishedLeaf);
    if (!finalVfsResult) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(finalVfsResult).error());
    }
    auto finalVfs = std::move(finalVfsResult).value();
    WinsqliteConnectionOptions options{
        std::string{finalVfs->vfsName()},
        WinsqliteOpenMode::ReadOnlyExisting,
        WinsqliteSynchronousMode::Full,
        WinsqliteJournalMode::WriteAheadLog,
        finalNamespace};
    auto connectionResult = WinsqliteConnection::open(
        finalNamespace->canonicalMainDatabasePath(), options, context);
    if (!connectionResult) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(connectionResult).error());
    }
    auto connection = std::move(connectionResult).value();
    auto checked = quickCheck(connection, context);
    if (!checked) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(checked).error());
    }
    auto closed = connection.close(context);
    if (!closed) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(closed).error());
    }
    closed = finalVfs->close();
    if (!closed) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(closed).error());
    }
    auto currentArtifact = finalNamespace->revalidateRetainedLeaf(publishedLeaf);
    if (!currentArtifact) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                std::move(currentArtifact).error());
    }
    std::shared_ptr<const Detail::MigrationBackupArtifactLease> backupArtifact;
    try {
        backupArtifact = std::make_shared<Detail::MigrationBackupArtifactLease>(
            finalNamespace, std::move(publishedLeaf));
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The immutable migration-backup artifact lease could not be retained."));
    }
    return Domain::Result<
        std::shared_ptr<const Detail::MigrationBackupArtifactLease>>::success(
            std::move(backupArtifact));
}

[[nodiscard]] Domain::Result<BackupOutcome> createBackup(
    WinsqliteConnection& source,
    const std::shared_ptr<DatabaseNamespaceLease>& sourceNamespace,
    const Domain::OperationContext& context,
    const bool migration) noexcept
{
    try {
        if (!sourceNamespace) {
            return Domain::Result<BackupOutcome>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "An online database backup requires an anchored source namespace."));
        }
        if (!source.belongsToNamespace(*sourceNamespace)) {
            return Domain::Result<BackupOutcome>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "An online database backup requires a connection for the exact supplied namespace."));
        }
        auto validContext = validateContext(context, "create an online backup");
        if (!validContext) {
            return Domain::Result<BackupOutcome>::failure(std::move(validContext).error());
        }
        auto sourceValid = sourceNamespace->revalidate();
        if (!sourceValid) {
            return Domain::Result<BackupOutcome>::failure(std::move(sourceValid).error());
        }
        auto sourceCohort = sourceNamespace->revalidateCohort();
        if (!sourceCohort) {
            return Domain::Result<BackupOutcome>::failure(std::move(sourceCohort).error());
        }
        auto sourceLeaf = sourceNamespace->openLeaf(
            DatabaseLeafRole::Main,
            DatabaseLeafDisposition::OpenExisting,
            FILE_READ_ATTRIBUTES);
        if (!sourceLeaf) {
            return Domain::Result<BackupOutcome>::failure(std::move(sourceLeaf).error());
        }
        const Detail::DatabaseFileIdentity sourceIdentity = sourceLeaf.value().identity();

        auto names = makeBackupNames(*sourceNamespace, context, migration);
        if (!names) {
            return Domain::Result<BackupOutcome>::failure(std::move(names).error());
        }
        auto stageResult = DatabaseNamespaceLease::create(
            sourceNamespace->canonicalDirectory(),
            names.value().stageMain,
            names.value().stageLock);
        if (!stageResult) {
            return Domain::Result<BackupOutcome>::failure(std::move(stageResult).error());
        }
        auto stageNamespace = std::move(stageResult).value();
        auto finalResult = DatabaseNamespaceLease::create(
            sourceNamespace->canonicalDirectory(),
            names.value().finalMain,
            names.value().finalLock);
        if (!finalResult) {
            return Domain::Result<BackupOutcome>::failure(std::move(finalResult).error());
        }
        auto finalNamespace = std::move(finalResult).value();
        auto reportPath = Domain::PathText::create(
            finalNamespace->canonicalUtf8Path(DatabaseLeafRole::Main));
        if (!reportPath) {
            return Domain::Result<BackupOutcome>::failure(
                std::move(reportPath).error());
        }

        auto finalExists = finalNamespace->accessLeaf(
            DatabaseLeafRole::Main, DatabaseLeafAccess::Exists);
        if (!finalExists) {
            return Domain::Result<BackupOutcome>::failure(std::move(finalExists).error());
        }
        if (finalExists.value()) {
            return Domain::Result<BackupOutcome>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "The operation-scoped database backup destination already exists."));
        }
        auto cleaned = cleanupStage(stageNamespace, context);
        if (!cleaned) {
            return Domain::Result<BackupOutcome>::failure(std::move(cleaned).error());
        }
        auto stageLockResult = stageNamespace->acquireMigrationLock(context);
        if (!stageLockResult) {
            return Domain::Result<BackupOutcome>::failure(
                std::move(stageLockResult).error());
        }
        DatabaseMigrationLock stageLock = std::move(stageLockResult).value();
        auto stageCohort = stageNamespace->revalidateCohort();
        if (!stageCohort) {
            Domain::Error error = std::move(stageCohort).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        auto finalCohort = finalNamespace->revalidateCohort();
        if (!finalCohort) {
            Domain::Error error = std::move(finalCohort).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }

        auto pageCount = writeAndVerifyStage(source, stageNamespace, context);
        if (!pageCount) {
            Domain::Error error = std::move(pageCount).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        sourceCohort = sourceNamespace->revalidateCohort();
        if (!sourceCohort) {
            Domain::Error error = std::move(sourceCohort).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        stageCohort = stageNamespace->revalidateCohort();
        if (!stageCohort) {
            Domain::Error error = std::move(stageCohort).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        finalCohort = finalNamespace->revalidateCohort();
        if (!finalCohort) {
            Domain::Error error = std::move(finalCohort).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        auto published = stageNamespace->publishClosedMainToWithSourceLockAndRetain(
            *finalNamespace, stageLock, context);
        if (!published) {
            Domain::Error error = std::move(published).error();
            cleanupLockedStageAfterFailure(stageNamespace, stageLock, context, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        const Detail::DatabaseFileIdentity publishedIdentity =
            published.value().identity();
        PublishedArtifactRollback publishedRollback{
            finalNamespace, publishedIdentity};
        DatabaseLeafLease publishedLeaf = std::move(published).value();
        auto stageReleased = cleanupLockedStage(stageNamespace, stageLock, context);
        if (!stageReleased) {
            Domain::Error error = std::move(stageReleased).error();
            publishedLeaf = DatabaseLeafLease{};
            cleanupOwnedPublishedArtifactAfterFailure(
                finalNamespace, publishedIdentity, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        stageCohort = stageNamespace->revalidateCohort();
        if (!stageCohort) {
            Domain::Error error = std::move(stageCohort).error();
            publishedLeaf = DatabaseLeafLease{};
            cleanupOwnedPublishedArtifactAfterFailure(
                finalNamespace, publishedIdentity, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        auto retainedFinal = finalNamespace->revalidateRetainedLeaf(publishedLeaf);
        if (!retainedFinal) {
            Domain::Error error = std::move(retainedFinal).error();
            publishedLeaf = DatabaseLeafLease{};
            cleanupOwnedPublishedArtifactAfterFailure(
                finalNamespace, publishedIdentity, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        auto verified = verifyPublishedBackup(
            finalNamespace, std::move(publishedLeaf), context);
        if (!verified) {
            Domain::Error error = std::move(verified).error();
            cleanupOwnedPublishedArtifactAfterFailure(
                finalNamespace, publishedIdentity, error);
            return Domain::Result<BackupOutcome>::failure(std::move(error));
        }
        auto backupArtifact = std::move(verified).value();
        BackupOutcome outcome{
            DatabaseBackupReport{
                std::move(reportPath).value(), pageCount.value(), true},
            sourceIdentity,
            std::move(backupArtifact)};
        auto result = Domain::Result<BackupOutcome>::success(std::move(outcome));
        publishedRollback.dismiss();
        return result;
    } catch (...) {
        return Domain::Result<BackupOutcome>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The online database backup could not be created with bounded resources."));
    }
}

} // namespace

DatabaseBackupCoordinator::DatabaseBackupCoordinator(
    Migrations::SchemaAssessment assessment,
    Detail::DatabaseFileIdentity sourceIdentity,
    DatabaseBackupReport report,
    std::string evidenceId,
    std::shared_ptr<Detail::WinsqliteTransactionLifetime> transactionLifetime,
    std::shared_ptr<const Detail::MigrationBackupArtifactLease> backupArtifact) noexcept
    : assessment_{assessment},
      sourceIdentity_{sourceIdentity},
      report_{std::move(report)},
      evidenceId_{std::move(evidenceId)},
      transactionLifetime_{std::move(transactionLifetime)},
      backupArtifact_{std::move(backupArtifact)}
{
}

Domain::Result<DatabaseBackupCoordinator> DatabaseBackupCoordinator::createMigrationBackup(
    WinsqliteConnection& readOnlySource,
    Detail::WinsqliteTransaction& admittedTransaction,
    std::shared_ptr<DatabaseNamespaceLease> sourceNamespace,
    const Migrations::SchemaAssessment& sourceAssessment,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto transactionLifetime = admittedTransaction.lifetime_;
        auto readOnlyState = readOnlySource.state_;
        if (!admittedTransaction.active_ ||
            admittedTransaction.connectionIdentity_ == nullptr ||
            admittedTransaction.namespaceAuthority_ == nullptr ||
            readOnlyState == nullptr || !readOnlyState->readOnly_ ||
            readOnlyState.get() == admittedTransaction.connectionIdentity_ ||
            admittedTransaction.namespaceAuthority_ != sourceNamespace.get() ||
            readOnlyState->namespaceAuthority_.get() != sourceNamespace.get() ||
            transactionLifetime == nullptr) {
            return Domain::Result<DatabaseBackupCoordinator>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A migration backup requires read-only and admitted connections for the exact supplied namespace."));
        }
        std::unique_lock transactionLock{transactionLifetime->mutex};
        if (!transactionLifetime->active) {
            return Domain::Result<DatabaseBackupCoordinator>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A migration backup cannot outlive its admitted transaction."));
        }
        if (!sourceAssessment.requiresOnlineBackup ||
            sourceAssessment.sourceVersion >= sourceAssessment.targetVersion) {
            return Domain::Result<DatabaseBackupCoordinator>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A migration backup is allowed only for a recognized older schema."));
        }
        std::string evidenceId{"p07-migration-backup:"};
        evidenceId += context.operationId.value();
        auto outcome = createBackup(
            readOnlySource, sourceNamespace, context, true);
        if (!outcome) {
            return Domain::Result<DatabaseBackupCoordinator>::failure(
                std::move(outcome).error());
        }
        auto backupOutcome = std::move(outcome).value();
        return Domain::Result<DatabaseBackupCoordinator>::success(
            DatabaseBackupCoordinator{
                sourceAssessment,
                backupOutcome.sourceIdentity,
                std::move(backupOutcome.report),
                std::move(evidenceId),
                std::move(transactionLifetime),
                std::move(backupOutcome.backupArtifact)});
    } catch (...) {
        return Domain::Result<DatabaseBackupCoordinator>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The verified migration-backup receipt could not be retained."));
    }
}

Domain::Result<DatabaseBackupReport> DatabaseBackupCoordinator::createOperationalBackup(
    WinsqliteConnection& source,
    std::shared_ptr<DatabaseNamespaceLease> sourceNamespace,
    const Domain::OperationContext& context) noexcept
{
    auto outcome = createBackup(source, sourceNamespace, context, false);
    if (!outcome) {
        return Domain::Result<DatabaseBackupReport>::failure(std::move(outcome).error());
    }
    auto backupOutcome = std::move(outcome).value();
    return Domain::Result<DatabaseBackupReport>::success(std::move(backupOutcome.report));
}

Domain::Result<std::size_t> DatabaseBackupCoordinator::cleanupStaleBackupStages(
    std::shared_ptr<DatabaseNamespaceLease> sourceNamespace,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (!sourceNamespace) {
            return Domain::Result<std::size_t>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Stale backup cleanup requires an anchored source namespace."));
        }
        auto validContext = validateContext(context, "clean stale backup stages");
        if (!validContext) {
            return Domain::Result<std::size_t>::failure(
                std::move(validContext).error());
        }
        auto stableSource = sourceNamespace->revalidate();
        if (!stableSource) {
            return Domain::Result<std::size_t>::failure(
                std::move(stableSource).error());
        }

        constexpr std::wstring_view StageSuffix = L".sqlite.stage";
        const std::wstring backupPrefix =
            sourceNamespace->leafName(DatabaseLeafRole::Main) + L".backup.";
        const std::wstring migrationPrefix =
            sourceNamespace->leafName(DatabaseLeafRole::Main) + L".pre-migration.";
        std::vector<std::shared_ptr<DatabaseNamespaceLease>> stages;
        stages.reserve(DatabaseNamespaceLease::MaximumStaleStageLeasesPerCleanup);
        std::vector<std::string> stagedOperations;
        stagedOperations.reserve(DatabaseNamespaceLease::MaximumStaleStageLeasesPerCleanup);

        const auto collect = [&](const std::wstring& prefix) -> Domain::Result<void> {
            auto matching = sourceNamespace->enumerateMatchingLeafNames(
                prefix,
                StageSuffix,
                DatabaseNamespaceLease::MaximumStaleStageLeasesPerCleanup,
                context);
            if (!matching) {
                return Domain::Result<void>::failure(std::move(matching).error());
            }
            for (const auto& leafName : matching.value()) {
                if (leafName.size() != prefix.size() + 36U + StageSuffix.size() ||
                    leafName.compare(0U, prefix.size(), prefix) != 0 ||
                    leafName.compare(
                        leafName.size() - StageSuffix.size(),
                        StageSuffix.size(),
                        StageSuffix) != 0) {
                    continue;
                }
                const std::wstring_view operationWide{
                    leafName.data() + prefix.size(), 36U};
                std::string operation;
                operation.reserve(operationWide.size());
                bool ascii{true};
                for (const wchar_t character : operationWide) {
                    if (character > 0x7f) {
                        ascii = false;
                        break;
                    }
                    operation.push_back(static_cast<char>(character));
                }
                if (!ascii) {
                    continue;
                }
                auto parsed = Domain::OperationId::parse(operation);
                if (!parsed || parsed.value().value() != operation) {
                    continue;
                }
                if (stages.size() ==
                    DatabaseNamespaceLease::MaximumStaleStageLeasesPerCleanup) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "Stale backup cleanup exceeded its 32-stage bound."));
                }
                std::wstring lockName{L"backup-stage-"};
                lockName.append(operationWide);
                lockName += L".lock";
                auto stage = DatabaseNamespaceLease::create(
                    sourceNamespace->canonicalDirectory(), leafName, lockName);
                if (!stage) {
                    return Domain::Result<void>::failure(std::move(stage).error());
                }
                stages.push_back(std::move(stage).value());
                stagedOperations.push_back(std::move(operation));
            }
            return Domain::Result<void>::success();
        };

        auto collected = collect(backupPrefix);
        if (!collected) {
            return Domain::Result<std::size_t>::failure(std::move(collected).error());
        }
        collected = collect(migrationPrefix);
        if (!collected) {
            return Domain::Result<std::size_t>::failure(std::move(collected).error());
        }
        constexpr std::wstring_view LockPrefix = L"backup-stage-";
        constexpr std::wstring_view LockSuffix = L".lock";
        auto matchingLocks = sourceNamespace->enumerateMatchingLeafNames(
            LockPrefix,
            LockSuffix,
            DatabaseNamespaceLease::MaximumStaleStageLeasesPerCleanup,
            context);
        if (!matchingLocks) {
            return Domain::Result<std::size_t>::failure(
                std::move(matchingLocks).error());
        }
        for (const auto& lockName : matchingLocks.value()) {
            if (lockName.size() != LockPrefix.size() + 36U + LockSuffix.size() ||
                lockName.compare(0U, LockPrefix.size(), LockPrefix) != 0 ||
                lockName.compare(
                    lockName.size() - LockSuffix.size(),
                    LockSuffix.size(),
                    LockSuffix) != 0) {
                continue;
            }
            const std::wstring_view operationWide{
                lockName.data() + LockPrefix.size(), 36U};
            std::string operation;
            operation.reserve(operationWide.size());
            bool ascii{true};
            for (const wchar_t character : operationWide) {
                if (character > 0x7f) {
                    ascii = false;
                    break;
                }
                operation.push_back(static_cast<char>(character));
            }
            if (!ascii) {
                continue;
            }
            auto parsed = Domain::OperationId::parse(operation);
            if (!parsed || parsed.value().value() != operation ||
                std::find(
                    stagedOperations.begin(), stagedOperations.end(), operation) !=
                    stagedOperations.end()) {
                continue;
            }
            if (stages.size() ==
                DatabaseNamespaceLease::MaximumStaleStageLeasesPerCleanup) {
                return Domain::Result<std::size_t>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "Stale backup cleanup exceeded its 32-stage bound."));
            }
            std::wstring syntheticStageName = backupPrefix;
            syntheticStageName.append(operationWide);
            syntheticStageName.append(StageSuffix);
            auto stage = DatabaseNamespaceLease::create(
                sourceNamespace->canonicalDirectory(),
                syntheticStageName,
                lockName);
            if (!stage) {
                return Domain::Result<std::size_t>::failure(std::move(stage).error());
            }
            stages.push_back(std::move(stage).value());
        }
        return DatabaseNamespaceLease::cleanupClosedStages(stages, context);
    } catch (...) {
        return Domain::Result<std::size_t>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Stale backup stages could not be discovered and cleaned safely."));
    }
}

Domain::Result<Migrations::MigrationBackupReceipt>
DatabaseBackupCoordinator::bindToTransaction(
    Detail::WinsqliteTransaction& admittedTransaction,
    const DatabaseNamespaceLease& sourceNamespace,
    const Migrations::SchemaAssessment& reassessment) const noexcept
{
    try {
        auto transactionLifetime = admittedTransaction.lifetime_;
        if (!admittedTransaction.active_ ||
            admittedTransaction.connectionIdentity_ == nullptr ||
            transactionLifetime == nullptr ||
            transactionLifetime.get() != transactionLifetime_.get()) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The migration backup belongs to a different transaction generation.",
                    true));
        }
        std::unique_lock transactionLock{transactionLifetime->mutex};
        if (!transactionLifetime->active) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The migration transaction ended before backup binding.",
                    true));
        }
        if (reassessment != assessment_) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The database schema changed after its migration backup was verified.",
                    true));
        }
        auto validNamespace = sourceNamespace.revalidate();
        if (!validNamespace) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                std::move(validNamespace).error());
        }
        auto validCohort = sourceNamespace.revalidateCohort();
        if (!validCohort) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                std::move(validCohort).error());
        }
        auto sourceLeaf = sourceNamespace.openLeaf(
            DatabaseLeafRole::Main,
            DatabaseLeafDisposition::OpenExisting,
            FILE_READ_ATTRIBUTES);
        if (!sourceLeaf) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                std::move(sourceLeaf).error());
        }
        if (sourceLeaf.value().identity() != sourceIdentity_) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The database file identity changed after its migration backup was verified.",
                    true));
        }
        Migrations::MigrationBackupReceipt receipt{
            transactionLifetime,
            reassessment,
            evidenceId_,
            backupArtifact_};
        auto validBackup = receipt.revalidateArtifact();
        if (!validBackup) {
            return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
                std::move(validBackup).error());
        }
        return Domain::Result<Migrations::MigrationBackupReceipt>::success(
            std::move(receipt));
    } catch (...) {
        return Domain::Result<Migrations::MigrationBackupReceipt>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The verified migration backup could not be bound to its transaction."));
    }
}

} // namespace ForgeConductor::Persistence::Windows
