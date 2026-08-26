#include "PersistenceTestSupport.h"

#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Persistence/Windows/DatabaseBackupCoordinator.h"
#include "Persistence/Windows/DatabaseQuarantine.h"
#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"
#include "Persistence/Windows/Detail/WinsqliteTransaction.h"

#include <nlohmann/json.hpp>
#include <winioctl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ForgeConductor::Tests {
namespace {

namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
using namespace std::chrono_literals;
using PersistenceSupport::FixedClock;
using PersistenceSupport::ScopedTestDirectory;
using PersistenceDetail::WinsqliteConnection;
using PersistenceDetail::WinsqliteOpenMode;
using PersistenceDetail::WinsqliteStepResult;
using PersistenceWindows::WindowsCentralDatabase;

inline constexpr std::wstring_view CentralMainBasename = L"store.sqlite";
inline constexpr std::wstring_view CentralMigrationLockBasename =
    L"store.sqlite.migration.lock";

struct CentralDependencies final {
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<FixedClock> clock;
};

[[nodiscard]] Domain::OperationContext operationContext(
    const std::string_view operationId,
    const std::string_view correlationId,
    const std::stop_token cancellation = {},
    const std::chrono::seconds timeout = 120s)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operationId),
        std::chrono::steady_clock::now() + timeout,
        cancellation,
        parse<Domain::CorrelationId>(correlationId)};
}

[[nodiscard]] std::filesystem::path canonicalDirectory(
    const std::filesystem::path& directory)
{
    std::error_code error;
    const auto canonical = std::filesystem::canonical(directory, error);
    require(!error, "the persistence test directory could not be canonicalized");
    return canonical;
}

[[nodiscard]] CentralDependencies makeCentralDependencies(
    const std::filesystem::path& directory)
{
    const auto monotonic = std::chrono::steady_clock::now();
    auto paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
    paths->setNow(monotonic);
    paths->dataRootResult.set(Domain::Result<Domain::PathText>::success(
        PersistenceSupport::pathText(canonicalDirectory(directory))));
    return CentralDependencies{
        std::move(paths),
        std::make_shared<Fakes::RuntimeDiagnosticsFake>(monotonic),
        std::make_shared<FixedClock>(
            Domain::UtcTimePoint{std::chrono::seconds{1'735'689'600}},
            monotonic)};
}

[[nodiscard]] std::unique_ptr<WindowsCentralDatabase> openCentral(
    const CentralDependencies& dependencies,
    const Domain::OperationContext& context)
{
    return take(WindowsCentralDatabase::open(
        dependencies.paths,
        dependencies.diagnostics,
        dependencies.clock,
        context));
}

[[nodiscard]] std::wstring widenAscii(const std::string_view value)
{
    return std::wstring{value.begin(), value.end()};
}

[[nodiscard]] std::filesystem::path pathFromText(const Domain::PathText& value)
{
    return std::filesystem::path{take(
        Infrastructure::Windows::Detail::strictUtf8ToUtf16(value.value()))};
}

[[nodiscard]] std::filesystem::path backupPathFor(
    const std::filesystem::path& directory,
    const std::string_view operationId)
{
    return canonicalDirectory(directory) /
        (std::wstring{CentralMainBasename} + L".backup." +
         widenAscii(operationId) + L".sqlite");
}

[[nodiscard]] std::filesystem::path backupLockPathFor(
    const std::filesystem::path& directory,
    const std::string_view operationId,
    const bool stage)
{
    return canonicalDirectory(directory) /
        ((stage ? L"backup-stage-" : L"backup-") + widenAscii(operationId) +
         L".lock");
}

[[nodiscard]] std::filesystem::path quarantinePathFor(
    const std::filesystem::path& directory,
    const std::string_view operationId)
{
    return canonicalDirectory(directory) /
        (std::wstring{CentralMainBasename} + L".corrupt." +
         widenAscii(operationId) + L".sqlite");
}

[[nodiscard]] std::filesystem::path sidecarPath(
    const std::filesystem::path& mainPath,
    const std::wstring_view suffix)
{
    return std::filesystem::path{mainPath.native() + std::wstring{suffix}};
}

void requireClosedCohortAbsent(
    const std::filesystem::path& mainPath,
    const std::string_view message)
{
    require(!std::filesystem::exists(mainPath), message);
    require(!std::filesystem::exists(sidecarPath(mainPath, L"-wal")), message);
    require(!std::filesystem::exists(sidecarPath(mainPath, L"-shm")), message);
    require(!std::filesystem::exists(sidecarPath(mainPath, L"-journal")), message);
}

void writeFixture(
    const std::filesystem::path& path,
    const std::string_view bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    require(output.good(), "the persistence test fixture could not be opened for writing");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    require(output.good(), "the complete persistence test fixture could not be written");
}

[[nodiscard]] PersistenceDetail::DatabaseFileIdentity fileIdentity(
    const std::filesystem::path& path)
{
    Infrastructure::Windows::Detail::UniqueHandle file{::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(file),
            "the persistence test file identity could not be opened; native error " +
                std::to_string(::GetLastError()));
    FILE_ID_INFO information{};
    require(::GetFileInformationByHandleEx(
                file.get(), FileIdInfo, &information, sizeof(information)) != FALSE,
            "the persistence test file identity could not be read; native error " +
                std::to_string(::GetLastError()));
    PersistenceDetail::DatabaseFileIdentity identity{};
    identity.volumeSerialNumber = information.VolumeSerialNumber;
    std::memcpy(
        identity.fileIdentifier.data(), information.FileId.Identifier,
        identity.fileIdentifier.size());
    return identity;
}

void createSparseFixture(
    const std::filesystem::path& path,
    const std::uint64_t logicalBytes)
{
    require(logicalBytes <= static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()),
            "the sparse fixture size exceeds the native signed range");
    Infrastructure::Windows::Detail::UniqueHandle file{::CreateFileW(
        path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(static_cast<bool>(file),
            "the sparse persistence fixture could not be created; native error " +
                std::to_string(::GetLastError()));
    DWORD returned{};
    require(::DeviceIoControl(
                file.get(), FSCTL_SET_SPARSE, nullptr, 0U, nullptr, 0U,
                &returned, nullptr) != FALSE,
            "the persistence fixture volume does not support sparse files; native error " +
                std::to_string(::GetLastError()));
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(logicalBytes);
    require(::SetFilePointerEx(file.get(), end, nullptr, FILE_BEGIN) != FALSE &&
                ::SetEndOfFile(file.get()) != FALSE &&
                ::FlushFileBuffers(file.get()) != FALSE,
            "the sparse persistence fixture size could not be committed; native error " +
                std::to_string(::GetLastError()));
    LARGE_INTEGER observed{};
    require(::GetFileSizeEx(file.get(), &observed) != FALSE &&
                observed.QuadPart == end.QuadPart,
            "the sparse persistence fixture has the wrong logical size");
}

class QuarantineBarrierObserver final
    : public PersistenceWindows::IDatabaseQuarantineObserver {
public:
    using Callback = std::function<void()>;

    QuarantineBarrierObserver(
        Callback sourceCaptured = {},
        Callback evidenceCopied = {},
        Callback manifestCommitted = {})
        : sourceCaptured_{std::move(sourceCaptured)},
          evidenceCopied_{std::move(evidenceCopied)},
          manifestCommitted_{std::move(manifestCommitted)}
    {
    }

    void onSourceCohortCaptured() noexcept override
    {
        invoke(sourceCaptured_, sourceCapturedCount_);
    }

    void onEvidenceCohortCopied() noexcept override
    {
        invoke(evidenceCopied_, evidenceCopiedCount_);
    }

    void onManifestCommitted() noexcept override
    {
        invoke(manifestCommitted_, manifestCommittedCount_);
    }

    void rethrowCallbackFailure() const
    {
        if (callbackFailure_ != nullptr) {
            std::rethrow_exception(callbackFailure_);
        }
    }

    [[nodiscard]] std::size_t sourceCapturedCount() const noexcept
    {
        return sourceCapturedCount_;
    }

    [[nodiscard]] std::size_t evidenceCopiedCount() const noexcept
    {
        return evidenceCopiedCount_;
    }

    [[nodiscard]] std::size_t manifestCommittedCount() const noexcept
    {
        return manifestCommittedCount_;
    }

private:
    void invoke(Callback& callback, std::size_t& count) noexcept
    {
        ++count;
        if (!callback || callbackFailure_ != nullptr) {
            return;
        }
        try {
            callback();
        } catch (...) {
            callbackFailure_ = std::current_exception();
        }
    }

    Callback sourceCaptured_;
    Callback evidenceCopied_;
    Callback manifestCommitted_;
    std::exception_ptr callbackFailure_;
    std::size_t sourceCapturedCount_{};
    std::size_t evidenceCopiedCount_{};
    std::size_t manifestCommittedCount_{};
};

[[nodiscard]] Domain::Error quarantineReason()
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The deliberately hostile test database is invalid.");
}

[[nodiscard]] std::string sha256(const std::string_view bytes)
{
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    const auto byteSpan = std::as_bytes(
        std::span<const char>{bytes.data(), bytes.size()});
    return take(hasher.sha256(byteSpan)).value();
}

class AnchoredDatabase final {
public:
    [[nodiscard]] static std::unique_ptr<AnchoredDatabase> open(
        const std::filesystem::path& directory,
        std::wstring basename,
        const WinsqliteOpenMode openMode,
        const Domain::OperationContext& context)
    {
        const std::wstring lockBasename = basename + L".test.lock";
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonicalDirectory(directory).native(), basename, lockBasename));
        auto vfs = take(PersistenceDetail::AnchoredSqliteVfs::create(namespaceLease));
        auto connection = take(WinsqliteConnection::open(
            namespaceLease->canonicalMainDatabasePath(),
            PersistenceDetail::WinsqliteConnectionOptions{
                std::string{vfs->vfsName()},
                openMode,
                PersistenceDetail::WinsqliteSynchronousMode::Full,
                PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
                namespaceLease},
            context));
        return std::unique_ptr<AnchoredDatabase>{new AnchoredDatabase{
            std::move(namespaceLease), std::move(vfs), std::move(connection)}};
    }

    ~AnchoredDatabase() noexcept
    {
        connection_.reset();
        if (vfs_ != nullptr) {
            static_cast<void>(vfs_->close());
        }
    }

    AnchoredDatabase(const AnchoredDatabase&) = delete;
    AnchoredDatabase& operator=(const AnchoredDatabase&) = delete;
    AnchoredDatabase(AnchoredDatabase&&) = delete;
    AnchoredDatabase& operator=(AnchoredDatabase&&) = delete;

    [[nodiscard]] WinsqliteConnection& connection()
    {
        require(connection_.has_value(), "the anchored test database is closed");
        return connection_.value();
    }

    void close(const Domain::OperationContext& context)
    {
        if (connection_.has_value()) {
            take(connection_->close(context));
            connection_.reset();
        }
        if (vfs_ != nullptr) {
            take(vfs_->close());
            vfs_.reset();
        }
        require(namespaceLease_->openVfsFileCount() == 0U,
                "an anchored test database retained a VFS file owner");
    }

private:
    AnchoredDatabase(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs,
        WinsqliteConnection connection) noexcept
        : namespaceLease_{std::move(namespaceLease)},
          vfs_{std::move(vfs)},
          connection_{std::move(connection)}
    {
    }

    std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs_;
    std::optional<WinsqliteConnection> connection_;
};

