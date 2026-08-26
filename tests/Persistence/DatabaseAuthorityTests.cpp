#include "PersistenceTestSupport.h"

#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"

#include <winsqlite/winsqlite3.h>

#include <Windows.h>
#include <winioctl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;

using namespace std::chrono_literals;
using PersistenceSupport::ScopedTestDirectory;

[[nodiscard]] std::filesystem::path canonicalDirectory(
    const std::filesystem::path& directory)
{
    std::error_code error;
    auto canonical = std::filesystem::canonical(directory, error);
    require(!error, "the authority test directory could not be canonicalized");
    return canonical;
}

[[nodiscard]] Domain::OperationContext authorityContext(
    const std::string_view correlation,
    const std::stop_token cancellation = {},
    const std::chrono::seconds timeout = 30s)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("81000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + timeout,
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> makeNamespace(
    const std::filesystem::path& directory,
    const std::wstring_view mainBasename,
    const std::wstring_view lockBasename)
{
    return take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonicalDirectory(directory).native(), mainBasename, lockBasename));
}

void writeFile(
    const std::filesystem::path& path,
    const std::string_view bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    require(output.good(), "an authority fixture file could not be opened");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    require(output.good(), "an authority fixture file could not be written completely");
}

[[nodiscard]] PersistenceDetail::DatabaseFileIdentity writeLeaf(
    const std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease>& namespaceLease,
    const PersistenceDetail::DatabaseLeafRole role,
    const std::string_view bytes)
{
    require(bytes.size() <= static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()),
            "an authority leaf fixture exceeds the native write bound");
    auto leaf = take(namespaceLease->openLeaf(
        role,
        PersistenceDetail::DatabaseLeafDisposition::CreateNew,
        GENERIC_WRITE | FILE_READ_ATTRIBUTES));
    DWORD written{};
    require(::WriteFile(
                leaf.nativeHandle(), bytes.data(), static_cast<DWORD>(bytes.size()),
                &written, nullptr) != FALSE &&
                written == bytes.size(),
            "an anchored authority leaf could not be written completely");
    require(::FlushFileBuffers(leaf.nativeHandle()) != FALSE,
            "an anchored authority leaf could not be flushed");
    return leaf.identity();
}

class OpenAnchoredDatabase final {
public:
    [[nodiscard]] static std::unique_ptr<OpenAnchoredDatabase> create(
        const std::filesystem::path& directory,
        const std::wstring_view mainBasename,
        const std::wstring_view lockBasename,
        const PersistenceDetail::WinsqliteOpenMode openMode,
        const Domain::OperationContext& context)
    {
        auto namespaceLease = makeNamespace(directory, mainBasename, lockBasename);
        auto vfs = take(PersistenceDetail::AnchoredSqliteVfs::create(namespaceLease));
        auto connection = take(PersistenceDetail::WinsqliteConnection::open(
            namespaceLease->canonicalMainDatabasePath(),
            PersistenceDetail::WinsqliteConnectionOptions{
                std::string{vfs->vfsName()}, openMode,
                PersistenceDetail::WinsqliteSynchronousMode::Full,
                PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
                namespaceLease},
            context));
        return std::unique_ptr<OpenAnchoredDatabase>{new OpenAnchoredDatabase{
            std::move(namespaceLease), std::move(vfs), std::move(connection)}};
    }

    ~OpenAnchoredDatabase() noexcept
    {
        connection_.reset();
        if (vfs_) {
            static_cast<void>(vfs_->close());
        }
    }

    OpenAnchoredDatabase(const OpenAnchoredDatabase&) = delete;
    OpenAnchoredDatabase& operator=(const OpenAnchoredDatabase&) = delete;

    [[nodiscard]] PersistenceDetail::WinsqliteConnection& connection() noexcept
    {
        return connection_.value();
    }

    [[nodiscard]] PersistenceDetail::DatabaseNamespaceLease& namespaceLease() noexcept
    {
        return *namespaceLease_;
    }

    [[nodiscard]] std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease>
    namespaceAuthority() const noexcept
    {
        return namespaceLease_;
    }