[[nodiscard]] std::int64_t queryInteger(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "the backup inspection query returned no row");
    const std::int64_t value = take(statement.columnInt64(0));
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "the backup inspection query returned multiple rows");
    return value;
}

[[nodiscard]] std::string queryText(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "the backup inspection text query returned no row");
    auto value = take(statement.columnText(0, 64U));
    require(value.has_value(), "the backup inspection text query returned null");
    std::string copied = std::move(value).value();
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "the backup inspection text query returned multiple rows");
    return copied;
}

void createProbeRows(
    WinsqliteConnection& writer,
    const Domain::OperationContext& context)
{
    take(writer.execute("PRAGMA wal_autocheckpoint=0;", context));
    auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
        writer, context));
    take(transaction.execute(
        "CREATE TABLE backup_probe("
        "id INTEGER PRIMARY KEY, value INTEGER NOT NULL);"
        "INSERT INTO backup_probe(id, value) VALUES(1, 10);"
        "INSERT INTO backup_probe(id, value) VALUES(2, 20);"
        "INSERT INTO backup_probe(id, value) VALUES(3, 30);"));
    take(transaction.commit());
}

void insertProbeRow(
    WinsqliteConnection& writer,
    const std::int64_t id,
    const std::int64_t value,
    const Domain::OperationContext& context)
{
    auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
        writer, context));
    auto statement = take(transaction.prepare(
        "INSERT INTO backup_probe(id, value) VALUES(?1, ?2);"));
    take(statement.bindInt64(1, id));
    take(statement.bindInt64(2, value));
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "the additional WAL-resident probe row was not committed");
    take(transaction.commit());
}

void createLargeWalPayload(
    WinsqliteConnection& writer,
    const Domain::OperationContext& context)
{
    take(writer.execute("PRAGMA wal_autocheckpoint=0;", context));
    auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
        writer, context));
    take(transaction.execute(
        "CREATE TABLE interrupted_backup_payload("
        "id INTEGER PRIMARY KEY, payload BLOB NOT NULL);"));
    for (std::int64_t id = 1; id <= 8; ++id) {
        auto statement = take(transaction.prepare(
            "INSERT INTO interrupted_backup_payload(id, payload) "
            "VALUES(?1, zeroblob(8388608));"));
        take(statement.bindInt64(1, id));
        require(take(statement.step()) == WinsqliteStepResult::Done,
                "the bounded cancellation payload could not be created");
    }
    take(transaction.commit());
}

void inspectBackup(
    const PersistenceWindows::DatabaseBackupReport& report,
    const std::int64_t expectedCount,
    const std::int64_t expectedSum,
    const Domain::OperationContext& context)
{
    require(report.quickCheckPassed,
            "the facade did not report a successful backup quick-check");
    require(report.pageCount > 0,
            "the facade reported a non-positive backup page count");
    const std::filesystem::path backupPath = pathFromText(report.backupPath);
    require(std::filesystem::is_regular_file(backupPath),
            "the reported backup main file does not exist");
    const bool publishedWal = std::filesystem::exists(sidecarPath(backupPath, L"-wal"));
    const bool publishedShm = std::filesystem::exists(sidecarPath(backupPath, L"-shm"));
    const bool publishedJournal =
        std::filesystem::exists(sidecarPath(backupPath, L"-journal"));
    require(!publishedWal && !publishedShm && !publishedJournal,
            "the freshly published backup retained a transient sidecar (wal=" +
                std::to_string(publishedWal) + ", shm=" +
                std::to_string(publishedShm) + ", journal=" +
                std::to_string(publishedJournal) + ")");

    auto database = AnchoredDatabase::open(
        backupPath.parent_path(), backupPath.filename().native(),
        WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryText(database->connection(), "PRAGMA main.quick_check(1);", context) == "ok",
            "the published backup failed an independent quick-check");
    require(queryInteger(
                database->connection(),
                "SELECT COUNT(*) FROM backup_probe;",
                context) == expectedCount,
            "the published backup did not preserve every committed row");
    require(queryInteger(
                database->connection(),
                "SELECT SUM(value) FROM backup_probe;",
                context) == expectedSum,
            "the published backup changed committed row values");
    database->close(context);
    require(!std::filesystem::exists(sidecarPath(backupPath, L"-wal")) &&
                !std::filesystem::exists(sidecarPath(backupPath, L"-shm")) &&
                !std::filesystem::exists(sidecarPath(backupPath, L"-journal")),
            "the published backup retained a transient sidecar");
}

[[nodiscard]] std::jthread cancelWhenPathAppears(
    const std::filesystem::path& path,
    std::stop_source& cancellation,
    std::atomic_bool& observed)
{
    return std::jthread{[path, &cancellation, &observed](const std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        const HANDLE changeNotification = ::FindFirstChangeNotificationW(
            path.parent_path().c_str(),
            FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        while (!stop.stop_requested() &&
               std::chrono::steady_clock::now() < deadline) {
            std::error_code error;
            if (std::filesystem::exists(path, error) && !error) {
                observed.store(true, std::memory_order_release);
                cancellation.request_stop();
                break;
            }
            if (changeNotification == INVALID_HANDLE_VALUE) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            const DWORD waitResult = ::WaitForSingleObject(changeNotification, 10U);
            if (waitResult == WAIT_OBJECT_0) {
                if (::FindNextChangeNotification(changeNotification) == FALSE) {
                    break;
                }
            } else if (waitResult == WAIT_FAILED) {
                break;
            }
        }
        if (changeNotification != INVALID_HANDLE_VALUE) {
            static_cast<void>(::FindCloseChangeNotification(changeNotification));
        }
    }};
}

void verifyQuarantineManifest(
    const std::filesystem::path& directory,
    const std::string_view operationId,
    const std::string_view expectedBytes,
    const Domain::Error& error)
{
    const std::filesystem::path sourcePath =
        canonicalDirectory(directory) / CentralMainBasename;
    const std::filesystem::path quarantinePath =
        quarantinePathFor(directory, operationId);
    const std::filesystem::path manifestPath =
        sidecarPath(quarantinePath, L".manifest.json");
    const std::string evidenceId = "p07-quarantine:" + std::string{operationId};

    require(error.code == Domain::ErrorCodes::IntegrityFailure,
            "a corrupt database was not reported as an integrity failure");
    require(error.evidenceId.has_value() && error.evidenceId.value() == evidenceId,
            "the quarantine error did not carry its operation-scoped evidence ID");
    require(error.message.find(PersistenceSupport::pathText(quarantinePath).value()) !=
                std::string::npos &&
                error.message.find(PersistenceSupport::pathText(manifestPath).value()) !=
                    std::string::npos,
            "the quarantine error claimed no concrete preserved evidence paths");
    require(!std::filesystem::exists(sourcePath),
            "quarantine left the corrupt source main file in service");
    require(std::filesystem::is_regular_file(quarantinePath),
            "quarantine did not preserve the corrupt source main file");
    require(std::filesystem::is_regular_file(manifestPath),
            "quarantine did not publish its evidence manifest");
    require(PersistenceSupport::readFixture(
                quarantinePath, expectedBytes.size() + 1U) == expectedBytes,
            "the quarantined main file differs from the corrupt source bytes");

    const auto manifest = nlohmann::json::parse(
        PersistenceSupport::readFixture(manifestPath, 64U * 1024U));
    require(manifest.at("schema_version").get<int>() == 1,
            "the quarantine manifest schema version is wrong");
    require(manifest.at("evidence_id").get<std::string>() == evidenceId,
            "the quarantine manifest evidence ID does not match the error");
    require(manifest.at("copy_semantics").get<std::string>() ==
                "anchored_stream_copy_before_source_removal",
            "the quarantine manifest recorded the wrong copy semantics");
    require(manifest.at("source_cleanup_policy").get<std::string>() ==
                "exact_identity_delete_after_evidence_commit",
            "the quarantine manifest recorded the wrong source cleanup policy");
    require(manifest.at("reason").at("code").get<std::string>() ==
                Domain::ErrorCodes::IntegrityFailure,
            "the quarantine manifest recorded the wrong failure reason");
    require(manifest.at("source_main").get<std::string>() ==
                PersistenceSupport::pathText(sourcePath).value(),
            "the quarantine manifest recorded the wrong source path");
    require(manifest.at("quarantine_main").get<std::string>() ==
                PersistenceSupport::pathText(quarantinePath).value(),
            "the quarantine manifest recorded the wrong evidence path");

    struct QuarantineRole final {
        std::string_view name;
        std::wstring_view suffix;
    };
    constexpr std::array QuarantineRoles{
        QuarantineRole{"main", L""},
        QuarantineRole{"wal", L"-wal"},
        QuarantineRole{"shared_memory", L"-shm"},
        QuarantineRole{"journal", L"-journal"}};

    const auto& files = manifest.at("files");
    require(files.is_array() && !files.empty() &&
                files.size() <= QuarantineRoles.size(),
            "the corrupt cohort produced an invalid manifest file set");
    std::array<bool, QuarantineRoles.size()> seen{};
    for (const auto& file : files) {
        const std::string role = file.at("role").get<std::string>();
        const auto roleEntry = std::find_if(
            QuarantineRoles.begin(), QuarantineRoles.end(),
            [&role](const QuarantineRole& candidate) {
                return candidate.name == role;
            });
        require(roleEntry != QuarantineRoles.end(),
                "the quarantine manifest contains an unknown database role");
        const std::size_t roleIndex = static_cast<std::size_t>(
            std::distance(QuarantineRoles.begin(), roleEntry));
        require(!seen[roleIndex],
                "the quarantine manifest contains a duplicate database role");
        seen[roleIndex] = true;

        const std::filesystem::path evidencePath = roleEntry->suffix.empty()
            ? quarantinePath
            : sidecarPath(quarantinePath, roleEntry->suffix);
        const std::filesystem::path originalPath = roleEntry->suffix.empty()
            ? sourcePath
            : sidecarPath(sourcePath, roleEntry->suffix);
        require(file.at("path").get<std::string>() ==
                    PersistenceSupport::pathText(evidencePath).value(),
                "the quarantine manifest file path is wrong");
        require(std::filesystem::is_regular_file(evidencePath),
                "a manifested quarantine artifact is missing");
        const std::string preserved = PersistenceSupport::readFixture(evidencePath);
        require(file.at("bytes").get<std::uint64_t>() == preserved.size(),
                "the quarantine manifest file size is wrong");
        require(file.at("sha256").get<std::string>() == sha256(preserved),
                "the quarantine manifest SHA-256 does not match the preserved bytes");
        require(!file.at("source_file_identity").get<std::string>().empty(),
                "the quarantine manifest omitted the source file identity");
        require(!file.at("file_identity").get<std::string>().empty(),
                "the quarantine manifest omitted the preserved file identity");
        require(!std::filesystem::exists(originalPath),
                "quarantine retained a manifested source cohort leaf in service");
        if (roleEntry->name == "main") {
            require(preserved == expectedBytes,
                    "the quarantined main file differs from the corrupt source bytes");
        }
    }
    require(seen[0], "the quarantine manifest omitted the main database role");
}

void requireCommittedMainEvidence(
    const std::filesystem::path& directory,
    const std::string_view operationId,
    const std::string_view expectedBytes)
{
    const std::filesystem::path evidencePath =
        quarantinePathFor(directory, operationId);
    const std::filesystem::path manifestPath =
        sidecarPath(evidencePath, L".manifest.json");
    require(PersistenceSupport::readFixture(
                evidencePath, expectedBytes.size() + 1U) == expectedBytes,
            "committed quarantine evidence bytes differ from the retained source");
    const auto manifest = nlohmann::json::parse(
        PersistenceSupport::readFixture(manifestPath, 64U * 1024U));
    require(manifest.at("evidence_id").get<std::string>() ==
                "p07-quarantine:" + std::string{operationId},
            "committed quarantine evidence has the wrong evidence ID");
    const auto& files = manifest.at("files");
    require(files.is_array() && files.size() == 1U &&
                files.front().at("role").get<std::string>() == "main",
            "the main-only quarantine fixture produced the wrong manifest role set");
    const auto& main = files.front();
    require(main.at("bytes").get<std::uint64_t>() == expectedBytes.size() &&
                main.at("sha256").get<std::string>() == sha256(expectedBytes),
            "committed quarantine evidence has the wrong size or SHA-256");
    require(!main.at("source_file_identity").get<std::string>().empty() &&
                !main.at("file_identity").get<std::string>().empty(),
            "committed quarantine evidence omitted a file identity");
    require(!std::filesystem::exists(sidecarPath(evidencePath, L"-wal")) &&
                !std::filesystem::exists(sidecarPath(evidencePath, L"-shm")) &&
                !std::filesystem::exists(sidecarPath(evidencePath, L"-journal")),
            "main-only committed quarantine evidence gained a sidecar");
}

void requireNoQuarantineEvidence(
    const std::filesystem::path& directory,
    const std::string_view operationId,
    const std::string_view message)
{
    const std::filesystem::path evidencePath =
        quarantinePathFor(directory, operationId);
    requireClosedCohortAbsent(evidencePath, message);
    require(!std::filesystem::exists(
                sidecarPath(evidencePath, L".manifest.json")),
            message);
}

void testFacadeBackupIncludesWalResidentRows()
{
    constexpr std::string_view OpenId = "70000000-0000-4000-8000-000000000001";
    constexpr std::string_view BackupId = "70000000-0000-4000-8000-000000000002";
    ScopedTestDirectory directory{L"backup-wal"};
    const auto dependencies = makeCentralDependencies(directory.path());
    const auto openContext = operationContext(OpenId, "p07-backup-wal-open");
    auto database = openCentral(dependencies, openContext);
    auto writer = AnchoredDatabase::open(
        directory.path(), std::wstring{CentralMainBasename},
        WinsqliteOpenMode::ReadWriteExisting, openContext);
    createProbeRows(writer->connection(), openContext);

    const std::filesystem::path walPath =
        canonicalDirectory(directory.path()) / L"store.sqlite-wal";
    std::error_code sizeError;
    const auto walBytes = std::filesystem::file_size(walPath, sizeError);
    require(!sizeError && walBytes > 32U,
            "the committed probe rows were not resident in the WAL precondition");

    const auto backupContext = operationContext(BackupId, "p07-backup-wal-copy");
    const auto report = take(database->createOnlineBackup(backupContext));
    const std::filesystem::path expectedPath = backupPathFor(directory.path(), BackupId);
    require(pathFromText(report.backupPath) == expectedPath,
            "the facade reported a non-operation-scoped backup path");
    inspectBackup(report, 3, 60, backupContext);

    writer->close(openContext);
    take(database->close(openContext));
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "the facade retained its runtime database ownership after close");
}