    [[nodiscard]] PersistenceDetail::AnchoredSqliteVfs& vfs() noexcept
    {
        return *vfs_;
    }

    void close(const Domain::OperationContext& context)
    {
        require(connection_.has_value(), "the authority database was already closed");
        take(connection_->close(context));
        connection_.reset();
        require(vfs_->openFileCount() == 0U && namespaceLease_->openVfsFileCount() == 0U,
                "closing the authority database retained a VFS file owner");
        take(vfs_->close());
        require(!vfs_->isRegistered(), "the closed authority VFS remained registered");
    }

private:
    OpenAnchoredDatabase(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs,
        PersistenceDetail::WinsqliteConnection connection) noexcept
        : namespaceLease_{std::move(namespaceLease)},
          vfs_{std::move(vfs)},
          connection_{std::move(connection)}
    {
    }

    std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs_;
    std::optional<PersistenceDetail::WinsqliteConnection> connection_;
};

[[nodiscard]] std::int64_t queryInteger(
    PersistenceDetail::WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == PersistenceDetail::WinsqliteStepResult::Row,
            "the authority query returned no row");
    const std::int64_t value = take(statement.columnInt64(0));
    require(take(statement.step()) == PersistenceDetail::WinsqliteStepResult::Done,
            "the authority query returned multiple rows");
    return value;
}

class CaseSensitiveDirectoryGuard final {
public:
    explicit CaseSensitiveDirectoryGuard(const std::filesystem::path& path)
        : handle_{::CreateFileW(
              path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
              nullptr, OPEN_EXISTING,
              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)}
    {
        require(static_cast<bool>(handle_),
                "the case-sensitive test directory could not be opened");
        FILE_CASE_SENSITIVE_INFO information{};
        information.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
        require(::SetFileInformationByHandle(
                    handle_.get(), FileCaseSensitiveInfo,
                    &information, sizeof(information)) != FALSE,
                "the Windows test volume cannot enable per-directory case sensitivity");
        enabled_ = true;
    }

    ~CaseSensitiveDirectoryGuard() noexcept
    {
        if (enabled_) {
            FILE_CASE_SENSITIVE_INFO information{};
            static_cast<void>(::SetFileInformationByHandle(
                handle_.get(), FileCaseSensitiveInfo,
                &information, sizeof(information)));
        }
    }

    CaseSensitiveDirectoryGuard(const CaseSensitiveDirectoryGuard&) = delete;
    CaseSensitiveDirectoryGuard& operator=(const CaseSensitiveDirectoryGuard&) = delete;

private:
    InfrastructureDetail::UniqueHandle handle_;
    bool enabled_{};
};

struct MountPointReparseData final {
    DWORD tag{};
    WORD dataLength{};
    WORD reserved{};
    WORD substituteNameOffset{};
    WORD substituteNameLength{};
    WORD printNameOffset{};
    WORD printNameLength{};
    wchar_t pathBuffer[1]{};
};

void makeJunction(
    const std::filesystem::path& junction,
    const std::filesystem::path& target)
{
    require(std::filesystem::create_directory(junction),
            "the junction placeholder directory could not be created");
    InfrastructureDetail::UniqueHandle handle{::CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(handle),
            "the junction placeholder could not be opened as a reparse leaf");

    const std::wstring substitute = L"\\??\\" + canonicalDirectory(target).native();
    const std::wstring printName = canonicalDirectory(target).native();
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) +
                                  printBytes + sizeof(wchar_t);
    const std::size_t totalBytes = offsetof(MountPointReparseData, pathBuffer) + pathBytes;
    require(totalBytes <= static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) &&
                pathBytes + 8U <= static_cast<std::size_t>((std::numeric_limits<WORD>::max)()),
            "the junction reparse payload exceeds native bounds");

    const std::size_t storageWords =
        (totalBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t);
    std::vector<std::uint64_t> storage(storageWords);
    auto* const data = reinterpret_cast<MountPointReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(pathBytes + 8U);
    data->substituteNameLength = static_cast<WORD>(substituteBytes);
    data->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    data->printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
    data->pathBuffer[substitute.size()] = L'\0';
    std::memcpy(
        reinterpret_cast<std::byte*>(data->pathBuffer) + data->printNameOffset,
        printName.data(), printBytes);
    data->pathBuffer[(data->printNameOffset / sizeof(wchar_t)) + printName.size()] = L'\0';

    DWORD returned{};
    require(::DeviceIoControl(
                handle.get(), FSCTL_SET_REPARSE_POINT,
                data, static_cast<DWORD>(totalBytes),
                nullptr, 0U, &returned, nullptr) != FALSE,
            "the test junction reparse point could not be created");
}