void testOperationScopedBackupCollision()
{
    constexpr std::string_view OpenId = "70000000-0000-4000-8000-000000000011";
    constexpr std::string_view FirstBackupId = "70000000-0000-4000-8000-000000000012";
    constexpr std::string_view SecondBackupId = "70000000-0000-4000-8000-000000000013";
    ScopedTestDirectory directory{L"backup-collision"};
    const auto dependencies = makeCentralDependencies(directory.path());
    const auto openContext = operationContext(OpenId, "p07-backup-collision-open");
    auto database = openCentral(dependencies, openContext);
    auto writer = AnchoredDatabase::open(
        directory.path(), std::wstring{CentralMainBasename},
        WinsqliteOpenMode::ReadWriteExisting, openContext);
    createProbeRows(writer->connection(), openContext);

    const auto firstContext = operationContext(
        FirstBackupId, "p07-backup-collision-first");
    const auto firstReport = take(database->createOnlineBackup(firstContext));
    const auto firstPath = pathFromText(firstReport.backupPath);
    const std::string firstBytes = PersistenceSupport::readFixture(firstPath);

    insertProbeRow(writer->connection(), 4, 40, openContext);
    const auto collision = database->createOnlineBackup(firstContext);
    requireError(collision, Domain::ErrorCodes::Conflict,
                 "an operation-scoped backup collision overwrote existing evidence");
    require(!collision.error().evidenceId.has_value(),
            "a failed backup collision falsely claimed evidence");
    require(PersistenceSupport::readFixture(firstPath) == firstBytes,
            "a backup collision changed the previously verified artifact");
    inspectBackup(firstReport, 3, 60, firstContext);

    const auto secondContext = operationContext(
        SecondBackupId, "p07-backup-collision-second");
    const auto secondReport = take(database->createOnlineBackup(secondContext));
    require(pathFromText(secondReport.backupPath) != firstPath,
            "distinct backup operations reused one artifact path");
    inspectBackup(secondReport, 4, 100, secondContext);

    writer->close(openContext);
    take(database->close(openContext));
}

void testBackupRejectsFinalSidecarDecoysWithoutMain()
{
    constexpr std::string_view OpenId = "70000000-0000-4000-8000-000000000014";
    constexpr std::array<std::string_view, 3U> OperationIds{
        "70000000-0000-4000-8000-000000000015",
        "70000000-0000-4000-8000-000000000016",
        "70000000-0000-4000-8000-000000000017"};
    constexpr std::array<std::wstring_view, 3U> Suffixes{
        L"-wal", L"-shm", L"-journal"};
    ScopedTestDirectory directory{L"backup-final-sidecar-decoys"};
    const auto dependencies = makeCentralDependencies(directory.path());
    const auto openContext = operationContext(
        OpenId, "p07-backup-final-sidecar-open");
    auto database = openCentral(dependencies, openContext);
    auto writer = AnchoredDatabase::open(
        directory.path(), std::wstring{CentralMainBasename},
        WinsqliteOpenMode::ReadWriteExisting, openContext);
    createProbeRows(writer->connection(), openContext);

    for (std::size_t index = 0; index < OperationIds.size(); ++index) {
        const std::filesystem::path finalPath =
            backupPathFor(directory.path(), OperationIds[index]);
        const std::filesystem::path stagePath = sidecarPath(finalPath, L".stage");
        const std::filesystem::path decoyPath =
            sidecarPath(finalPath, Suffixes[index]);
        const std::string decoyBytes =
            "final-sidecar-decoy-" + std::to_string(index);
        writeFixture(decoyPath, decoyBytes);
        const auto decoyIdentity = fileIdentity(decoyPath);

        const auto context = operationContext(
            OperationIds[index], "p07-backup-final-sidecar-collision");
        const auto collision = database->createOnlineBackup(context);
        requireError(
            collision, Domain::ErrorCodes::Conflict,
            "a final-name sidecar decoy was not rejected as a namespace collision");
        require(!collision.error().evidenceId.has_value(),
                "a final-name sidecar collision falsely claimed evidence");
        require(!std::filesystem::exists(finalPath) &&
                    std::filesystem::exists(decoyPath),
                "a final-name sidecar collision created Main or removed the decoy");
        require(PersistenceSupport::readFixture(decoyPath) == decoyBytes &&
                    fileIdentity(decoyPath) == decoyIdentity,
                "a final-name sidecar collision changed the decoy bytes or identity");
        requireClosedCohortAbsent(
            stagePath, "a final-name sidecar collision retained its stage cohort");
        require(!std::filesystem::exists(
                    backupLockPathFor(directory.path(), OperationIds[index], false)) &&
                    !std::filesystem::exists(
                        backupLockPathFor(directory.path(), OperationIds[index], true)),
                "a final-name sidecar collision retained an operation lock");
    }

    writer->close(openContext);
    take(database->close(openContext));
}

void testSimultaneousSameOperationBackupProducers()
{
    constexpr std::string_view OpenId = "70000000-0000-4000-8000-000000000018";
    constexpr std::string_view BackupId = "70000000-0000-4000-8000-000000000019";
    ScopedTestDirectory directory{L"backup-same-operation-race"};
    const auto dependencies = makeCentralDependencies(directory.path());
    const auto openContext = operationContext(OpenId, "p07-backup-race-open");
    auto database = openCentral(dependencies, openContext);
    auto writer = AnchoredDatabase::open(
        directory.path(), std::wstring{CentralMainBasename},
        WinsqliteOpenMode::ReadWriteExisting, openContext);
    createProbeRows(writer->connection(), openContext);

    const auto backupContext = operationContext(
        BackupId, "p07-backup-same-operation-race");
    using BackupResult = Domain::Result<PersistenceWindows::DatabaseBackupReport>;
    std::array<std::optional<BackupResult>, 2U> results;
    std::latch ready{2};
    std::latch release{1};
    std::array<std::jthread, 2U> producers;
    for (std::size_t index = 0; index < producers.size(); ++index) {
        producers[index] = std::jthread{[&, index] {
            ready.count_down();
            release.wait();
            results[index].emplace(database->createOnlineBackup(backupContext));
        }};
    }
    ready.wait();
    release.count_down();
    for (auto& producer : producers) {
        producer.join();
    }

    std::size_t successCount{};
    std::size_t conflictCount{};
    std::optional<PersistenceWindows::DatabaseBackupReport> winner;
    for (auto& result : results) {
        require(result.has_value(),
                "a simultaneous backup producer did not return a result");
        if (result.value()) {
            ++successCount;
            winner.emplace(std::move(result.value()).value());
        } else {
            require(result->error().code == Domain::ErrorCodes::Conflict,
                    "the losing simultaneous backup producer returned the wrong error");
            require(!result->error().evidenceId.has_value(),
                    "the losing simultaneous backup producer falsely claimed evidence");
            ++conflictCount;
        }
    }
    require(successCount == 1U && conflictCount == 1U && winner.has_value(),
            "simultaneous same-operation producers did not yield one winner and one conflict");

    const std::filesystem::path finalPath =
        backupPathFor(directory.path(), BackupId);
    const std::filesystem::path stagePath = sidecarPath(finalPath, L".stage");
    require(pathFromText(winner->backupPath) == finalPath,
            "the winning simultaneous producer reported the wrong artifact path");
    inspectBackup(*winner, 3, 60, backupContext);
    requireClosedCohortAbsent(
        stagePath, "simultaneous same-operation producers retained a stage cohort");
    require(!std::filesystem::exists(sidecarPath(finalPath, L"-wal")) &&
                !std::filesystem::exists(sidecarPath(finalPath, L"-shm")) &&
                !std::filesystem::exists(sidecarPath(finalPath, L"-journal")) &&
                !std::filesystem::exists(
                    backupLockPathFor(directory.path(), BackupId, false)) &&
                !std::filesystem::exists(
                    backupLockPathFor(directory.path(), BackupId, true)),
            "simultaneous same-operation producers retained sidecars or operation locks");

    writer->close(openContext);
    take(database->close(openContext));
}

void testCancelledBackupCleansStageAndUnverifiedFinal()
{
    constexpr std::string_view OpenId = "70000000-0000-4000-8000-000000000021";
    constexpr std::string_view StageCancellationId =
        "70000000-0000-4000-8000-000000000022";
    constexpr std::string_view FinalCancellationId =
        "70000000-0000-4000-8000-000000000023";
    ScopedTestDirectory directory{L"backup-cancel"};
    const auto dependencies = makeCentralDependencies(directory.path());
    const auto openContext = operationContext(OpenId, "p07-backup-cancel-open");
    auto database = openCentral(dependencies, openContext);
    auto writer = AnchoredDatabase::open(
        directory.path(), std::wstring{CentralMainBasename},
        WinsqliteOpenMode::ReadWriteExisting, openContext);
    createLargeWalPayload(writer->connection(), openContext);

    {
        std::stop_source cancellation;
        const auto context = operationContext(
            StageCancellationId, "p07-backup-cancel-stage",
            cancellation.get_token(), 60s);
        const std::filesystem::path finalPath =
            backupPathFor(directory.path(), StageCancellationId);
        const std::filesystem::path stagePath = sidecarPath(finalPath, L".stage");
        std::atomic_bool observed{};
        auto watcher = cancelWhenPathAppears(stagePath, cancellation, observed);
        const auto cancelled = database->createOnlineBackup(context);
        watcher.request_stop();
        watcher.join();

        require(observed.load(std::memory_order_acquire),
                "the stage-cancellation test did not observe a created stage");
        requireError(cancelled, Domain::ErrorCodes::Cancelled,
                     "an interrupted staged backup did not preserve cancellation");
        require(!cancelled.error().evidenceId.has_value(),
                "an interrupted staged backup falsely claimed evidence");
        requireClosedCohortAbsent(
            stagePath, "an interrupted backup retained its unpublished stage cohort");
        requireClosedCohortAbsent(
            finalPath, "an interrupted staged backup published a final artifact");
    }

    {
        std::stop_source cancellation;
        const auto context = operationContext(
            FinalCancellationId, "p07-backup-cancel-final",
            cancellation.get_token(), 60s);
        const std::filesystem::path finalPath =
            backupPathFor(directory.path(), FinalCancellationId);
        const std::filesystem::path stagePath = sidecarPath(finalPath, L".stage");
        std::atomic_bool observed{};
        auto watcher = cancelWhenPathAppears(finalPath, cancellation, observed);
        const auto cancelled = database->createOnlineBackup(context);
        watcher.request_stop();
        watcher.join();

        require(observed.load(std::memory_order_acquire),
                "the final-cancellation test did not observe publication");
        requireError(cancelled, Domain::ErrorCodes::Cancelled,
                     "post-publication verification did not preserve cancellation");
        require(!cancelled.error().evidenceId.has_value(),
                "an unverified published backup falsely claimed evidence");
        requireClosedCohortAbsent(
            stagePath, "post-publication cancellation retained a stage cohort");
        requireClosedCohortAbsent(
            finalPath, "post-publication cancellation retained an unverified artifact");
    }

    writer->close(openContext);
    take(database->close(openContext));
}

void testStaleBackupCleanupUsesExactBoundedNamePolicy()
{
    constexpr std::string_view BackupOperationId =
        "7a000000-0000-4000-8000-000000000001";
    constexpr std::string_view MigrationOperationId =
        "7a000000-0000-4000-8000-000000000002";
    constexpr std::string_view LockOnlyOperationId =
        "7a000000-0000-4000-8000-000000000003";
    constexpr std::string_view ActiveOperationId =
        "7a000000-0000-4000-8000-000000000004";
    constexpr std::string_view SourceBytes = "source-must-remain";
    constexpr std::string_view StageBytes = "stale-stage";
    constexpr std::string_view DecoyBytes = "do-not-delete";
    ScopedTestDirectory directory{L"backup-stale-stage-policy"};
    const std::filesystem::path canonical = canonicalDirectory(directory.path());
    const std::filesystem::path sourcePath = canonical / CentralMainBasename;
    writeFixture(sourcePath, SourceBytes);

    const auto stagePath = [&](const std::wstring_view kind,
                               const std::string_view operationId) {
        return canonical /
            (std::wstring{CentralMainBasename} + L"." + std::wstring{kind} + L"." +
             widenAscii(operationId) + L".sqlite.stage");
    };
    const auto stageLockPath = [&](const std::string_view operationId) {
        return canonical /
            (L"backup-stage-" + widenAscii(operationId) + L".lock");
    };
    const std::filesystem::path backupStage =
        stagePath(L"backup", BackupOperationId);
    const std::filesystem::path migrationStage =
        stagePath(L"pre-migration", MigrationOperationId);
    const std::filesystem::path backupStageLock =
        stageLockPath(BackupOperationId);
    const std::filesystem::path migrationStageLock =
        stageLockPath(MigrationOperationId);
    writeFixture(backupStage, StageBytes);
    writeFixture(migrationStage, StageBytes);
    writeFixture(backupStageLock, StageBytes);
    writeFixture(migrationStageLock, StageBytes);
    const std::filesystem::path lockOnlyStageLock =
        stageLockPath(LockOnlyOperationId);
    writeFixture(lockOnlyStageLock, StageBytes);

    const std::filesystem::path uppercaseUuidDecoy =
        stagePath(L"backup", "7A000000-0000-4000-8000-000000000005");
    const std::filesystem::path malformedUuidDecoy =
        stagePath(L"pre-migration", "not-a-canonical-operation-identifier");
    const std::filesystem::path finalBackupDecoy = canonical /
        (std::wstring{CentralMainBasename} + L".backup." +
         widenAscii(BackupOperationId) + L".sqlite");
    writeFixture(uppercaseUuidDecoy, DecoyBytes);
    writeFixture(malformedUuidDecoy, DecoyBytes);
    writeFixture(finalBackupDecoy, DecoyBytes);

    auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonical.native(),
        CentralMainBasename,
        CentralMigrationLockBasename));
    {
        const auto invalidShare = namespaceLease->openLeafWithShareAccess(
            PersistenceDetail::DatabaseLeafRole::Main,
            PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_DELETE);
        requireError(invalidShare, Domain::ErrorCodes::InvalidRequest,
                     "anchored leaf open accepted delete sharing");
        auto constrainedRead = take(namespaceLease->openLeafWithShareAccess(
            PersistenceDetail::DatabaseLeafRole::Main,
            PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ));
        require(static_cast<bool>(constrainedRead),
                "anchored leaf open rejected read-only sharing");
    }
    const auto context = operationContext(
        "70000000-0000-4000-8000-000000000024",
        "p07-backup-stale-stage-policy");
    const std::size_t cleaned = take(
        PersistenceWindows::DatabaseBackupCoordinator::cleanupStaleBackupStages(
            namespaceLease, context));
    require(cleaned == 3U,
            "stale backup cleanup did not report exact stages plus a lock-only crash artifact");
    requireClosedCohortAbsent(
        backupStage, "stale backup cleanup retained an exact backup stage");
    requireClosedCohortAbsent(
        migrationStage, "stale backup cleanup retained an exact migration stage");
    require(!std::filesystem::exists(backupStageLock) &&
                !std::filesystem::exists(migrationStageLock) &&
                !std::filesystem::exists(lockOnlyStageLock),
            "stale backup cleanup retained an exact stage lock");
    require(PersistenceSupport::readFixture(
                uppercaseUuidDecoy, DecoyBytes.size() + 1U) == DecoyBytes &&
                PersistenceSupport::readFixture(
                    malformedUuidDecoy, DecoyBytes.size() + 1U) == DecoyBytes &&
                PersistenceSupport::readFixture(
                    finalBackupDecoy, DecoyBytes.size() + 1U) == DecoyBytes,
            "stale backup cleanup deleted a noncanonical or published artifact");
    require(PersistenceSupport::readFixture(
                sourcePath, SourceBytes.size() + 1U) == SourceBytes,
            "stale backup cleanup changed the source database");

    const std::filesystem::path activeStage =
        stagePath(L"backup", ActiveOperationId);
    const std::filesystem::path activeStageLock =
        stageLockPath(ActiveOperationId);
    const std::wstring activeFinalName =
        std::wstring{CentralMainBasename} + L".backup." +
        widenAscii(ActiveOperationId) + L".sqlite";
    const std::filesystem::path activeFinal = canonical / activeFinalName;
    auto activeStageNamespace = take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonical.native(),
        activeStage.filename().native(),
        activeStageLock.filename().native()));
    auto activeFinalNamespace = take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonical.native(),
        activeFinal.filename().native(),
        (L"backup-" + widenAscii(ActiveOperationId) + L".lock")));
    auto activeStageOwner = take(activeStageNamespace->acquireMigrationLock(context));
    require(std::filesystem::is_regular_file(activeStageLock),
            "active producer did not create its exact stage lock");
    const auto blockedContext = operationContext(
        "70000000-0000-4000-8000-000000000025",
        "p07-backup-active-stage-cleanup",
        {},
        1s);
    const auto blockedCleanup =
        PersistenceWindows::DatabaseBackupCoordinator::cleanupStaleBackupStages(
            namespaceLease, blockedContext);
    requireError(blockedCleanup, Domain::ErrorCodes::DeadlineExceeded,
                 "cleanup deleted a lock-only stage owned by an active producer");
    require(std::filesystem::is_regular_file(activeStageLock),
            "cleanup removed an active producer's lock-only stage");

    writeFixture(activeStage, StageBytes);
    take(activeStageNamespace->publishClosedMainToWithSourceLock(
        *activeFinalNamespace, activeStageOwner, context));
    const bool removedAfterPublication = take(
        activeStageNamespace->cleanupClosedStageWithLock(
            activeStageOwner, context));
    require(!removedAfterPublication,
            "post-publication stage cleanup reported a moved stage as deleted");
    require(!std::filesystem::exists(activeStage) &&
                !std::filesystem::exists(activeStageLock) &&
                PersistenceSupport::readFixture(
                    activeFinal, StageBytes.size() + 1U) == StageBytes,
            "active producer could not publish safely under its retained stage lock");
}