void testCanonicalAuthorityAndFixedNames()
{
    ScopedTestDirectory directory{L"authority-paths"};
    const std::filesystem::path root = canonicalDirectory(directory.path());
    const std::filesystem::path nested = root / L"missing" / L"database";

    auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
        nested.native(), L"authority.sqlite", L"authority.sqlite.migration.lock"));
    require(std::filesystem::is_directory(nested),
            "the anchored namespace did not create its missing directory chain");
    require(namespaceLease->canonicalDirectory() == nested.native() &&
                namespaceLease->canonicalMainDatabasePath() ==
                    (nested / L"authority.sqlite").native(),
            "the anchored namespace changed its canonical directory or main path");
    require(namespaceLease->leafName(PersistenceDetail::DatabaseLeafRole::Wal) ==
                L"authority.sqlite-wal" &&
                namespaceLease->leafName(PersistenceDetail::DatabaseLeafRole::SharedMemory) ==
                    L"authority.sqlite-shm" &&
                namespaceLease->leafName(PersistenceDetail::DatabaseLeafRole::Journal) ==
                    L"authority.sqlite-journal",
            "the anchored namespace did not derive exact role-consistent sidecar names");

    const auto relative = PersistenceDetail::DatabaseNamespaceLease::create(
        L"relative\\database", L"db.sqlite", L"db.lock");
    requireError(relative, Domain::ErrorCodes::InvalidRequest,
                 "a relative database authority was accepted");
    const auto traversal = PersistenceDetail::DatabaseNamespaceLease::create(
        root.native() + L"\\..\\escape", L"db.sqlite", L"db.lock");
    requireError(traversal, Domain::ErrorCodes::InvalidRequest,
                 "a traversing database authority was accepted");
    const auto ads = PersistenceDetail::DatabaseNamespaceLease::create(
        root.native(), L"db.sqlite:hostile", L"db.lock");
    requireError(ads, Domain::ErrorCodes::InvalidRequest,
                 "an alternate-data-stream database basename was accepted");
    const auto collision = PersistenceDetail::DatabaseNamespaceLease::create(
        root.native(), L"db.sqlite", L"db.sqlite-wal");
    requireError(collision, Domain::ErrorCodes::InvalidRequest,
                 "a migration-lock and WAL role collision was accepted");
    const auto classifiedAds = namespaceLease->classifyCanonicalPath(
        namespaceLease->canonicalMainDatabasePath() + L":hostile");
    requireError(classifiedAds, Domain::ErrorCodes::PathOutsideAuthority,
                 "an ADS-qualified SQLite path was classified inside authority");

    static_cast<void>(writeLeaf(
        namespaceLease, PersistenceDetail::DatabaseLeafRole::Main, "create-once"));
    const auto duplicateCreate = namespaceLease->openLeaf(
        PersistenceDetail::DatabaseLeafRole::Main,
        PersistenceDetail::DatabaseLeafDisposition::CreateNew,
        GENERIC_READ | GENERIC_WRITE);
    requireError(duplicateCreate, Domain::ErrorCodes::Conflict,
                 "an atomic database-leaf create collision was not mapped to conflict");
    require(duplicateCreate.error().retryable,
            "an atomic database-leaf create collision was not marked retryable");
}