void testNotADatabaseMainIsQuarantinedWithManifestHash()
{
    constexpr std::string_view OperationId = "70000000-0000-4000-8000-000000000031";
    ScopedTestDirectory directory{L"quarantine-notadb"};
    const std::filesystem::path sourcePath =
        canonicalDirectory(directory.path()) / CentralMainBasename;
    std::string hostileBytes(4096U, 'x');
    hostileBytes.replace(0U, 24U, "not-a-sqlite-database!!");
    writeFixture(sourcePath, hostileBytes);

    const auto dependencies = makeCentralDependencies(directory.path());
    const auto context = operationContext(OperationId, "p07-quarantine-notadb");
    const auto opened = WindowsCentralDatabase::open(
        dependencies.paths, dependencies.diagnostics, dependencies.clock, context);
    require(!opened, "a non-SQLite main file was opened as a valid database");
    verifyQuarantineManifest(
        directory.path(), OperationId, hostileBytes, opened.error());
}

void testCorruptSqliteMainIsQuarantinedWithManifestHash()
{
    constexpr std::string_view SetupId = "70000000-0000-4000-8000-000000000041";
    constexpr std::string_view QuarantineId = "70000000-0000-4000-8000-000000000042";
    ScopedTestDirectory directory{L"quarantine-corrupt"};
    const auto dependencies = makeCentralDependencies(directory.path());
    const auto setupContext = operationContext(SetupId, "p07-quarantine-corrupt-setup");
    auto database = openCentral(dependencies, setupContext);
    take(database->close(setupContext));
    database.reset();

    const std::filesystem::path sourcePath =
        canonicalDirectory(directory.path()) / CentralMainBasename;
    for (const std::wstring_view suffix : {L"-wal", L"-shm", L"-journal"}) {
        std::error_code removedError;
        static_cast<void>(std::filesystem::remove(
            sidecarPath(sourcePath, suffix), removedError));
        require(!removedError,
                "the corrupt-main fixture could not remove a persisted setup sidecar");
    }
    std::string corruptBytes = PersistenceSupport::readFixture(sourcePath);
    require(corruptBytes.size() > 100U,
            "the valid SQLite fixture is too small for deterministic corruption");
    require(static_cast<unsigned char>(corruptBytes[100]) == 0x0dU ||
                static_cast<unsigned char>(corruptBytes[100]) == 0x05U,
            "the valid SQLite fixture has an unexpected page-one B-tree type");
    corruptBytes[100] = '\0';
    writeFixture(sourcePath, corruptBytes);

    const auto quarantineContext = operationContext(
        QuarantineId, "p07-quarantine-corrupt-open");
    const auto opened = WindowsCentralDatabase::open(
        dependencies.paths,
        dependencies.diagnostics,
        dependencies.clock,
        quarantineContext);
    require(!opened, "a malformed SQLite B-tree was opened as a valid database");
    verifyQuarantineManifest(
        directory.path(), QuarantineId, corruptBytes, opened.error());
}

void testFailedQuarantineMakesNoEvidenceClaim()
{
    constexpr std::string_view OperationId = "70000000-0000-4000-8000-000000000051";
    ScopedTestDirectory directory{L"quarantine-cancel"};
    const std::filesystem::path canonical = canonicalDirectory(directory.path());
    const std::filesystem::path sourcePath = canonical / CentralMainBasename;
    const std::string hostileBytes(4096U, 'q');
    writeFixture(sourcePath, hostileBytes);

    auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonical.native(),
        CentralMainBasename,
        CentralMigrationLockBasename));
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto context = operationContext(
        OperationId, "p07-quarantine-cancel",
        cancellation.get_token());
    const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
        namespaceLease,
        Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The deliberately hostile test database is invalid."),
        context);
    requireError(quarantined, Domain::ErrorCodes::Cancelled,
                 "a cancelled quarantine reported preserved evidence");
    require(!quarantined.error().evidenceId.has_value(),
            "a failed quarantine falsely claimed an evidence ID");
    require(PersistenceSupport::readFixture(
                sourcePath, hostileBytes.size() + 1U) == hostileBytes,
            "a failed quarantine removed or changed the source database");

    const std::filesystem::path quarantinePath =
        quarantinePathFor(directory.path(), OperationId);
    requireClosedCohortAbsent(
        quarantinePath, "a failed quarantine published a cohort artifact");
    require(!std::filesystem::exists(
                sidecarPath(quarantinePath, L".manifest.json")),
            "a failed quarantine published a manifest");
}

void testQuarantineCollisionPreservesExistingEvidenceAndSource()
{
    constexpr std::string_view EvidenceOperationId =
        "70000000-0000-4000-8000-000000000061";
    constexpr std::string_view ManifestOperationId =
        "70000000-0000-4000-8000-000000000062";
    constexpr std::string_view SourceBytes = "hostile-source-bytes";
    constexpr std::string_view ExistingEvidenceBytes = "existing-evidence-bytes";
    constexpr std::string_view ExistingManifestBytes = "existing-manifest-bytes";
    constexpr std::string_view SidecarDecoyBytes = "existing-sidecar-bytes";

    const auto runCollision = [&](
        const std::wstring_view directoryName,
        const std::string_view operationId,
        const bool collideWithManifest,
        const std::string_view existingBytes) {
        ScopedTestDirectory directory{directoryName};
        const std::filesystem::path canonical = canonicalDirectory(directory.path());
        const std::filesystem::path sourcePath = canonical / CentralMainBasename;
        const std::filesystem::path quarantinePath =
            quarantinePathFor(directory.path(), operationId);
        const std::filesystem::path manifestPath =
            sidecarPath(quarantinePath, L".manifest.json");
        const std::filesystem::path collisionPath =
            collideWithManifest ? manifestPath : quarantinePath;
        writeFixture(sourcePath, SourceBytes);
        writeFixture(collisionPath, existingBytes);

        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonical.native(),
            CentralMainBasename,
            CentralMigrationLockBasename));
        const auto context = operationContext(
            operationId, "p07-quarantine-collision");
        const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
            namespaceLease,
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The deliberately hostile test database is invalid."),
            context);
        requireError(quarantined, Domain::ErrorCodes::Conflict,
                     "a quarantine collision overwrote operation-scoped evidence");
        require(!quarantined.error().evidenceId.has_value(),
                "a quarantine collision falsely claimed preserved evidence");
        require(PersistenceSupport::readFixture(
                    sourcePath, SourceBytes.size() + 1U) == SourceBytes,
                "a quarantine collision changed or removed the corrupt source");
        require(PersistenceSupport::readFixture(
                    collisionPath, existingBytes.size() + 1U) == existingBytes,
                "a quarantine collision changed the pre-existing artifact");
        const std::filesystem::path absentPath =
            collideWithManifest ? quarantinePath : manifestPath;
        require(!std::filesystem::exists(absentPath),
                "a quarantine collision published a second partial artifact");
    };

    runCollision(
        L"quarantine-evidence-collision",
        EvidenceOperationId,
        false,
        ExistingEvidenceBytes);
    runCollision(
        L"quarantine-manifest-collision",
        ManifestOperationId,
        true,
        ExistingManifestBytes);

    struct SidecarCollision final {
        std::wstring_view directoryName;
        std::string_view operationId;
        std::wstring_view suffix;
    };
    constexpr std::array SidecarCollisions{
        SidecarCollision{
            L"quarantine-wal-collision",
            "70000000-0000-4000-8000-000000000063",
            L"-wal"},
        SidecarCollision{
            L"quarantine-shm-collision",
            "70000000-0000-4000-8000-000000000064",
            L"-shm"},
        SidecarCollision{
            L"quarantine-journal-collision",
            "70000000-0000-4000-8000-000000000065",
            L"-journal"}};
    for (const auto& collision : SidecarCollisions) {
        ScopedTestDirectory directory{collision.directoryName};
        const std::filesystem::path canonical = canonicalDirectory(directory.path());
        const std::filesystem::path sourcePath = canonical / CentralMainBasename;
        const std::filesystem::path quarantinePath =
            quarantinePathFor(directory.path(), collision.operationId);
        const std::filesystem::path manifestPath =
            sidecarPath(quarantinePath, L".manifest.json");
        const std::filesystem::path decoyPath =
            sidecarPath(quarantinePath, collision.suffix);
        writeFixture(sourcePath, SourceBytes);
        writeFixture(decoyPath, SidecarDecoyBytes);

        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonical.native(),
            CentralMainBasename,
            CentralMigrationLockBasename));
        const auto context = operationContext(
            collision.operationId, "p07-quarantine-sidecar-collision");
        const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
            namespaceLease,
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The deliberately hostile test database is invalid."),
            context);
        requireError(quarantined, Domain::ErrorCodes::Conflict,
                     "quarantine overwrote an absent-source sidecar decoy");
        require(!quarantined.error().evidenceId.has_value(),
                "a sidecar collision falsely claimed preserved evidence");
        require(PersistenceSupport::readFixture(
                    sourcePath, SourceBytes.size() + 1U) == SourceBytes &&
                    PersistenceSupport::readFixture(
                        decoyPath, SidecarDecoyBytes.size() + 1U) ==
                        SidecarDecoyBytes,
                "a sidecar collision changed the source or existing decoy");
        require(!std::filesystem::exists(quarantinePath) &&
                    !std::filesystem::exists(manifestPath),
                "a sidecar collision published partial evidence or a manifest");
    }
}