void testCaseSensitiveAndReparseDirectoriesRejected()
{
    ScopedTestDirectory directory{L"authority-directories"};
    const std::filesystem::path root = canonicalDirectory(directory.path());

    const std::filesystem::path caseSensitive = root / L"case-sensitive";
    require(std::filesystem::create_directory(caseSensitive),
            "the case-sensitive authority fixture could not be created");
    {
        CaseSensitiveDirectoryGuard guard{caseSensitive};
        const auto rejected = PersistenceDetail::DatabaseNamespaceLease::create(
            caseSensitive.native(), L"case.sqlite", L"case.lock");
        requireError(rejected, Domain::ErrorCodes::PathOutsideAuthority,
                     "a case-sensitive database directory was accepted");
    }

    const std::filesystem::path target = root / L"junction-target";
    const std::filesystem::path junction = root / L"junction";
    require(std::filesystem::create_directory(target),
            "the junction target directory could not be created");
    makeJunction(junction, target);
    const auto rejectedJunction = PersistenceDetail::DatabaseNamespaceLease::create(
        junction.native(), L"junction.sqlite", L"junction.lock");
    requireError(rejectedJunction, Domain::ErrorCodes::PathOutsideAuthority,
                 "a reparse-point database directory was accepted");
    require(::RemoveDirectoryW(junction.c_str()) != FALSE,
            "the authority test junction could not be removed");
}

void testHardLinkedLeafRejected()
{
    ScopedTestDirectory directory{L"authority-hardlink"};
    const std::filesystem::path root = canonicalDirectory(directory.path());
    const std::filesystem::path mainPath = root / L"hard.sqlite";
    const std::filesystem::path aliasPath = root / L"hard-alias.sqlite";
    writeFile(mainPath, "hard-link-probe");
    require(::CreateHardLinkW(aliasPath.c_str(), mainPath.c_str(), nullptr) != FALSE,
            "the hard-link authority fixture could not be created");

    const auto namespaceLease = PersistenceDetail::DatabaseNamespaceLease::create(
        root.native(), L"hard.sqlite", L"hard.sqlite.lock");
    requireError(namespaceLease, Domain::ErrorCodes::IntegrityFailure,
                 "a multiply linked database leaf was accepted into authority");
}

void testRememberedCohortIdentitiesRejectReplacement()
{
    ScopedTestDirectory directory{L"authority-cohort-identities"};
    constexpr std::array<PersistenceDetail::DatabaseLeafRole, 3U> Roles{
        PersistenceDetail::DatabaseLeafRole::Main,
        PersistenceDetail::DatabaseLeafRole::Wal,
        PersistenceDetail::DatabaseLeafRole::SharedMemory};
    for (std::size_t index = 0U; index < Roles.size(); ++index) {
        const std::wstring mainName = L"identity-" + std::to_wstring(index) + L".sqlite";
        const std::wstring lockName = mainName + L".lock";
        auto namespaceLease = makeNamespace(directory.path(), mainName, lockName);
        const auto originalIdentity = writeLeaf(namespaceLease, Roles[index], "original");
        const std::filesystem::path originalPath = namespaceLease->canonicalPath(Roles[index]);
        const std::filesystem::path displacedPath =
            canonicalDirectory(directory.path()) /
            (L"displaced-" + std::to_wstring(index) + L".bin");
        require(::MoveFileExW(
                    originalPath.c_str(), displacedPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
                "a closed cohort identity fixture could not be displaced");
        writeFile(originalPath, "replacement");
        const auto changed = namespaceLease->revalidateCohort();
        requireError(changed, Domain::ErrorCodes::Conflict,
                     "a replaced main, WAL, or SHM identity was accepted");

        require(::DeleteFileW(originalPath.c_str()) != FALSE,
                "the replacement cohort fixture could not be removed");
        require(::MoveFileExW(
                    displacedPath.c_str(), originalPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
                "the original cohort fixture could not be restored");
        auto restored = take(namespaceLease->openLeaf(
            Roles[index], PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
            GENERIC_READ));
        require(restored.identity() == originalIdentity,
                "restoring the remembered cohort identity was not accepted");
    }

    auto deletionNamespace = makeNamespace(
        directory.path(), L"identity-delete.sqlite", L"identity-delete.sqlite.lock");
    const auto deletedIdentity = writeLeaf(
        deletionNamespace, PersistenceDetail::DatabaseLeafRole::Wal, "old-wal");
    take(deletionNamespace->deleteClosedLeaf(
        PersistenceDetail::DatabaseLeafRole::Wal, deletedIdentity));
    take(deletionNamespace->revalidateCohort());
    const auto replacementIdentity = writeLeaf(
        deletionNamespace, PersistenceDetail::DatabaseLeafRole::Wal, "new-wal");
    take(deletionNamespace->revalidateCohort());
    auto replacement = take(deletionNamespace->openLeaf(
        PersistenceDetail::DatabaseLeafRole::Wal,
        PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
        GENERIC_READ));
    require(replacement.identity() == replacementIdentity,
            "an exact closed-leaf deletion did not admit and remember a new WAL identity");
}

void testPinnedMainAndSidecarsRejectReplacement()
{
    ScopedTestDirectory directory{L"authority-replacement"};
    auto namespaceLease = makeNamespace(
        directory.path(), L"pinned.sqlite", L"pinned.sqlite.lock");
    constexpr std::array<PersistenceDetail::DatabaseLeafRole, 4U> Roles{
        PersistenceDetail::DatabaseLeafRole::Main,
        PersistenceDetail::DatabaseLeafRole::Wal,
        PersistenceDetail::DatabaseLeafRole::SharedMemory,
        PersistenceDetail::DatabaseLeafRole::Journal};
    std::vector<PersistenceDetail::DatabaseLeafLease> pinned;
    pinned.reserve(Roles.size());

    for (std::size_t index = 0U; index < Roles.size(); ++index) {
        auto leaf = take(namespaceLease->openLeaf(
            Roles[index], PersistenceDetail::DatabaseLeafDisposition::CreateNew,
            GENERIC_READ | GENERIC_WRITE));
        constexpr std::string_view Original = "original";
        DWORD written{};
        require(::WriteFile(
                    leaf.nativeHandle(), Original.data(),
                    static_cast<DWORD>(Original.size()), &written, nullptr) != FALSE &&
                    written == Original.size(),
                "a pinned authority leaf could not be initialized");
        pinned.push_back(std::move(leaf));

        const std::filesystem::path attacker =
            canonicalDirectory(directory.path()) /
            (L"attacker-" + std::to_wstring(index) + L".tmp");
        writeFile(attacker, "replacement");
        const BOOL replaced = ::MoveFileExW(
            attacker.c_str(), namespaceLease->canonicalPath(Roles[index]).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        const DWORD error = replaced == FALSE ? ::GetLastError() : ERROR_SUCCESS;
        require(replaced == FALSE &&
                    (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED),
                "an open anchored main or sidecar leaf was replaced by pathname");
        require(PersistenceSupport::readFixture(
                    namespaceLease->canonicalPath(Roles[index]), 32U) == Original,
                "a failed replacement changed a pinned database leaf");
    }
}

void testVfsRejectsAmbientAndUnrecognizedOpens()
{
    ScopedTestDirectory directory{L"authority-vfs"};
    const auto context = authorityContext("p07-authority-vfs");
    auto database = OpenAnchoredDatabase::create(
        directory.path(), L"vfs.sqlite", L"vfs.sqlite.lock",
        PersistenceDetail::WinsqliteOpenMode::ReadWriteCreate, context);
    take(database->connection().execute(
        "CREATE TABLE authority_probe(value INTEGER NOT NULL);"
        "INSERT INTO authority_probe(value) VALUES(7);",
        context));

    const std::size_t ownedFiles = database->vfs().openFileCount();
    require(ownedFiles > 0U &&
                ownedFiles == database->namespaceLease().openVfsFileCount(),
            "the open VFS and namespace disagree about exact file ownership");
    sqlite3_vfs* const vfs = ::sqlite3_vfs_find(
        std::string{database->vfs().vfsName()}.c_str());
    require(vfs != nullptr && vfs->xOpen != nullptr,
            "the explicitly owned authority VFS is not registered");
    const std::size_t words =
        (static_cast<std::size_t>(vfs->szOsFile) + sizeof(std::max_align_t) - 1U) /
        sizeof(std::max_align_t);
    std::vector<std::max_align_t> storage(words);
    auto* const file = reinterpret_cast<sqlite3_file*>(storage.data());
    std::memset(file, 0, static_cast<std::size_t>(vfs->szOsFile));
    int outputFlags{};
    const int tempOpen = vfs->xOpen(
        vfs, nullptr, file,
        SQLITE_OPEN_TEMP_DB | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_DELETEONCLOSE,
        &outputFlags);
    require(tempOpen == SQLITE_CANTOPEN && file->pMethods == nullptr,
            "the anchored VFS accepted an unnamed disk-backed temporary file");

    std::memset(file, 0, static_cast<std::size_t>(vfs->szOsFile));
    const int uriOpen = vfs->xOpen(
        vfs,
        database->namespaceLease().canonicalUtf8Path(
            PersistenceDetail::DatabaseLeafRole::Main).c_str(),
        file,
        SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI,
        &outputFlags);
    require(uriOpen == SQLITE_CANTOPEN && file->pMethods == nullptr,
            "the anchored VFS accepted a URI-mode main database open");
    require(vfs->xDlOpen != nullptr &&
                vfs->xDlOpen(vfs, "hostile-extension.dll") == nullptr,
            "the anchored VFS opened a dynamic SQLite extension");

    const auto attach = database->connection().execute(
        "ATTACH DATABASE ':memory:' AS hostile;", context);
    requireError(attach, Domain::ErrorCodes::Unauthorized,
                 "the database authorizer accepted ATTACH");
    const auto loadExtension = database->connection().execute(
        "SELECT load_extension('hostile-extension.dll');", context);
    requireError(loadExtension, Domain::ErrorCodes::Unauthorized,
                 "the database authorizer accepted dynamic extension loading");
    require(queryInteger(
                database->connection(), "SELECT SUM(value) FROM authority_probe;", context) == 7,
            "denied ambient opens changed the primary database");
    require(database->vfs().openFileCount() == ownedFiles &&
                database->namespaceLease().openVfsFileCount() == ownedFiles,
            "denied VFS opens changed exact file ownership");

    const auto emptyVfs = PersistenceDetail::WinsqliteConnection::open(
        database->namespaceLease().canonicalMainDatabasePath(),
        PersistenceDetail::WinsqliteConnectionOptions{
            {}, PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
            PersistenceDetail::WinsqliteSynchronousMode::Full,
            PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
            database->namespaceAuthority()},
        context);
    requireError(emptyVfs, Domain::ErrorCodes::InvalidRequest,
                 "the SQLite kernel fell back from an empty VFS name");
    sqlite3_vfs* const defaultVfs = ::sqlite3_vfs_find(nullptr);
    require(defaultVfs != nullptr && defaultVfs->zName != nullptr,
            "the inbox default VFS is unavailable for fallback characterization");
    const auto defaultFallback = PersistenceDetail::WinsqliteConnection::open(
        database->namespaceLease().canonicalMainDatabasePath(),
        PersistenceDetail::WinsqliteConnectionOptions{
            defaultVfs->zName, PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
            PersistenceDetail::WinsqliteSynchronousMode::Full,
            PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
            database->namespaceAuthority()},
        context);
    requireError(defaultFallback, Domain::ErrorCodes::InvalidRequest,
                 "the SQLite kernel accepted its default VFS as an authority fallback");
    const auto missingVfs = PersistenceDetail::WinsqliteConnection::open(
        database->namespaceLease().canonicalMainDatabasePath(),
        PersistenceDetail::WinsqliteConnectionOptions{
            "forge-unregistered-vfs", PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
            PersistenceDetail::WinsqliteSynchronousMode::Full,
            PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
            database->namespaceAuthority()},
        context);
    requireError(missingVfs, Domain::ErrorCodes::InvalidRequest,
                 "the SQLite kernel accepted an unregistered VFS name");
    database->close(context);
}

void testHandleRelativePublication()
{
    ScopedTestDirectory directory{L"authority-publication"};
    const auto context = authorityContext("p07-authority-publication");
    auto source = makeNamespace(
        directory.path(), L"stage.sqlite", L"stage.sqlite.lock");
    auto destination = makeNamespace(
        directory.path(), L"published.sqlite", L"published.sqlite.lock");
    const auto sourceIdentity = writeLeaf(
        source, PersistenceDetail::DatabaseLeafRole::Main, "verified-stage");
    take(source->publishClosedMainTo(*destination, context));
    require(!std::filesystem::exists(source->canonicalMainDatabasePath()) &&
                PersistenceSupport::readFixture(
                    destination->canonicalMainDatabasePath(), 64U) == "verified-stage",
            "handle-relative publication did not atomically replace the final main leaf");
    auto published = take(destination->openLeaf(
        PersistenceDetail::DatabaseLeafRole::Main,
        PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
        GENERIC_READ));
    require(published.identity() == sourceIdentity,
            "handle-relative publication changed the staged file identity");

    auto conflictingSource = makeNamespace(
        directory.path(), L"conflicting-stage.sqlite", L"conflicting-stage.sqlite.lock");
    auto conflictingDestination = makeNamespace(
        directory.path(), L"conflicting-final.sqlite", L"conflicting-final.sqlite.lock");
    static_cast<void>(writeLeaf(
        conflictingSource, PersistenceDetail::DatabaseLeafRole::Main, "new-stage"));
    static_cast<void>(writeLeaf(
        conflictingDestination, PersistenceDetail::DatabaseLeafRole::Main,
        "existing-final"));
    const auto conflict = conflictingSource->publishClosedMainTo(
        *conflictingDestination, context);
    requireError(conflict, Domain::ErrorCodes::Conflict,
                 "create-only publication overwrote an existing destination");
    require(PersistenceSupport::readFixture(
                conflictingSource->canonicalMainDatabasePath(), 64U) == "new-stage" &&
                PersistenceSupport::readFixture(
                    conflictingDestination->canonicalMainDatabasePath(), 64U) ==
                    "existing-final",
            "a refused create-only publication changed source or destination bytes");

    auto busySource = makeNamespace(
        directory.path(), L"busy-stage.sqlite", L"busy-stage.sqlite.lock");
    auto busyDestination = makeNamespace(
        directory.path(), L"busy-final.sqlite", L"busy-final.sqlite.lock");
    auto pinned = take(busySource->openLeaf(
        PersistenceDetail::DatabaseLeafRole::Main,
        PersistenceDetail::DatabaseLeafDisposition::CreateNew,
        GENERIC_READ | GENERIC_WRITE));
    const auto refused = busySource->publishClosedMainTo(*busyDestination, context);
    requireError(refused, Domain::ErrorCodes::DatabaseBusy,
                 "publication replaced a main leaf that still had an open owner");
    require(static_cast<bool>(pinned) &&
                std::filesystem::exists(busySource->canonicalMainDatabasePath()) &&
                !std::filesystem::exists(busyDestination->canonicalMainDatabasePath()),
            "failed publication changed its source or destination namespace");
}

void testPublicationRemovesFinalLockWithOpenWaiter()
{
    ScopedTestDirectory directory{L"authority-publication-lock-waiter"};
    const auto context = authorityContext("p07-authority-publication-lock-waiter");
    auto source = makeNamespace(
        directory.path(), L"waiter-stage.sqlite", L"waiter-stage.sqlite.lock");
    auto destination = makeNamespace(
        directory.path(), L"waiter-final.sqlite", L"waiter-final.sqlite.lock");
    static_cast<void>(writeLeaf(
        source, PersistenceDetail::DatabaseLeafRole::Main, "verified-waiter-stage"));
    auto sourceLock = take(source->acquireMigrationLock(context));

    // This handle models the pinned pathname handle opened by a lock waiter.
    // Migration-lock observers must share deletion so the producer can unlink
    // the ephemeral final lock while retaining byte-zero ownership.
    auto waiterPathHandle = take(destination->openLeaf(
        PersistenceDetail::DatabaseLeafRole::MigrationLock,
        PersistenceDetail::DatabaseLeafDisposition::CreateNew,
        FILE_READ_ATTRIBUTES));
    take(source->publishClosedMainToWithSourceLock(
        *destination, sourceLock, context));

    require(!std::filesystem::exists(source->canonicalMainDatabasePath()) &&
                PersistenceSupport::readFixture(
                    destination->canonicalMainDatabasePath(), 64U) ==
                    "verified-waiter-stage",
            "a final-lock waiter changed or removed a valid publication");
    require(!std::filesystem::exists(destination->canonicalPath(
                PersistenceDetail::DatabaseLeafRole::MigrationLock)),
            "publication retained its ephemeral final lock while a waiter handle was open");
    require(static_cast<bool>(waiterPathHandle),
            "publication invalidated the waiter's owned native handle");

    static_cast<void>(take(source->cleanupClosedStageWithLock(sourceLock, context)));
    require(!std::filesystem::exists(source->canonicalPath(
                PersistenceDetail::DatabaseLeafRole::MigrationLock)),
            "publication waiter regression retained its exact stage lock");
}

void testBoundedHandleRelativeLeafEnumeration()
{
    ScopedTestDirectory directory{L"authority-enumeration"};
    auto namespaceLease = makeNamespace(
        directory.path(), L"enumeration.sqlite", L"enumeration.sqlite.lock");
    const auto context = authorityContext("p07-authority-enumeration");
    const std::filesystem::path root = canonicalDirectory(directory.path());
    writeFile(root / L"backup-stage-b.sqlite.stage", "b");
    writeFile(root / L"backup-stage-a.sqlite.stage", "a");
    writeFile(root / L"backup-stage-a.sqlite", "not-a-stage");

    const auto matches = take(namespaceLease->enumerateMatchingLeafNames(
        L"backup-stage-", L".sqlite.stage", 4U, context));
    require(matches == std::vector<std::wstring>{
                           L"backup-stage-a.sqlite.stage",
                           L"backup-stage-b.sqlite.stage"},
            "bounded handle-relative enumeration was incomplete or nondeterministic");
    const auto bounded = namespaceLease->enumerateMatchingLeafNames(
        L"backup-stage-", L".sqlite.stage", 1U, context);
    requireError(bounded, Domain::ErrorCodes::LimitExceeded,
                 "handle-relative leaf enumeration exceeded its caller result bound");
    const auto wildcard = namespaceLease->enumerateMatchingLeafNames(
        L"backup-*", L".stage", 4U, context);
    requireError(wildcard, Domain::ErrorCodes::InvalidRequest,
                 "handle-relative leaf enumeration accepted a wildcard fragment");
}

} // namespace

void registerDatabaseAuthorityTests(TestRegistry& tests)
{
    addTest(tests, "persistence.authority.canonical-paths",
            testCanonicalAuthorityAndFixedNames);
    addTest(tests, "persistence.authority.directory-flags",
            testCaseSensitiveAndReparseDirectoriesRejected);
    addTest(tests, "persistence.authority.hard-link", testHardLinkedLeafRejected);
    addTest(tests, "persistence.authority.cohort-identities",
            testRememberedCohortIdentitiesRejectReplacement);
    addTest(tests, "persistence.authority.replacement-pins",
            testPinnedMainAndSidecarsRejectReplacement);
    addTest(tests, "persistence.authority.vfs-denials",
            testVfsRejectsAmbientAndUnrecognizedOpens);
    addTest(tests, "persistence.authority.publication", testHandleRelativePublication);
    addTest(tests, "persistence.authority.publication-lock-waiter",
            testPublicationRemovesFinalLockWithOpenWaiter);
    addTest(tests, "persistence.authority.bounded-enumeration",
            testBoundedHandleRelativeLeafEnumeration);
}

} // namespace ForgeConductor::Tests