void testQuarantineRejectsAbsentSourceRoleInjectionAfterCapture()
{
    constexpr std::string_view SourceBytes = "captured-hostile-main-bytes";
    constexpr std::string_view DecoyBytes = "late-source-sidecar-decoy";
    struct InjectionCase final {
        std::wstring_view directoryName;
        std::string_view operationId;
        std::wstring_view suffix;
    };
    constexpr std::array InjectionCases{
        InjectionCase{
            L"quarantine-source-wal-injection",
            "70000000-0000-4000-8000-000000000071",
            L"-wal"},
        InjectionCase{
            L"quarantine-source-shm-injection",
            "70000000-0000-4000-8000-000000000072",
            L"-shm"},
        InjectionCase{
            L"quarantine-source-journal-injection",
            "70000000-0000-4000-8000-000000000073",
            L"-journal"}};

    for (const auto& injection : InjectionCases) {
        ScopedTestDirectory directory{injection.directoryName};
        const std::filesystem::path canonical = canonicalDirectory(directory.path());
        const std::filesystem::path sourcePath = canonical / CentralMainBasename;
        const std::filesystem::path decoyPath =
            sidecarPath(sourcePath, injection.suffix);
        writeFixture(sourcePath, SourceBytes);
        const auto sourceIdentity = fileIdentity(sourcePath);

        bool injected{};
        PersistenceDetail::DatabaseFileIdentity decoyIdentity{};
        QuarantineBarrierObserver observer{[&] {
            writeFixture(decoyPath, DecoyBytes);
            decoyIdentity = fileIdentity(decoyPath);
            injected = true;
        }};
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonical.native(),
            CentralMainBasename,
            CentralMigrationLockBasename));
        const auto context = operationContext(
            injection.operationId, "p07-quarantine-source-role-injection");
        const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
            namespaceLease, quarantineReason(), context, &observer);
        observer.rethrowCallbackFailure();

        require(injected && observer.sourceCapturedCount() == 1U &&
                    observer.evidenceCopiedCount() == 0U &&
                    observer.manifestCommittedCount() == 0U,
                "source-sidecar injection did not occur at the capture barrier");
        requireError(quarantined, Domain::ErrorCodes::Conflict,
                     "an absent source cohort role injected after capture was accepted");
        require(!quarantined.error().evidenceId.has_value(),
                "pre-commit source-role injection falsely claimed evidence");
        require(PersistenceSupport::readFixture(
                    sourcePath, SourceBytes.size() + 1U) == SourceBytes &&
                    fileIdentity(sourcePath) == sourceIdentity,
                "source-role injection changed the captured main file");
        require(PersistenceSupport::readFixture(
                    decoyPath, DecoyBytes.size() + 1U) == DecoyBytes &&
                    fileIdentity(decoyPath) == decoyIdentity,
                "source-role injection changed or replaced the injected decoy");
        requireNoQuarantineEvidence(
            directory.path(), injection.operationId,
            "source-role injection published quarantine evidence or a manifest");
    }
}

void testQuarantineRejectsAbsentEvidenceRoleInjectionAfterCopy()
{
    constexpr std::string_view SourceBytes = "copied-hostile-main-bytes";
    constexpr std::string_view DecoyBytes = "late-evidence-sidecar-decoy";
    struct InjectionCase final {
        std::wstring_view directoryName;
        std::string_view operationId;
        std::wstring_view suffix;
    };
    constexpr std::array InjectionCases{
        InjectionCase{
            L"quarantine-evidence-wal-injection",
            "70000000-0000-4000-8000-000000000081",
            L"-wal"},
        InjectionCase{
            L"quarantine-evidence-shm-injection",
            "70000000-0000-4000-8000-000000000082",
            L"-shm"},
        InjectionCase{
            L"quarantine-evidence-journal-injection",
            "70000000-0000-4000-8000-000000000083",
            L"-journal"}};

    for (const auto& injection : InjectionCases) {
        ScopedTestDirectory directory{injection.directoryName};
        const std::filesystem::path canonical = canonicalDirectory(directory.path());
        const std::filesystem::path sourcePath = canonical / CentralMainBasename;
        const std::filesystem::path evidencePath =
            quarantinePathFor(directory.path(), injection.operationId);
        const std::filesystem::path manifestPath =
            sidecarPath(evidencePath, L".manifest.json");
        const std::filesystem::path decoyPath =
            sidecarPath(evidencePath, injection.suffix);
        writeFixture(sourcePath, SourceBytes);
        const auto sourceIdentity = fileIdentity(sourcePath);

        bool injected{};
        PersistenceDetail::DatabaseFileIdentity decoyIdentity{};
        QuarantineBarrierObserver observer{{}, [&] {
            writeFixture(decoyPath, DecoyBytes);
            decoyIdentity = fileIdentity(decoyPath);
            injected = true;
        }};
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonical.native(),
            CentralMainBasename,
            CentralMigrationLockBasename));
        const auto context = operationContext(
            injection.operationId, "p07-quarantine-evidence-role-injection");
        const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
            namespaceLease, quarantineReason(), context, &observer);
        observer.rethrowCallbackFailure();

        require(injected && observer.sourceCapturedCount() == 1U &&
                    observer.evidenceCopiedCount() == 1U &&
                    observer.manifestCommittedCount() == 0U,
                "evidence-sidecar injection did not occur at the copy barrier");
        requireError(quarantined, Domain::ErrorCodes::Conflict,
                     "an absent evidence cohort role injected after copy was accepted");
        require(!quarantined.error().evidenceId.has_value(),
                "pre-commit evidence-role injection falsely claimed evidence");
        require(PersistenceSupport::readFixture(
                    sourcePath, SourceBytes.size() + 1U) == SourceBytes &&
                    fileIdentity(sourcePath) == sourceIdentity,
                "evidence-role injection changed the retained source main file");
        require(!std::filesystem::exists(evidencePath) &&
                    !std::filesystem::exists(manifestPath),
                "evidence-role injection retained owned evidence or published a manifest");
        require(PersistenceSupport::readFixture(
                    decoyPath, DecoyBytes.size() + 1U) == DecoyBytes &&
                    fileIdentity(decoyPath) == decoyIdentity,
                "evidence-role cleanup changed or removed the injected decoy");
    }
}

void testQuarantinePinsSourceContentThroughManifestCommit()
{
    constexpr std::string_view OperationId =
        "70000000-0000-4000-8000-000000000091";
    constexpr std::string_view SourceBytes = "manifest-committed-hostile-main-bytes";
    ScopedTestDirectory directory{L"quarantine-manifest-source-pin"};
    const std::filesystem::path canonical = canonicalDirectory(directory.path());
    const std::filesystem::path sourcePath = canonical / CentralMainBasename;
    const std::filesystem::path evidencePath =
        quarantinePathFor(directory.path(), OperationId);
    const std::filesystem::path manifestPath =
        sidecarPath(evidencePath, L".manifest.json");
    writeFixture(sourcePath, SourceBytes);

    bool overwriteOpened{};
    DWORD overwriteError{ERROR_SUCCESS};
    QuarantineBarrierObserver observer{{}, {}, [&] {
        Infrastructure::Windows::Detail::UniqueHandle overwrite{::CreateFileW(
            sourcePath.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        overwriteOpened = static_cast<bool>(overwrite);
        overwriteError = overwriteOpened ? ERROR_SUCCESS : ::GetLastError();
    }};
    auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonical.native(),
        CentralMainBasename,
        CentralMigrationLockBasename));
    const auto context = operationContext(
        OperationId, "p07-quarantine-manifest-source-pin");
    auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
        namespaceLease, quarantineReason(), context, &observer);
    observer.rethrowCallbackFailure();

    require(observer.sourceCapturedCount() == 1U &&
                observer.evidenceCopiedCount() == 1U &&
                observer.manifestCommittedCount() == 1U,
            "the source-pin test did not reach every quarantine barrier exactly once");
    require(!overwriteOpened && overwriteError == ERROR_SHARING_VIOLATION,
            "a same-file truncate open was not blocked by the retained source handle");
    require(static_cast<bool>(quarantined),
            "source pinning prevented successful exact source removal");
    const auto report = take(std::move(quarantined));
    require(report.evidenceId ==
                "p07-quarantine:" + std::string{OperationId} &&
                report.preservedFileCount == 1U &&
                pathFromText(report.cohortMainPath) == evidencePath &&
                pathFromText(report.manifestPath) == manifestPath,
            "successful quarantine reported the wrong committed evidence");
    require(!std::filesystem::exists(sourcePath),
            "the retained source handle did not permit exact removal after commit");
    requireCommittedMainEvidence(directory.path(), OperationId, SourceBytes);
}

void testQuarantineRetainsCommittedEvidenceWhenHardLinkBlocksRemoval()
{
    constexpr std::string_view OperationId =
        "70000000-0000-4000-8000-000000000092";
    constexpr std::string_view SourceBytes = "hard-linked-hostile-main-bytes";
    ScopedTestDirectory directory{L"quarantine-hardlink-postcommit"};
    const std::filesystem::path canonical = canonicalDirectory(directory.path());
    const std::filesystem::path sourcePath = canonical / CentralMainBasename;
    const std::filesystem::path hardLinkPath = canonical / L"source-hardlink-decoy";
    writeFixture(sourcePath, SourceBytes);
    const auto sourceIdentity = fileIdentity(sourcePath);

    bool hardLinkCreated{};
    DWORD hardLinkError{ERROR_SUCCESS};
    QuarantineBarrierObserver observer{{}, {}, [&] {
        hardLinkCreated = ::CreateHardLinkW(
            hardLinkPath.c_str(), sourcePath.c_str(), nullptr) != FALSE;
        hardLinkError = hardLinkCreated ? ERROR_SUCCESS : ::GetLastError();
    }};
    auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonical.native(),
        CentralMainBasename,
        CentralMigrationLockBasename));
    const auto context = operationContext(
        OperationId, "p07-quarantine-hardlink-postcommit");
    const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
        namespaceLease, quarantineReason(), context, &observer);
    observer.rethrowCallbackFailure();

    require(observer.sourceCapturedCount() == 1U &&
                observer.evidenceCopiedCount() == 1U &&
                observer.manifestCommittedCount() == 1U,
            "the hard-link test did not reach every quarantine barrier exactly once");
    require(hardLinkCreated,
            "the post-commit hard-link fixture could not be created; native error " +
                std::to_string(hardLinkError));
    requireError(quarantined, Domain::ErrorCodes::IntegrityFailure,
                 "a post-commit hard link did not fail exact source removal");
    require(quarantined.error().evidenceId.has_value() &&
                quarantined.error().evidenceId.value() ==
                    "p07-quarantine:" + std::string{OperationId},
            "post-commit exact-removal failure omitted its committed evidence ID");
    require(PersistenceSupport::readFixture(
                sourcePath, SourceBytes.size() + 1U) == SourceBytes &&
                PersistenceSupport::readFixture(
                    hardLinkPath, SourceBytes.size() + 1U) == SourceBytes &&
                fileIdentity(sourcePath) == sourceIdentity &&
                fileIdentity(hardLinkPath) == sourceIdentity,
            "exact-removal failure changed or detached the source hard link");
    requireCommittedMainEvidence(directory.path(), OperationId, SourceBytes);
}

void testQuarantineRejectsOversizeSparseCohortsBeforeCopy()
{
    constexpr std::string_view FileOperationId =
        "70000000-0000-4000-8000-000000000093";
    constexpr std::string_view CohortOperationId =
        "70000000-0000-4000-8000-000000000094";

    {
        ScopedTestDirectory directory{L"quarantine-sparse-file-cap"};
        const std::filesystem::path canonical = canonicalDirectory(directory.path());
        const std::filesystem::path sourcePath = canonical / CentralMainBasename;
        constexpr std::uint64_t LogicalBytes =
            PersistenceWindows::DatabaseQuarantine::MaximumFileBytes + 1ULL;
        createSparseFixture(sourcePath, LogicalBytes);
        const auto sourceIdentity = fileIdentity(sourcePath);

        QuarantineBarrierObserver observer;
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonical.native(),
            CentralMainBasename,
            CentralMigrationLockBasename));
        const auto context = operationContext(
            FileOperationId, "p07-quarantine-sparse-file-cap");
        const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
            namespaceLease, quarantineReason(), context, &observer);
        observer.rethrowCallbackFailure();

        requireError(quarantined, Domain::ErrorCodes::PayloadTooLarge,
                     "a sparse per-file cap+1 source entered quarantine copying");
        require(!quarantined.error().evidenceId.has_value(),
                "a preflight size rejection falsely claimed evidence");
        require(observer.sourceCapturedCount() == 0U &&
                    observer.evidenceCopiedCount() == 0U &&
                    observer.manifestCommittedCount() == 0U,
                "per-file size preflight crossed a quarantine copy barrier");
        require(std::filesystem::file_size(sourcePath) == LogicalBytes &&
                    fileIdentity(sourcePath) == sourceIdentity,
                "per-file size preflight changed or removed the sparse source");
        requireNoQuarantineEvidence(
            directory.path(), FileOperationId,
            "per-file size preflight created evidence or a manifest");
    }

    {
        ScopedTestDirectory directory{L"quarantine-sparse-cohort-cap"};
        const std::filesystem::path canonical = canonicalDirectory(directory.path());
        const std::filesystem::path sourcePath = canonical / CentralMainBasename;
        const std::filesystem::path walPath = sidecarPath(sourcePath, L"-wal");
        const std::filesystem::path sharedMemoryPath =
            sidecarPath(sourcePath, L"-shm");
        constexpr std::uint64_t FileBytes =
            PersistenceWindows::DatabaseQuarantine::MaximumFileBytes;
        createSparseFixture(sourcePath, FileBytes);
        createSparseFixture(walPath, FileBytes);
        createSparseFixture(sharedMemoryPath, 1ULL);
        const auto sourceIdentity = fileIdentity(sourcePath);
        const auto walIdentity = fileIdentity(walPath);
        const auto sharedMemoryIdentity = fileIdentity(sharedMemoryPath);

        QuarantineBarrierObserver observer;
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonical.native(),
            CentralMainBasename,
            CentralMigrationLockBasename));
        const auto context = operationContext(
            CohortOperationId, "p07-quarantine-sparse-cohort-cap");
        const auto quarantined = PersistenceWindows::DatabaseQuarantine::preserve(
            namespaceLease, quarantineReason(), context, &observer);
        observer.rethrowCallbackFailure();

        requireError(quarantined, Domain::ErrorCodes::PayloadTooLarge,
                     "a sparse aggregate 8-GiB+1 cohort entered quarantine copying");
        require(!quarantined.error().evidenceId.has_value(),
                "an aggregate preflight size rejection falsely claimed evidence");
        require(observer.sourceCapturedCount() == 0U &&
                    observer.evidenceCopiedCount() == 0U &&
                    observer.manifestCommittedCount() == 0U,
                "aggregate size preflight crossed a quarantine copy barrier");
        require(std::filesystem::file_size(sourcePath) == FileBytes &&
                    std::filesystem::file_size(walPath) == FileBytes &&
                    std::filesystem::file_size(sharedMemoryPath) == 1ULL &&
                    fileIdentity(sourcePath) == sourceIdentity &&
                    fileIdentity(walPath) == walIdentity &&
                    fileIdentity(sharedMemoryPath) == sharedMemoryIdentity,
                "aggregate size preflight changed or removed a sparse source role");
        requireNoQuarantineEvidence(
            directory.path(), CohortOperationId,
            "aggregate size preflight created evidence or a manifest");
    }
}

} // namespace

void registerBackupIntegrityTests(TestRegistry& tests)
{
    addTest(tests, "persistence.backup.wal-online",
            testFacadeBackupIncludesWalResidentRows);
    addTest(tests, "persistence.backup.operation-collision",
            testOperationScopedBackupCollision);
    addTest(tests, "persistence.backup.final-sidecar-collision",
            testBackupRejectsFinalSidecarDecoysWithoutMain);
    addTest(tests, "persistence.backup.same-operation-race",
            testSimultaneousSameOperationBackupProducers);
    addTest(tests, "persistence.backup.cancelled-cleanup",
            testCancelledBackupCleansStageAndUnverifiedFinal);
    addTest(tests, "persistence.backup.stale-stage-policy",
            testStaleBackupCleanupUsesExactBoundedNamePolicy);
    addTest(tests, "persistence.quarantine.notadb-manifest",
            testNotADatabaseMainIsQuarantinedWithManifestHash);
    addTest(tests, "persistence.quarantine.corrupt-manifest",
            testCorruptSqliteMainIsQuarantinedWithManifestHash);
    addTest(tests, "persistence.quarantine.no-false-claim",
            testFailedQuarantineMakesNoEvidenceClaim);
    addTest(tests, "persistence.quarantine.collision-no-overwrite",
            testQuarantineCollisionPreservesExistingEvidenceAndSource);
    addTest(tests, "persistence.quarantine.source-role-injection",
            testQuarantineRejectsAbsentSourceRoleInjectionAfterCapture);
    addTest(tests, "persistence.quarantine.evidence-role-injection",
            testQuarantineRejectsAbsentEvidenceRoleInjectionAfterCopy);
    addTest(tests, "persistence.quarantine.manifest-source-pin",
            testQuarantinePinsSourceContentThroughManifestCommit);
    addTest(tests, "persistence.quarantine.hardlink-postcommit",
            testQuarantineRetainsCommittedEvidenceWhenHardLinkBlocksRemoval);
    addTest(tests, "persistence.quarantine.size-preflight",
            testQuarantineRejectsOversizeSparseCohortsBeforeCopy);
}

} // namespace ForgeConductor::Tests
