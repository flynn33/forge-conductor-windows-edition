#include "PersistenceTestSupport.h"

#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectDatabase.h"
#include "Persistence/Windows/DatabaseBackupCoordinator.h"
#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"
#include "Persistence/Windows/Detail/WinsqliteTransaction.h"
#include "Persistence/Windows/Migrations/ProjectMigrations.h"
#include "Persistence/Windows/Migrations/SchemaMigrator.h"

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;
namespace PersistenceMigrations = ForgeConductor::Persistence::Windows::Migrations;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace TestFakes = ForgeConductor::Tests::Fakes;

using PersistenceSupport::FixedClock;
using PersistenceSupport::ScopedTestDirectory;
using PersistenceSupport::activeContext;
using PersistenceDetail::WinsqliteConnection;
using PersistenceDetail::WinsqliteOpenMode;
using PersistenceDetail::WinsqliteStepResult;
using PersistenceDetail::WinsqliteSynchronousMode;

constexpr std::string_view AppliedAt = "2026-01-02T03:04:05Z";
constexpr std::string_view Version2AppliedAt = "2025-12-01T02:03:04Z";
constexpr std::string_view ProjectLedger =
    "1:P001:9c9d3e635b2c75088da271ca773f4cea18aca862c40d77ad13f3d9ea183a514f|"
    "2:P002:69fa2b2c63f84903badba580bc0692804a9569a8b2a50b55b919e0c9881b0c08|"
    "3:P003:6a84a8c63e67ed4760ff589cb7ba96bec3ce25140c8e85c849b28b421f25acb9";
constexpr std::string_view ProjectIdText =
    "11111111-1111-4111-8111-111111111111";
constexpr DWORD ChildWaitMilliseconds = 15'000U;

const std::vector<std::string> ProjectTables{
    "artifacts",
    "continuity_handoffs",
    "event_journal",
    "handoffs",
    "maintenance_state",
    "memory_links",
    "memory_record_tags",
    "memory_records",
    "memory_tags",
    "project_active_sessions",
    "project_aliases",
    "project_metadata",
    "rollover_operations",
    "rollover_transitions",
    "schema_migrations",
    "sessions"};

const std::vector<std::string> ProjectFtsTables{
    "artifacts",
    "continuity_handoffs",
    "event_journal",
    "handoffs",
    "maintenance_state",
    "memory_links",
    "memory_record_tags",
    "memory_records",
    "memory_records_fts",
    "memory_records_fts_config",
    "memory_records_fts_content",
    "memory_records_fts_data",
    "memory_records_fts_docsize",
    "memory_records_fts_idx",
    "memory_tags",
    "project_active_sessions",
    "project_aliases",
    "project_metadata",
    "rollover_operations",
    "rollover_transitions",
    "schema_migrations",
    "sessions"};

const std::vector<std::string> ProjectIndexes{
    "idx_event_journal_event_id",
    "idx_event_journal_idempotency",
    "idx_memory_project_kind",
    "idx_memory_project_recent",
    "idx_memory_project_session",
    "idx_rollover_active_project",
    "idx_rollover_project_updated",
    "memory_records_project_recent"};

const std::vector<std::string> ProjectFtsTriggers{
    "memory_fts_delete",
    "memory_fts_insert",
    "memory_fts_update"};

[[nodiscard]] std::wstring quoteProcessArgument(const std::wstring_view argument)
{
    std::wstring quoted;
    quoted.reserve(argument.size() + 2U);
    quoted.push_back(L'"');
    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append((backslashes * 2U) + 1U, L'\\');
            quoted.push_back(character);
            backslashes = 0U;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::filesystem::path processFixturePath()
{
    std::array<wchar_t, 32U * 1024U> modulePath{};
    const DWORD length = ::GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    require(length != 0U && length < modulePath.size(),
            "the persistence test executable path could not be resolved");
    std::filesystem::path fixture =
        std::filesystem::path{std::wstring_view{modulePath.data(), length}}.parent_path() /
        L"ForgeConductor.Persistence.ProcessFixture.exe";
    std::error_code error;
    fixture = std::filesystem::canonical(fixture, error);
    require(!error && std::filesystem::is_regular_file(fixture),
            "the persistence process fixture is unavailable beside the test executable");
    return fixture;
}

[[nodiscard]] std::wstring makeProcessCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine = quoteProcessArgument(executable.native());
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteProcessArgument(argument);
    }
    require(commandLine.size() < 32U * 1024U,
            "the project generation-writer command line exceeds the Windows bound");
    return commandLine;
}

class GenerationWriterProcess final {
public:
    [[nodiscard]] static GenerationWriterProcess launch(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments)
    {
        InfrastructureDetail::UniqueHandle job{::CreateJobObjectW(nullptr, nullptr)};
        require(static_cast<bool>(job),
                "the project generation-writer job object could not be created");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(::SetInformationJobObject(
                    job.get(), JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits)) != FALSE,
                "the project generation-writer kill-on-close policy could not be applied");

        std::wstring commandLine = makeProcessCommandLine(executable, arguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        const BOOL created = ::CreateProcessW(
            executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
            &startup, &information);
        const DWORD createError = created != FALSE ? ERROR_SUCCESS : ::GetLastError();
        require(created != FALSE,
                "the project generation writer could not be launched; native error " +
                    std::to_string(createError));

        InfrastructureDetail::UniqueHandle process{information.hProcess};
        InfrastructureDetail::UniqueHandle thread{information.hThread};
        if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
            const DWORD assignError = ::GetLastError();
            static_cast<void>(::TerminateProcess(process.get(), 91U));
            static_cast<void>(::WaitForSingleObject(process.get(), 5'000U));
            require(false,
                    "the project generation writer could not enter its job; native error " +
                        std::to_string(assignError));
        }
        if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            const DWORD resumeError = ::GetLastError();
            static_cast<void>(::TerminateProcess(process.get(), 92U));
            static_cast<void>(::WaitForSingleObject(process.get(), 5'000U));
            require(false,
                    "the project generation writer could not resume; native error " +
                        std::to_string(resumeError));
        }
        thread.reset();
        return GenerationWriterProcess{std::move(job), std::move(process)};
    }

    ~GenerationWriterProcess() noexcept
    {
        if (!process_) {
            return;
        }
        DWORD code{};
        if (::GetExitCodeProcess(process_.get(), &code) != FALSE &&
            code == STILL_ACTIVE) {
            static_cast<void>(::TerminateProcess(process_.get(), 93U));
            static_cast<void>(::WaitForSingleObject(process_.get(), 5'000U));
        }
    }

    GenerationWriterProcess(const GenerationWriterProcess&) = delete;
    GenerationWriterProcess& operator=(const GenerationWriterProcess&) = delete;
    GenerationWriterProcess(GenerationWriterProcess&&) noexcept = default;
    GenerationWriterProcess& operator=(GenerationWriterProcess&&) noexcept = default;

    [[nodiscard]] HANDLE nativeHandle() const noexcept { return process_.get(); }

    [[nodiscard]] DWORD exitCode() const
    {
        DWORD code{};
        require(::GetExitCodeProcess(process_.get(), &code) != FALSE &&
                    code != STILL_ACTIVE,
                "the project generation writer has no completed exit code");
        return code;
    }

    [[nodiscard]] DWORD wait(const DWORD timeoutMilliseconds) const
    {
        const DWORD waitResult =
            ::WaitForSingleObject(process_.get(), timeoutMilliseconds);
        require(waitResult == WAIT_OBJECT_0,
                waitResult == WAIT_TIMEOUT
                    ? "the project generation writer exceeded its shutdown deadline"
                    : "waiting for the project generation writer failed");
        return exitCode();
    }

private:
    GenerationWriterProcess(
        InfrastructureDetail::UniqueHandle job,
        InfrastructureDetail::UniqueHandle process) noexcept
        : job_{std::move(job)}, process_{std::move(process)}
    {
    }

    InfrastructureDetail::UniqueHandle job_;
    InfrastructureDetail::UniqueHandle process_;
};

struct GenerationEvent final {
    std::wstring name;
    InfrastructureDetail::UniqueHandle handle;
};

[[nodiscard]] GenerationEvent createGenerationEvent(const std::wstring_view label)
{
    LARGE_INTEGER counter{};
    require(::QueryPerformanceCounter(&counter) != FALSE,
            "a project generation event nonce could not be created");
    std::wstring name =
        L"Local\\ForgeConductor-P07-ProjectGeneration-" + std::wstring{label} + L"-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetCurrentThreadId()) + L"-" +
        std::to_wstring(static_cast<unsigned long long>(counter.QuadPart));
    InfrastructureDetail::UniqueHandle handle{
        ::CreateEventW(nullptr, TRUE, FALSE, name.c_str())};
    const DWORD createError = ::GetLastError();
    require(static_cast<bool>(handle) && createError != ERROR_ALREADY_EXISTS,
            "a unique project generation event could not be created");
    return GenerationEvent{std::move(name), std::move(handle)};
}

void signalGenerationEvent(
    const GenerationEvent& event,
    const std::string_view failure)
{
    require(::SetEvent(event.handle.get()) != FALSE, failure);
}

void waitForGenerationEventOrProcess(
    const GenerationEvent& event,
    const GenerationWriterProcess& process,
    const std::string_view earlyExit,
    const std::string_view timeout)
{
    const std::array<HANDLE, 2U> handles{event.handle.get(), process.nativeHandle()};
    const DWORD waitResult = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE,
        ChildWaitMilliseconds);
    if (waitResult == WAIT_OBJECT_0 + 1U) {
        require(false,
                std::string{earlyExit} + "; exit code " +
                    std::to_string(process.exitCode()));
    }
    require(waitResult == WAIT_OBJECT_0,
            waitResult == WAIT_TIMEOUT ? timeout
                                       : "waiting for a project generation event failed");
}

[[nodiscard]] std::vector<std::wstring> generationWriterArguments(
    const std::filesystem::path& directory,
    const GenerationEvent& start,
    const GenerationEvent& attempted,
    const GenerationEvent& done,
    const std::uint32_t marker)
{
    return {
        L"--project-generation-writer",
        std::filesystem::canonical(directory).native(),
        L"memory.sqlite",
        L"memory.sqlite.migration.lock",
        start.name,
        attempted.name,
        done.name,
        std::to_wstring(marker),
        std::to_wstring(ChildWaitMilliseconds)};
}

class KernelEnvironment final {
public:
    [[nodiscard]] static std::unique_ptr<KernelEnvironment> create(
        const std::filesystem::path& directory,
        const std::wstring_view basename)
    {
        const std::wstring canonicalDirectory =
            std::filesystem::canonical(directory).native();
        const std::wstring migrationLock = std::wstring{basename} + L".migration.lock";
        auto namespaceLease = take(PersistenceDetail::DatabaseNamespaceLease::create(
            canonicalDirectory, basename, migrationLock));
        auto vfs = take(PersistenceDetail::AnchoredSqliteVfs::create(namespaceLease));
        return std::unique_ptr<KernelEnvironment>{new KernelEnvironment{
            std::move(namespaceLease), std::move(vfs)}};
    }

    ~KernelEnvironment() noexcept
    {
        if (vfs_ != nullptr) {
            static_cast<void>(vfs_->close());
        }
    }

    KernelEnvironment(const KernelEnvironment&) = delete;
    KernelEnvironment& operator=(const KernelEnvironment&) = delete;
    KernelEnvironment(KernelEnvironment&&) = delete;
    KernelEnvironment& operator=(KernelEnvironment&&) = delete;

    [[nodiscard]] PersistenceDetail::WinsqliteConnectionOptions options(
        const WinsqliteOpenMode openMode) const
    {
        return PersistenceDetail::WinsqliteConnectionOptions{
            std::string{vfs_->vfsName()}, openMode, WinsqliteSynchronousMode::Normal,
            PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
            namespaceLease_};
    }

    [[nodiscard]] const std::wstring& databasePath() const noexcept
    {
        return namespaceLease_->canonicalMainDatabasePath();
    }

    [[nodiscard]] const std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease>&
    namespaceLease() const noexcept
    {
        return namespaceLease_;
    }

    [[nodiscard]] std::size_t openFileCount() const noexcept
    {
        return vfs_->openFileCount();
    }

private:
    KernelEnvironment(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs) noexcept
        : namespaceLease_{std::move(namespaceLease)}, vfs_{std::move(vfs)}
    {
    }

    std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs_;
};

struct ProjectDependencies final {
    explicit ProjectDependencies(const std::filesystem::path& directory)
    {
        const auto now = std::chrono::steady_clock::now();
        paths = std::make_shared<TestFakes::RecordingApplicationPathsFake>();
        paths->setNow(now);
        paths->projectRootResult.set(Domain::Result<Domain::PathText>::success(
            PersistenceSupport::pathText(directory)));
        diagnostics = std::make_shared<TestFakes::RuntimeDiagnosticsFake>(now);
        const auto day = std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::January / 2};
        const Domain::UtcTimePoint utc{
            day.time_since_epoch() + std::chrono::hours{3} +
            std::chrono::minutes{4} + std::chrono::seconds{5}};
        clock = std::make_shared<FixedClock>(utc, now);
    }

    std::shared_ptr<TestFakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<TestFakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<FixedClock> clock;
};

[[nodiscard]] const Domain::ProjectId& projectId()
{
    static const Domain::ProjectId Value = parse<Domain::ProjectId>(ProjectIdText);
    return Value;
}

[[nodiscard]] WinsqliteConnection openDatabase(
    const KernelEnvironment& environment,
    const WinsqliteOpenMode openMode,
    const Domain::OperationContext& context)
{
    return take(WinsqliteConnection::open(
        environment.databasePath(), environment.options(openMode), context));
}

[[nodiscard]] std::string fixtureSql(const std::filesystem::path& path)
{
    std::string sql = PersistenceSupport::readFixture(path);
    constexpr std::string_view ForeignKeysPragma = "PRAGMA foreign_keys = ON;";
    const std::size_t position = sql.find(ForeignKeysPragma);
    require(position != std::string::npos,
            "project fixture no longer contains its documented foreign-key pragma");
    sql.erase(position, ForeignKeysPragma.size());
    return sql;
}

void createFixture(
    const std::filesystem::path& directory,
    const std::filesystem::path& fixture)
{
    const auto context = activeContext("p07-project-create-fixture");
    auto environment = KernelEnvironment::create(directory, L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(connection.execute(fixtureSql(fixture), context));
    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "project fixture creation leaked a VFS file owner");
}

void createProjectVersion2Fixture(
    const std::filesystem::path& directory,
    const std::filesystem::path& version1Fixture)
{
    createFixture(directory, version1Fixture);
    const auto context = activeContext("p11-project-create-v2-fixture");
    auto environment = KernelEnvironment::create(directory, L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    const auto steps = PersistenceMigrations::projectMigrationSteps();
    require(steps.size() == 3U && steps[0].version == 1 && steps[1].version == 2 &&
                steps[2].version == 3,
            "project migration manifest is not the expected contiguous P001-P003 sequence");
    take(connection.execute(steps[1].sql, context));
    for (std::size_t index{}; index < 2U; ++index) {
        auto statement = take(connection.prepare(
            "INSERT INTO schema_migrations(version, identifier, applied_at, content_sha256) "
            "VALUES(?1, ?2, ?3, ?4);",
            context));
        take(statement.bindInt64(1, steps[index].version));
        take(statement.bindText(2, steps[index].identifier));
        take(statement.bindText(3, Version2AppliedAt));
        take(statement.bindText(4, steps[index].contentSha256));
        require(take(statement.step()) == WinsqliteStepResult::Done,
                "project v2 fixture ledger insertion returned a row");
    }
    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "project v2 fixture creation leaked a VFS file owner");
}

void createFixtureWithExactReplacement(
    const std::filesystem::path& directory,
    const std::filesystem::path& fixture,
    const std::string_view expected,
    const std::string_view replacement)
{
    std::string sql = fixtureSql(fixture);
    const std::size_t position = sql.find(expected);
    require(position != std::string::npos &&
                sql.find(expected, position + expected.size()) == std::string::npos,
            "the adversarial schema replacement is no longer exact");
    sql.replace(position, expected.size(), replacement);

    const auto context = activeContext("p07-project-create-adversarial-fixture");
    auto environment = KernelEnvironment::create(directory, L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(connection.execute(sql, context));
    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "adversarial project fixture creation leaked a VFS file owner");
}

[[nodiscard]] std::int64_t queryInteger(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "project integer query returned no row");
    const std::int64_t value = take(statement.columnInt64(0));
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "project integer query returned more than one row");
    return value;
}

[[nodiscard]] std::string queryText(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context,
    const std::size_t maximumBytes = 64U * 1024U)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "project text query returned no row");
    auto value = take(statement.columnText(0, maximumBytes));
    require(value.has_value(), "project text query unexpectedly returned null");
    std::string copied = std::move(value).value();
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "project text query returned more than one row");
    return copied;
}

[[nodiscard]] std::int64_t countGenerationTag(
    WinsqliteConnection& connection,
    const std::uint32_t marker,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(
        "SELECT COUNT(*) FROM memory_tags WHERE id = ?1 AND name = ?2;", context));
    take(statement.bindInt64(1, static_cast<std::int64_t>(marker)));
    const std::string tag = "process-generation-" + std::to_string(marker);
    take(statement.bindText(2, tag));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "project generation-tag query returned no row");
    const std::int64_t count = take(statement.columnInt64(0));
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "project generation-tag query returned multiple rows");
    return count;
}

void requireNoMigrationBackupArtifact(
    const std::filesystem::path& directory,
    const std::string_view failure)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        require(!error, failure);
        const std::wstring name = entry.path().filename().native();
        require(!name.starts_with(L"memory.sqlite.pre-migration.") &&
                    !name.starts_with(L"backup-"),
                failure);
    }
    require(!error, failure);
}

void requireGenerationWriterBlocked(
    const GenerationEvent& done,
    const GenerationWriterProcess& process)
{
    const std::array<HANDLE, 2U> handles{done.handle.get(), process.nativeHandle()};
    const DWORD waitResult = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE, 500U);
    if (waitResult == WAIT_OBJECT_0 + 1U) {
        require(false,
                "the post-admission generation writer exited before migration commit; exit code " +
                    std::to_string(process.exitCode()));
    }
    require(waitResult == WAIT_TIMEOUT,
            waitResult == WAIT_OBJECT_0
                ? "the post-admission generation writer committed before migration"
                : "checking the blocked post-admission generation writer failed");
}

void requireBackupArtifactMutationBlocked(
    const std::filesystem::path& backupPath,
    const std::string_view owner)
{
    ::SetLastError(ERROR_SUCCESS);
    require(::DeleteFileW(backupPath.c_str()) == FALSE,
            std::string{owner} + " did not retain a no-delete backup lease");
    const DWORD deleteError = ::GetLastError();
    require(deleteError == ERROR_SHARING_VIOLATION ||
                deleteError == ERROR_ACCESS_DENIED,
            std::string{owner} + " backup deletion failed for an unexpected reason");

    ::SetLastError(ERROR_SUCCESS);
    Infrastructure::Windows::Detail::UniqueHandle writer{::CreateFileW(
        backupPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    require(!writer,
            std::string{owner} + " allowed in-place writes to the verified backup");
    const DWORD writeError = ::GetLastError();
    require(writeError == ERROR_SHARING_VIOLATION || writeError == ERROR_ACCESS_DENIED,
            std::string{owner} + " backup write failed for an unexpected reason");
}

[[nodiscard]] std::string columnSignatureSql(const std::string_view table)
{
    return "SELECT group_concat(cid || ':' || name || ':' || type || ':' || "
           "[notnull] || ':' || COALESCE(dflt_value, '<null>') || ':' || pk || "
           "':' || hidden, '|') FROM (SELECT * FROM pragma_table_xinfo('" +
        std::string{table} + "') ORDER BY cid);";
}

[[nodiscard]] std::string indexKeySql(const std::string_view index)
{
    return "SELECT group_concat(name || ':' || [desc], '|') FROM (SELECT * FROM "
           "pragma_index_xinfo('" +
        std::string{index} + "') WHERE key = 1 ORDER BY seqno);";
}

[[nodiscard]] std::unique_ptr<PersistenceWindows::WindowsProjectDatabase> openProject(
    ProjectDependencies& dependencies,
    const bool enableFts5,
    const Domain::OperationContext& context)
{
    return take(PersistenceWindows::WindowsProjectDatabase::open(
        projectId(), dependencies.paths, dependencies.diagnostics, dependencies.clock,
        PersistenceWindows::WindowsProjectDatabaseOptions{enableFts5}, context));
}

void requireCurrentSnapshot(
    PersistenceWindows::WindowsProjectDatabase& database,
    const bool ftsEnabled,
    const Domain::OperationContext& context)
{
    const auto snapshot = take(database.schemaSnapshot(context));
    require(database.projectId() == projectId(),
            "project facade changed its project identity");
    require(snapshot.kind == PersistenceWindows::DatabaseStoreKind::Project,
            "project facade reported the wrong database kind");
    require(snapshot.physicalVersion == 3 &&
                snapshot.sourceCompatibilityVersion == 1,
            "project facade reported the wrong schema versions");
    require(snapshot.fts5Enabled == ftsEnabled,
            "project facade reported the wrong FTS state");
    require(snapshot.tables == (ftsEnabled ? ProjectFtsTables : ProjectTables),
            "project facade did not expose the exact target table set");
    require(snapshot.indexes == ProjectIndexes,
            "project facade did not expose the exact target index set");
    require(snapshot.triggers ==
                (ftsEnabled ? ProjectFtsTriggers : std::vector<std::string>{}),
            "project facade did not expose the exact trigger set");
    take(database.quickCheck(context));
}

void requireCurrentLedgerAndConstraints(
    const std::filesystem::path& directory)
{
    const auto context = activeContext("p07-project-inspection");
    auto environment = KernelEnvironment::create(directory, L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);

    require(queryInteger(connection, "PRAGMA main.user_version;", context) == 3,
            "project target does not advertise user_version 3");
    require(queryText(
                connection,
                "SELECT group_concat(version || ':' || identifier || ':' || "
                "content_sha256, '|') FROM (SELECT * FROM schema_migrations "
                "ORDER BY version);",
                context) == ProjectLedger,
            "project migration ledger identifiers or checksums changed");
    require(queryInteger(connection,
                         "SELECT COUNT(DISTINCT applied_at) FROM schema_migrations;",
                         context) == 1 &&
                queryText(connection,
                          "SELECT MIN(applied_at) FROM schema_migrations;",
                          context) == AppliedAt,
            "project migration ledger did not retain the injected applied-at text");

    require(queryText(connection, columnSignatureSql("schema_migrations"), context) ==
                "0:version:INTEGER:0:<null>:1:0|1:identifier:TEXT:1:<null>:0:0|"
                "2:applied_at:TEXT:1:<null>:0:0|3:content_sha256:TEXT:1:<null>:0:0",
            "project migration-ledger columns changed");
    require(queryText(connection, columnSignatureSql("project_metadata"), context) ==
                "0:project_id:TEXT:0:<null>:1:0|1:display_name:TEXT:1:<null>:0:0|"
                "2:repository_identity:TEXT:0:<null>:0:0|"
                "3:schema_version:INTEGER:1:<null>:0:0|"
                "4:created_at:TEXT:1:<null>:0:0|5:updated_at:TEXT:1:<null>:0:0",
            "project metadata columns changed");
    require(queryText(connection, columnSignatureSql("memory_records"), context) ==
                "0:id:TEXT:0:<null>:1:0|1:project_id:TEXT:1:<null>:0:0|"
                "2:version:INTEGER:1:<null>:0:0|3:kind:TEXT:1:<null>:0:0|"
                "4:title:TEXT:1:<null>:0:0|5:summary:TEXT:1:<null>:0:0|"
                "6:body:TEXT:0:<null>:0:0|7:importance:REAL:1:<null>:0:0|"
                "8:confidence:REAL:1:<null>:0:0|9:source_kind:TEXT:1:<null>:0:0|"
                "10:source_reference:TEXT:0:<null>:0:0|"
                "11:session_id:TEXT:0:<null>:0:0|12:created_at:TEXT:1:<null>:0:0|"
                "13:updated_at:TEXT:1:<null>:0:0|"
                "14:last_accessed_at:TEXT:1:<null>:0:0|"
                "15:expires_at:TEXT:0:<null>:0:0|"
                "16:content_hash:TEXT:1:<null>:0:0|"
                "17:is_tombstone:INTEGER:1:0:0:0|"
                "18:schema_version:INTEGER:1:<null>:0:0|"
                "19:idempotency_key:TEXT:0:<null>:0:0|"
                "20:source:TEXT:0:<null>:0:0",
            "project memory-record columns changed");
    require(queryText(connection, columnSignatureSql("memory_links"), context) ==
                "0:project_id:TEXT:1:<null>:0:0|1:source_id:TEXT:1:<null>:1:0|"
                "2:target_id:TEXT:1:<null>:2:0|3:relation:TEXT:1:<null>:3:0|"
                "4:created_at:TEXT:1:<null>:0:0|"
                "5:destination_id:TEXT:0:<null>:0:0",
            "project memory-link columns changed");
    require(queryText(connection, columnSignatureSql("event_journal"), context) ==
                "0:id:INTEGER:0:<null>:1:0|1:project_id:TEXT:1:<null>:0:0|"
                "2:record_id:TEXT:0:<null>:0:0|3:action:TEXT:1:<null>:0:0|"
                "4:detail:TEXT:0:<null>:0:0|5:created_at:TEXT:1:<null>:0:0|"
                "6:event_id:TEXT:0:<null>:0:0|7:event_type:TEXT:0:<null>:0:0|"
                "8:entity_id:TEXT:0:<null>:0:0|9:payload_json:TEXT:0:<null>:0:0|"
                "10:idempotency_key:TEXT:0:<null>:0:0",
            "project event-journal columns changed");
    require(queryText(connection, columnSignatureSql("rollover_operations"), context) ==
                "0:operation_id:TEXT:0:<null>:1:0|1:project_id:TEXT:1:<null>:0:0|"
                "2:predecessor_session_id:TEXT:1:<null>:0:0|"
                "3:successor_session_id:TEXT:0:<null>:0:0|"
                "4:handoff_id:TEXT:1:<null>:0:0|5:state:TEXT:1:<null>:0:0|"
                "6:attempt:INTEGER:1:<null>:0:0|7:adapter_id:TEXT:1:<null>:0:0|"
                "8:idempotency_key:TEXT:1:<null>:0:0|"
                "9:acknowledged_session_id:TEXT:0:<null>:0:0|"
                "10:acknowledged_handoff_id:TEXT:0:<null>:0:0|"
                "11:created_at:TEXT:1:<null>:0:0|12:updated_at:TEXT:1:<null>:0:0|"
                "13:last_error:TEXT:0:<null>:0:0|14:retry_at:TEXT:0:<null>:0:0|"
                "15:state_checksum:TEXT:1:<null>:0:0|"
                "16:retry_resume_state:TEXT:0:<null>:0:0",
            "project rollover-operation columns changed");

    require(queryText(connection,
                      "SELECT [unique] || ':' || origin || ':' || partial FROM "
                      "pragma_index_list('rollover_operations') WHERE "
                      "name = 'idx_rollover_active_project';",
                      context) == "1:c:1" &&
                queryText(connection,
                          indexKeySql("idx_rollover_active_project"), context) ==
                    "project_id:0",
            "project active-rollover index lost its unique partial property");
    require(queryInteger(connection,
                         "SELECT instr(sql, 'WHERE state NOT IN "
                         "(''predecessorSealed'',''completed'',''cancelled'')') "
                         "FROM sqlite_schema WHERE type = 'index' AND "
                         "name = 'idx_rollover_active_project';",
                         context) > 0,
            "project active-rollover terminal-state predicate changed");
    require(queryText(connection,
                      indexKeySql("memory_records_project_recent"), context) ==
                "project_id:0|is_tombstone:0|updated_at:1|id:0",
            "project stable recent-memory index columns changed");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM pragma_index_list('memory_records') "
                         "WHERE origin = 'pk' AND [unique] = 1;",
                         context) == 1 &&
                queryInteger(connection,
                             "SELECT COUNT(*) FROM pragma_index_list('memory_records') "
                             "WHERE origin = 'u' AND [unique] = 1;",
                             context) == 2,
            "project memory-record PK or UNIQUE constraints changed");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM pragma_foreign_key_list('memory_record_tags') "
                         "WHERE [table] = 'memory_records' AND [from] = 'record_id' "
                         "AND [to] = 'id' AND on_delete = 'CASCADE';",
                         context) == 1 &&
                queryInteger(connection,
                             "SELECT COUNT(*) FROM "
                             "pragma_foreign_key_list('memory_record_tags') WHERE "
                             "[table] = 'memory_tags' AND [from] = 'tag_id' AND "
                             "[to] = 'id' AND on_delete = 'NO ACTION';",
                             context) == 1,
            "project record-tag foreign keys changed");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM pragma_foreign_key_list('memory_links') "
                         "WHERE [table] = 'memory_records' AND [to] = 'id' "
                         "AND [from] IN ('source_id', 'target_id');",
                         context) == 2,
            "project memory-link foreign keys changed");
    require(queryText(connection, "PRAGMA main.quick_check(1);", context) == "ok",
            "project target failed its independent quick-check");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM pragma_foreign_key_check;", context) == 0,
            "project target failed its independent foreign-key check");

    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "project inspection leaked a VFS file owner");
}

void testFreshProjectSchemaAndIdempotentReopen()
{
    ScopedTestDirectory directory{L"project-fresh"};
    const auto context = activeContext("p07-project-fresh");
    ProjectDependencies dependencies{directory.path()};
    {
        auto database = openProject(dependencies, false, context);
        requireCurrentSnapshot(*database, false, context);
        take(database->close(context));
    }
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "fresh project close retained runtime ownership");
    requireCurrentLedgerAndConstraints(directory.path());

    const auto databasePath = directory.path() / L"memory.sqlite";
    const std::string beforeReopen = PersistenceSupport::readFixture(databasePath);
    {
        auto database = openProject(dependencies, false, context);
        requireCurrentSnapshot(*database, false, context);
        take(database->close(context));
    }
    requireCurrentLedgerAndConstraints(directory.path());
    require(PersistenceSupport::readFixture(databasePath) == beforeReopen,
            "idempotent project reopen changed the main database bytes");
}

void testProjectActiveRolloverIndexAndRetryResumeState()
{
    ScopedTestDirectory directory{L"project-p003-index"};
    const auto context = activeContext("p11-project-p003-index");
    ProjectDependencies dependencies{directory.path()};
    {
        auto database = openProject(dependencies, false, context);
        take(database->close(context));
    }

    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    take(connection.execute(
        R"sql(INSERT INTO rollover_operations(
    operation_id, project_id, predecessor_session_id, handoff_id, state, attempt,
    adapter_id, idempotency_key, created_at, updated_at, state_checksum,
    retry_resume_state
) VALUES
    ('10000000-0000-4000-8000-000000000001',
     '11111111-1111-4111-8111-111111111111',
     '20000000-0000-4000-8000-000000000001',
     '30000000-0000-4000-8000-000000000001',
     'predecessorSealed', 1, 'native-test', 'terminal-sealed',
     '2026-01-02T03:04:01Z', '2026-01-02T03:04:01Z', 'checksum-sealed', NULL),
    ('10000000-0000-4000-8000-000000000002',
     '11111111-1111-4111-8111-111111111111',
     '20000000-0000-4000-8000-000000000002',
     '30000000-0000-4000-8000-000000000002',
     'completed', 1, 'native-test', 'terminal-completed',
     '2026-01-02T03:04:02Z', '2026-01-02T03:04:02Z', 'checksum-completed', NULL),
    ('10000000-0000-4000-8000-000000000003',
     '11111111-1111-4111-8111-111111111111',
     '20000000-0000-4000-8000-000000000003',
     '30000000-0000-4000-8000-000000000003',
     'cancelled', 1, 'native-test', 'terminal-cancelled',
     '2026-01-02T03:04:03Z', '2026-01-02T03:04:03Z', 'checksum-cancelled', NULL),
    ('10000000-0000-4000-8000-000000000004',
     '11111111-1111-4111-8111-111111111111',
     '20000000-0000-4000-8000-000000000004',
     '30000000-0000-4000-8000-000000000004',
     'retry_wait', 2, 'native-test', 'active-retry',
     '2026-01-02T03:04:04Z', '2026-01-02T03:04:04Z', 'checksum-retry',
     'successor_creating');)sql",
        context));

    const auto conflictingActive = connection.execute(
        R"sql(INSERT INTO rollover_operations(
    operation_id, project_id, predecessor_session_id, handoff_id, state, attempt,
    adapter_id, idempotency_key, created_at, updated_at, state_checksum
) VALUES (
    '10000000-0000-4000-8000-000000000005',
    '11111111-1111-4111-8111-111111111111',
    '20000000-0000-4000-8000-000000000005',
    '30000000-0000-4000-8000-000000000005',
    'checkpoint_preparing', 1, 'native-test', 'active-conflict',
    '2026-01-02T03:04:05Z', '2026-01-02T03:04:05Z', 'checksum-conflict'
);)sql",
        context);
    require(!conflictingActive,
            "project P003 index admitted two active rollover operations for one project");
    require(queryInteger(
                connection,
                "SELECT COUNT(*) FROM rollover_operations WHERE project_id = "
                "'11111111-1111-4111-8111-111111111111' AND state IN "
                "('predecessorSealed', 'completed', 'cancelled');",
                context) == 3,
            "project P003 index did not allow all three terminal operation states to coexist");
    require(queryInteger(
                connection,
                "SELECT COUNT(*) FROM rollover_operations WHERE project_id = "
                "'11111111-1111-4111-8111-111111111111' AND state = 'retry_wait';",
                context) == 1,
            "project P003 index did not retain the one admitted active operation");
    require(queryText(
                connection,
                "SELECT retry_resume_state FROM rollover_operations WHERE "
                "idempotency_key = 'active-retry';",
                context) == "successor_creating",
            "project P003 retry resume state did not round-trip");
    require(queryText(connection, "PRAGMA main.quick_check(1);", context) == "ok",
            "project P003 index exercise failed quick-check");
    take(connection.close(context));
}

void testProjectVersion2MigrationAppendsP003AndPreservesData(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"project-v2-to-v3"};
    createProjectVersion2Fixture(directory.path(), fixtures / L"project-v1.sql");
    const auto context = activeContext("p11-project-v2-to-v3");

    {
        auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
        PersistenceMigrations::SchemaMigrator assessor{connection};
        const auto assessment = take(assessor.assess(
            PersistenceMigrations::DatabaseKind::Project, context));
        require(assessment.layout == PersistenceMigrations::SchemaLayout::ProjectVersion2 &&
                    assessment.sourceVersion == 2 && assessment.targetVersion == 3 &&
                    assessment.requiresOnlineBackup,
                "project v2 fixture was not recognized as a backup-qualified P003 source");
        require(queryInteger(connection,
                             "SELECT COUNT(*) FROM pragma_table_xinfo('rollover_operations') "
                             "WHERE name = 'retry_resume_state';",
                             context) == 0,
                "project v2 fixture unexpectedly contained the P003 retry column");
        take(connection.close(context));
    }

    ProjectDependencies dependencies{directory.path()};
    {
        auto database = openProject(dependencies, false, context);
        requireCurrentSnapshot(*database, false, context);
        take(database->close(context));
    }
    const auto databasePath = directory.path() / L"memory.sqlite";
    const auto backupPath = directory.path() /
        L"memory.sqlite.pre-migration.11111111-1111-4111-8111-111111111111.sqlite";
    require(std::filesystem::is_regular_file(backupPath),
            "project v2 migration did not publish its required online backup");

    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryInteger(connection, "PRAGMA main.user_version;", context) == 3,
            "project v2 migration did not commit user_version 3");
    require(queryText(
                connection,
                "SELECT group_concat(version || ':' || identifier || ':' || "
                "content_sha256, '|') FROM (SELECT * FROM schema_migrations "
                "ORDER BY version);",
                context) == ProjectLedger,
            "project v2 migration did not append the exact P003 ledger row");
    require(queryText(
                connection,
                "SELECT group_concat(applied_at, '|') FROM "
                "(SELECT applied_at FROM schema_migrations ORDER BY version);",
                context) == std::string{Version2AppliedAt} + "|" +
                    std::string{Version2AppliedAt} + "|" + std::string{AppliedAt},
            "project v2 migration rewrote prior ledger timestamps or misdated P003");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM rollover_operations WHERE "
                         "idempotency_key = 'legacy-rollover-idempotency' AND "
                         "retry_resume_state IS NULL;",
                         context) == 1,
            "project v2 migration did not preserve its existing rollover operation");
    take(connection.close(context));

    {
        auto backupEnvironment = KernelEnvironment::create(
            directory.path(), backupPath.filename().native());
        auto backup = openDatabase(
            *backupEnvironment, WinsqliteOpenMode::ReadOnlyExisting, context);
        require(queryInteger(backup, "PRAGMA main.user_version;", context) == 2 &&
                    queryInteger(
                        backup,
                        "SELECT COUNT(*) FROM schema_migrations;",
                        context) == 2 &&
                    queryInteger(
                        backup,
                        "SELECT COUNT(*) FROM pragma_table_xinfo('rollover_operations') "
                        "WHERE name = 'retry_resume_state';",
                        context) == 0,
                "project v2 backup did not preserve the exact admitted source schema");
        take(backup.close(context));
    }

    const std::string beforeReopen = PersistenceSupport::readFixture(databasePath);
    {
        auto database = openProject(dependencies, false, context);
        requireCurrentSnapshot(*database, false, context);
        take(database->close(context));
    }
    require(PersistenceSupport::readFixture(databasePath) == beforeReopen,
            "idempotent project v3 reopen changed the migrated v2 database bytes");
}

void testProjectVersion1MigrationPreservesDataAndBackfillsFts(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"project-v1"};
    createFixture(directory.path(), fixtures / L"project-v1.sql");
    const auto context = activeContext("p07-project-v1");
    ProjectDependencies dependencies{directory.path()};
    {
        auto database = openProject(dependencies, true, context);
        requireCurrentSnapshot(*database, true, context);
        take(database->close(context));
    }

    const auto backupPath = directory.path() /
        L"memory.sqlite.pre-migration.11111111-1111-4111-8111-111111111111.sqlite";
    require(std::filesystem::is_regular_file(backupPath),
            "project v1 migration did not publish its required online backup");
    requireCurrentLedgerAndConstraints(directory.path());

    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records WHERE "
                         "id = '22222222-2222-4222-8222-222222222222' AND "
                         "summary = 'project-v1-active-record-sentinel' AND "
                         "source = 'legacy-source-a' AND is_tombstone = 0;",
                         context) == 1,
            "project v1 active memory record was not preserved and backfilled");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records WHERE "
                         "id = '33333333-3333-4333-8333-333333333333' AND "
                         "summary = 'project-v1-tombstone-record-sentinel' AND "
                         "source = 'legacy_fixture' AND is_tombstone = 1;",
                         context) == 1,
            "project v1 tombstone was not preserved and backfilled");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_links WHERE "
                         "source_id = '22222222-2222-4222-8222-222222222222' AND "
                         "target_id = '33333333-3333-4333-8333-333333333333' AND "
                         "destination_id = target_id AND relation = 'supersedes';",
                         context) == 1,
            "project v1 memory link was not preserved and backfilled");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM event_journal WHERE id = 9 AND "
                          "detail = 'project-v1-event-sentinel' AND "
                          "event_type = action AND entity_id = record_id AND "
                          "payload_json IS NULL AND event_id IS NULL AND "
                          "idempotency_key IS NULL;",
                         context) == 1,
            "project v1 journal event was not preserved without reinterpreting its opaque detail as JSON");
    require(queryInteger(connection,
                         "SELECT (SELECT COUNT(*) FROM memory_tags) = 2 AND "
                         "(SELECT COUNT(*) FROM memory_record_tags) = 2 AND "
                         "(SELECT COUNT(*) FROM sessions WHERE "
                         "instr(state, 'legacy') > 0) = 1 AND "
                         "(SELECT COUNT(*) FROM artifacts WHERE "
                         "path = 'artifacts/legacy.txt') = 1 AND "
                         "(SELECT COUNT(*) FROM project_aliases WHERE "
                         "alias = 'legacy-project') = 1 AND "
                         "(SELECT COUNT(*) FROM maintenance_state WHERE "
                         "instr(state_json, 'legacy') > 0) = 1;",
                         context) == 1,
            "project v1 supporting durable rows were not preserved");
    require(queryInteger(connection,
                         "SELECT (SELECT COUNT(*) FROM continuity_handoffs WHERE "
                         "instr(payload_json, 'Legacy checkpoint persisted') > 0) = 1 "
                         "AND (SELECT COUNT(*) FROM rollover_operations WHERE "
                         "idempotency_key = 'legacy-rollover-idempotency') = 1 "
                         "AND (SELECT COUNT(*) FROM rollover_transitions WHERE "
                         "evidence = 'project-v1-transition-sentinel') = 1 "
                         "AND (SELECT COUNT(*) FROM project_active_sessions WHERE "
                         "session_id = '77777777-7777-4777-8777-777777777777') = 1;",
                         context) == 1,
            "project v1 continuity and rollover rows were not preserved");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "memory_records_fts MATCH 'Preserve';",
                         context) == 1,
            "project FTS backfill omitted the active v1 record");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "id = '33333333-3333-4333-8333-333333333333';",
                         context) == 0,
            "project FTS backfill indexed the v1 tombstone");
    take(connection.close(context));

    {
        auto database = openProject(dependencies, true, context);
        requireCurrentSnapshot(*database, true, context);
        take(database->close(context));
    }
    auto reopenedEnvironment = KernelEnvironment::create(
        directory.path(), L"memory.sqlite");
    auto reopened = openDatabase(
        *reopenedEnvironment, WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryInteger(reopened,
                         "SELECT COUNT(*) FROM memory_records_fts;", context) == 1,
            "idempotent FTS reopen duplicated the v1 backfill");
    require(queryText(reopened,
                      "SELECT MIN(applied_at) FROM schema_migrations;", context) ==
                AppliedAt,
            "idempotent project reopen rewrote the migration ledger");
    take(reopened.close(context));
}

void requireFtsMetadata(
    WinsqliteConnection& connection,
    const Domain::OperationContext& context)
{
    require(queryText(connection,
                      "SELECT sqlite_version() || '|' || sqlite_source_id();", context) ==
                "3.51.1|2025-11-28 17:28:25 "
                "281fc0e9afc38674b9b0991943b9e9d1e64c6cbdb133d35f6f5c87ff6af38a88",
            "project FTS tests are not running on the pinned WinSQLite build");
    require(queryText(connection, columnSignatureSql("memory_records_fts"), context) ==
                "0:id::0:<null>:0:0|1:title::0:<null>:0:0|"
                "2:summary::0:<null>:0:0|3:body::0:<null>:0:0|"
                "4:memory_records_fts::0:<null>:0:1|5:rank::0:<null>:0:1",
            "WinSQLite FTS virtual-table xinfo changed");
    require(queryText(connection, columnSignatureSql("memory_records_fts_config"), context) ==
                "0:k::1:<null>:1:0|1:v::0:<null>:0:0",
            "WinSQLite FTS config-shadow xinfo changed");
    require(queryText(connection, columnSignatureSql("memory_records_fts_content"), context) ==
                "0:id:INTEGER:0:<null>:1:0|1:c0::0:<null>:0:0|"
                "2:c1::0:<null>:0:0|3:c2::0:<null>:0:0|4:c3::0:<null>:0:0",
            "WinSQLite FTS content-shadow xinfo changed");
    require(queryText(connection, columnSignatureSql("memory_records_fts_data"), context) ==
                "0:id:INTEGER:0:<null>:1:0|1:block:BLOB:0:<null>:0:0",
            "WinSQLite FTS data-shadow xinfo changed");
    require(queryText(connection, columnSignatureSql("memory_records_fts_docsize"), context) ==
                "0:id:INTEGER:0:<null>:1:0|1:sz:BLOB:0:<null>:0:0",
            "WinSQLite FTS docsize-shadow xinfo changed");
    require(queryText(connection, columnSignatureSql("memory_records_fts_idx"), context) ==
                "0:segid::1:<null>:1:0|1:term::1:<null>:2:0|"
                "2:pgno::0:<null>:0:0",
            "WinSQLite FTS index-shadow xinfo changed");
    require(queryText(
                connection,
                "SELECT group_concat(name || ':' || [unique] || ':' || origin || ':' || "
                "partial, '|') FROM (SELECT * FROM "
                "pragma_index_list('memory_records_fts_config') ORDER BY seq);",
                context) == "sqlite_autoindex_memory_records_fts_config_1:1:pk:0",
            "WinSQLite FTS config-shadow autoindex changed");
    require(queryText(
                connection,
                "SELECT group_concat(name || ':' || [unique] || ':' || origin || ':' || "
                "partial, '|') FROM (SELECT * FROM "
                "pragma_index_list('memory_records_fts_idx') ORDER BY seq);",
                context) == "sqlite_autoindex_memory_records_fts_idx_1:1:pk:0",
            "WinSQLite FTS index-shadow autoindex changed");
    require(queryInteger(connection,
                         "SELECT (SELECT COUNT(*) FROM "
                         "pragma_index_list('memory_records_fts')) + "
                         "(SELECT COUNT(*) FROM "
                         "pragma_index_list('memory_records_fts_content')) + "
                         "(SELECT COUNT(*) FROM "
                         "pragma_index_list('memory_records_fts_data')) + "
                         "(SELECT COUNT(*) FROM "
                         "pragma_index_list('memory_records_fts_docsize'));",
                         context) == 0,
            "WinSQLite FTS emitted an unexpected shadow autoindex");
    const std::string ftsObjects = queryText(
        connection,
        "SELECT group_concat(type || ':' || name, '|') FROM (SELECT type, name "
        "FROM sqlite_schema WHERE name LIKE 'memory_records_fts%' OR "
        "name LIKE 'sqlite_autoindex_memory_records_fts_%' "
        "ORDER BY type, name);",
        context);
    require(ftsObjects ==
                "table:memory_records_fts|table:memory_records_fts_config|"
                "table:memory_records_fts_content|table:memory_records_fts_data|"
                "table:memory_records_fts_docsize|table:memory_records_fts_idx",
            "WinSQLite FTS emitted an unexpected object or omitted a shadow object: " +
                ftsObjects);
}

void testProjectFtsLifecycleAndPinnedMetadata()
{
    ScopedTestDirectory directory{L"project-fts"};
    const auto context = activeContext("p07-project-fts");
    ProjectDependencies dependencies{directory.path()};
    {
        auto database = openProject(dependencies, true, context);
        requireCurrentSnapshot(*database, true, context);
        take(database->close(context));
    }

    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    requireFtsMetadata(connection, context);
    take(connection.execute(
        "INSERT INTO memory_records("
        "id, project_id, version, kind, title, summary, body, importance, confidence, "
        "source_kind, created_at, updated_at, last_accessed_at, content_hash, "
        "is_tombstone, schema_version"
        ") VALUES("
        "'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa', "
        "'11111111-1111-4111-8111-111111111111', 1, 'fact', "
        "'alpha title', 'alpha summary', 'alpha body', 0.5, 0.75, 'test', "
        "'2026-01-02T03:04:05Z', '2026-01-02T03:04:05Z', "
        "'2026-01-02T03:04:05Z', "
        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', "
        "0, 2);",
        context));
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "memory_records_fts MATCH 'alpha';",
                         context) == 1,
            "project FTS insert trigger did not index the new record");

    take(connection.execute(
        "UPDATE memory_records SET title = 'beta title', summary = 'beta summary', "
        "body = 'beta body' WHERE id = "
        "'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';",
        context));
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "memory_records_fts MATCH 'alpha';",
                         context) == 0 &&
                queryInteger(connection,
                             "SELECT COUNT(*) FROM memory_records_fts WHERE "
                             "memory_records_fts MATCH 'beta';",
                             context) == 1,
            "project FTS update trigger retained stale terms or omitted new terms");

    take(connection.execute(
        "UPDATE memory_records SET is_tombstone = 1 WHERE id = "
        "'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';",
        context));
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "memory_records_fts MATCH 'beta';",
                         context) == 0,
            "project FTS update trigger retained a tombstoned record");

    take(connection.execute(
        "UPDATE memory_records SET is_tombstone = 0, title = 'gamma title', "
        "summary = 'gamma summary', body = 'gamma body' WHERE id = "
        "'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';",
        context));
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "memory_records_fts MATCH 'gamma';",
                         context) == 1,
            "project FTS update trigger did not restore an active record");

    take(connection.execute(
        "DELETE FROM memory_records WHERE id = "
        "'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';",
        context));
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM memory_records_fts WHERE "
                         "memory_records_fts MATCH 'gamma';",
                         context) == 0,
            "project FTS delete trigger retained a deleted record");
    take(connection.close(context));
}

void requireRejectedWithoutMainMutation(
    const std::filesystem::path& directory,
    const std::string_view expectedCode,
    const std::string_view message)
{
    const auto databasePath = directory / L"memory.sqlite";
    const std::string before = PersistenceSupport::readFixture(databasePath);
    const auto context = activeContext("p07-project-rejection");
    ProjectDependencies dependencies{directory};
    const auto opened = PersistenceWindows::WindowsProjectDatabase::open(
        projectId(), dependencies.paths, dependencies.diagnostics, dependencies.clock,
        PersistenceWindows::WindowsProjectDatabaseOptions{false}, context);
    requireError(opened, expectedCode, message);
    require(PersistenceSupport::readFixture(databasePath) == before, message);
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "rejected project open retained runtime ownership");
}

void requirePrivateAssessmentRejectedWithoutMutation(
    const std::filesystem::path& directory,
    const std::string_view expectedCode,
    const std::string_view message)
{
    const auto databasePath = directory / L"memory.sqlite";
    const std::string before = PersistenceSupport::readFixture(databasePath);
    const auto context = activeContext("p07-project-private-rejection");
    auto environment = KernelEnvironment::create(directory, L"memory.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    PersistenceMigrations::SchemaMigrator assessor{connection};
    const auto assessed = assessor.assess(
        PersistenceMigrations::DatabaseKind::Project, context);
    requireError(assessed, expectedCode, message);
    take(connection.close(context));
    environment.reset();
    require(PersistenceSupport::readFixture(databasePath) == before, message);
}

void createFreshProjectWithoutFts(
    const std::filesystem::path& directory,
    const std::string_view correlation)
{
    const auto context = activeContext(correlation);
    ProjectDependencies dependencies{directory};
    auto database = openProject(dependencies, false, context);
    take(database->close(context));
}

void testProjectRejectsUnsupportedFutureAmbiguousAndMalformedLayouts(
    const std::filesystem::path& fixtures)
{
    {
        ScopedTestDirectory directory{L"project-future"};
        createFixture(directory.path(), fixtures / L"project-v1.sql");
        const auto context = activeContext("p07-project-create-future");
        auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute("PRAGMA main.user_version = 4;", context));
        take(connection.close(context));
        environment.reset();
        requireRejectedWithoutMainMutation(
            directory.path(), Domain::ErrorCodes::UnsupportedVersion,
            "future project schema was not rejected without changing the main file");
    }
    {
        ScopedTestDirectory directory{L"project-ambiguous"};
        createFixture(directory.path(), fixtures / L"project-v1.sql");
        const auto context = activeContext("p07-project-create-ambiguous");
        auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute(
            "CREATE TABLE foreign_extra(value TEXT NOT NULL);", context));
        take(connection.close(context));
        environment.reset();
        requireRejectedWithoutMainMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "ambiguous project schema was not rejected without changing the main file");
    }
    {
        ScopedTestDirectory directory{L"project-checksum"};
        createFreshProjectWithoutFts(
            directory.path(), "p07-project-create-checksum");
        const auto context = activeContext("p07-project-corrupt-checksum");
        auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute(
            "UPDATE schema_migrations SET content_sha256 = "
            "'0000000000000000000000000000000000000000000000000000000000000000' "
            "WHERE version = 2;",
            context));
        take(connection.close(context));
        environment.reset();
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "project checksum mismatch was not rejected read-only");
    }
    {
        ScopedTestDirectory directory{L"project-ledger-gap"};
        createFreshProjectWithoutFts(
            directory.path(), "p07-project-create-ledger-gap");
        const auto context = activeContext("p07-project-corrupt-ledger");
        auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute(
            "DELETE FROM schema_migrations WHERE version = 1;", context));
        take(connection.close(context));
        environment.reset();
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "project noncontiguous ledger was not rejected read-only");
    }
    {
        ScopedTestDirectory directory{L"project-p003-index-predicate"};
        createFreshProjectWithoutFts(
            directory.path(), "p11-project-create-old-index-predicate");
        const auto context = activeContext("p11-project-corrupt-index-predicate");
        auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute(
            "DROP INDEX idx_rollover_active_project;"
            "CREATE UNIQUE INDEX idx_rollover_active_project "
            "ON rollover_operations(project_id) "
            "WHERE state <> 'predecessorSealed';",
            context));
        take(connection.close(context));
        environment.reset();
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "project v3 accepted the obsolete active-rollover predicate");
    }
}

void testProjectRejectsCommentSplitSchemaModifiersReadOnly(
    const std::filesystem::path& fixtures)
{
    const auto fixture = fixtures / L"project-v1.sql";
    {
        ScopedTestDirectory directory{L"project-comment-check-block"};
        createFixtureWithExactReplacement(
            directory.path(), fixture,
            "is_tombstone INTEGER NOT NULL DEFAULT 0,",
            "is_tombstone INTEGER NOT NULL DEFAULT 0 "
            "CHECK/**/(is_tombstone IN (0, 1)),");
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "block-comment-split CHECK was not rejected read-only");
    }
    {
        ScopedTestDirectory directory{L"project-comment-check-line"};
        createFixtureWithExactReplacement(
            directory.path(), fixture,
            "is_tombstone INTEGER NOT NULL DEFAULT 0,",
            "is_tombstone INTEGER NOT NULL DEFAULT 0 "
            "CHECK-- comment boundary\n(is_tombstone IN (0, 1)),");
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "line-comment-split CHECK was not rejected read-only");
    }
    {
        ScopedTestDirectory directory{L"project-comment-without-rowid"};
        createFixtureWithExactReplacement(
            directory.path(), fixture,
            "CREATE TABLE IF NOT EXISTS memory_record_tags(record_id TEXT NOT NULL "
            "REFERENCES memory_records(id) ON DELETE CASCADE,tag_id INTEGER NOT NULL "
            "REFERENCES memory_tags(id),PRIMARY KEY(record_id,tag_id));",
            "CREATE TABLE IF NOT EXISTS memory_record_tags(record_id TEXT NOT NULL "
            "REFERENCES memory_records(id) ON DELETE CASCADE,tag_id INTEGER NOT NULL "
            "REFERENCES memory_tags(id),PRIMARY KEY(record_id,tag_id)) "
            "WITHOUT/**/ROWID;");
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "comment-split WITHOUT ROWID was not rejected read-only");
    }
    {
        ScopedTestDirectory directory{L"project-comment-on-conflict"};
        createFixtureWithExactReplacement(
            directory.path(), fixture,
            "name TEXT UNIQUE NOT NULL",
            "name TEXT UNIQUE ON/**/CONFLICT ABORT NOT NULL");
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "comment-split ON CONFLICT was not rejected read-only");
    }
    {
        ScopedTestDirectory directory{L"project-comment-quoted-content"};
        createFixtureWithExactReplacement(
            directory.path(), fixture,
            "WHERE state <> 'predecessorSealed';",
            "WHERE state <> 'predecessor/**/Sealed';");
        requirePrivateAssessmentRejectedWithoutMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "a comment marker inside quoted SQL content was not preserved");
    }
}

void injectProjectForeignKeyViolation(const std::filesystem::path& databasePath)
{
    const auto utf8Path = take(
        Infrastructure::Windows::Detail::strictUtf16ToUtf8(databasePath.native()));
    sqlite3* database{};
    const int openResult = sqlite3_open_v2(
        utf8Path.c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
    if (openResult != SQLITE_OK) {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
        require(false, "could not open the rollback fixture through WinSQLite");
    }

    char* errorMessage{};
    const int insertResult = sqlite3_exec(
        database,
        "PRAGMA foreign_keys = OFF;"
        "INSERT INTO memory_record_tags(record_id, tag_id) "
        "VALUES('ffffffff-ffff-4fff-8fff-ffffffffffff', 1);",
        nullptr, nullptr, &errorMessage);
    std::string errorText;
    if (errorMessage != nullptr) {
        errorText = errorMessage;
        sqlite3_free(errorMessage);
    }
    const int closeResult = sqlite3_close_v2(database);
    require(insertResult == SQLITE_OK,
            "could not seed the rollback foreign-key violation: " + errorText);
    require(closeResult == SQLITE_OK,
            "could not close the rollback fixture after fault seeding");
}

void testProjectMigrationRollsBackAfterIntegrityFailure(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"project-rollback"};
    createFixture(directory.path(), fixtures / L"project-v1.sql");
    injectProjectForeignKeyViolation(directory.path() / L"memory.sqlite");

    const auto context = activeContext("p07-project-rollback");
    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto readOnly = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    PersistenceMigrations::SchemaMigrator readOnlyAssessor{readOnly};
    const auto sourceAssessment = take(readOnlyAssessor.assess(
        PersistenceMigrations::DatabaseKind::Project, context));
    require(sourceAssessment.layout ==
                PersistenceMigrations::SchemaLayout::ProjectVersion1 &&
                sourceAssessment.requiresOnlineBackup,
            "rollback fixture was not recognized as evidenced project v1");
    auto writable = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
        writable, context));
    PersistenceMigrations::SchemaMigrator migrator{writable};
    const auto writableAssessment = take(migrator.assess(
        transaction, PersistenceMigrations::DatabaseKind::Project));
    auto backup = take(PersistenceWindows::DatabaseBackupCoordinator::createMigrationBackup(
        readOnly, transaction, environment->namespaceLease(), writableAssessment, context));
    auto receipt = take(backup.bindToTransaction(
        transaction, *environment->namespaceLease(), writableAssessment));
    take(readOnly.close(context));
    const auto migrated = migrator.migrate(
        transaction, writableAssessment, &receipt, AppliedAt);
    requireError(migrated, Domain::ErrorCodes::IntegrityFailure,
                 "project migration did not fail its final foreign-key check");

    const auto afterFailure = take(migrator.assess(
        PersistenceMigrations::DatabaseKind::Project, context));
    require(afterFailure.layout ==
                PersistenceMigrations::SchemaLayout::ProjectVersion1 &&
                afterFailure.sourceVersion == 1 &&
                afterFailure.requiresOnlineBackup,
            "failed project migration did not restore its v1 assessment");
    require(queryInteger(writable, "PRAGMA main.user_version;", context) == 1,
            "failed project migration retained a target user_version");
    require(queryInteger(writable,
                         "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' "
                         "AND name IN ('schema_migrations', 'project_metadata');",
                         context) == 0,
            "failed project migration retained newly created target tables");
    require(queryInteger(writable,
                         "SELECT COUNT(*) FROM pragma_table_xinfo('memory_records') "
                         "WHERE name = 'source';",
                         context) == 0 &&
                queryInteger(writable,
                             "SELECT COUNT(*) FROM pragma_table_xinfo('memory_links') "
                             "WHERE name = 'destination_id';",
                             context) == 0 &&
                queryInteger(writable,
                             "SELECT COUNT(*) FROM pragma_table_xinfo('event_journal') "
                             "WHERE name = 'event_id';",
                             context) == 0,
            "failed project migration retained ALTER TABLE columns");
    require(queryInteger(writable,
                         "SELECT COUNT(*) FROM pragma_foreign_key_check;", context) == 1,
            "rollback fixture lost the pre-existing foreign-key violation");
    require(queryText(writable, "PRAGMA main.quick_check(1);", context) == "ok",
            "rolled-back project database failed quick-check");
    take(writable.close(context));
}

void testMigrationBackupArtifactRemainsPinnedThroughMigration(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"project-backup-receipt"};
    createFixture(directory.path(), fixtures / L"project-v1.sql");

    const auto context = activeContext("p07-project-backup-receipt");
    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto readOnly = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    PersistenceMigrations::SchemaMigrator assessor{readOnly};
    const auto sourceAssessment = take(assessor.assess(
        PersistenceMigrations::DatabaseKind::Project, context));
    require(sourceAssessment.requiresOnlineBackup,
            "backup-receipt fixture did not require a migration backup");

    auto writable = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
        writable, context));
    PersistenceMigrations::SchemaMigrator migrator{writable};
    const auto writableAssessment = take(migrator.assess(
        transaction, PersistenceMigrations::DatabaseKind::Project));
    std::optional<PersistenceWindows::DatabaseBackupCoordinator> backup;
    backup.emplace(take(
        PersistenceWindows::DatabaseBackupCoordinator::createMigrationBackup(
            readOnly,
            transaction,
            environment->namespaceLease(),
            writableAssessment,
            context)));
    const auto backupPath = std::filesystem::path{take(
        Infrastructure::Windows::Detail::strictUtf8ToUtf16(
            backup->report().backupPath.value()))};
    require(std::filesystem::is_regular_file(backupPath),
            "the verified migration backup was not published");

    requireBackupArtifactMutationBlocked(backupPath, "the migration-backup coordinator");

    std::optional<PersistenceMigrations::MigrationBackupReceipt> receipt;
    receipt.emplace(take(backup->bindToTransaction(
        transaction, *environment->namespaceLease(), writableAssessment)));
    take(readOnly.close(context));
    backup.reset();

    requireBackupArtifactMutationBlocked(backupPath, "the bound migration receipt");

    const auto migrated = migrator.migrate(
        transaction, writableAssessment, &receipt.value(), AppliedAt);
    require(static_cast<bool>(migrated),
            "the migration failed with its exact verified backup artifact retained");
    receipt.reset();

    require(::DeleteFileW(backupPath.c_str()) != FALSE,
            "the migration receipt did not release its backup lease after migration");
    require(!std::filesystem::exists(backupPath),
            "the released migration backup could not be deleted");
    take(writable.close(context));
}

void testMigrationBackupRejectsForeignSameSchemaSource(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory firstDirectory{L"project-backup-provenance-a"};
    ScopedTestDirectory secondDirectory{L"project-backup-provenance-b"};
    createFixture(firstDirectory.path(), fixtures / L"project-v1.sql");
    createFixture(secondDirectory.path(), fixtures / L"project-v1.sql");

    const auto context = activeContext("p07-project-backup-provenance");
    auto firstEnvironment = KernelEnvironment::create(
        firstDirectory.path(), L"memory.sqlite");
    auto secondEnvironment = KernelEnvironment::create(
        secondDirectory.path(), L"memory.sqlite");
    auto foreignReadOnly = openDatabase(
        *secondEnvironment, WinsqliteOpenMode::ReadOnlyExisting, context);
    PersistenceMigrations::SchemaMigrator foreignAssessor{foreignReadOnly};
    const auto foreignAssessment = take(foreignAssessor.assess(
        PersistenceMigrations::DatabaseKind::Project, context));

    auto admittedConnection = openDatabase(
        *firstEnvironment, WinsqliteOpenMode::ReadWriteExisting, context);
    auto admittedTransaction = take(
        PersistenceDetail::WinsqliteTransaction::beginImmediate(
            admittedConnection, context));
    PersistenceMigrations::SchemaMigrator admittedMigrator{admittedConnection};
    const auto admittedAssessment = take(admittedMigrator.assess(
        admittedTransaction, PersistenceMigrations::DatabaseKind::Project));
    require(admittedAssessment == foreignAssessment &&
                admittedAssessment.requiresOnlineBackup,
            "the provenance rejection fixtures did not have the same evidenced schema");

    const auto rejected =
        PersistenceWindows::DatabaseBackupCoordinator::createMigrationBackup(
            foreignReadOnly,
            admittedTransaction,
            firstEnvironment->namespaceLease(),
            admittedAssessment,
            context);
    requireError(rejected, Domain::ErrorCodes::InvalidRequest,
                 "migration backup accepted a same-schema connection from a foreign namespace");
    requireNoMigrationBackupArtifact(
        firstDirectory.path(),
        "foreign backup provenance created an artifact in the admitted namespace");
    requireNoMigrationBackupArtifact(
        secondDirectory.path(),
        "foreign backup provenance created an artifact in the read-only namespace");

    take(admittedTransaction.rollback());
    take(foreignReadOnly.close(context));
    take(admittedConnection.close(context));
    require(firstEnvironment->openFileCount() == 0U &&
                secondEnvironment->openFileCount() == 0U,
            "foreign backup provenance rejection retained a VFS file owner");
}

void testMigrationBackupCapturesExactAdmittedGeneration(
    const std::filesystem::path& fixtures)
{
    constexpr std::uint32_t BeforeAdmissionMarker = 9'101U;
    constexpr std::uint32_t AfterAdmissionMarker = 9'102U;

    ScopedTestDirectory directory{L"project-backup-generation"};
    createFixture(directory.path(), fixtures / L"project-v1.sql");
    const std::filesystem::path fixture = processFixturePath();

    auto beforeStart = createGenerationEvent(L"BeforeStart");
    auto beforeAttempted = createGenerationEvent(L"BeforeAttempted");
    auto beforeDone = createGenerationEvent(L"BeforeDone");
    auto beforeWriter = GenerationWriterProcess::launch(
        fixture,
        generationWriterArguments(
            directory.path(), beforeStart, beforeAttempted, beforeDone,
            BeforeAdmissionMarker));
    signalGenerationEvent(
        beforeStart, "the pre-admission generation writer could not be started");
    waitForGenerationEventOrProcess(
        beforeAttempted, beforeWriter,
        "the pre-admission generation writer exited before its write attempt",
        "the pre-admission generation writer did not attempt its write");
    waitForGenerationEventOrProcess(
        beforeDone, beforeWriter,
        "the pre-admission generation writer exited before reporting commit",
        "the pre-admission generation writer did not commit within 15 seconds");
    require(beforeWriter.wait(ChildWaitMilliseconds) == 0U,
            "the pre-admission generation writer did not exit successfully");

    const auto context = activeContext("p07-project-backup-generation");
    auto environment = KernelEnvironment::create(directory.path(), L"memory.sqlite");
    auto readOnly = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    PersistenceMigrations::SchemaMigrator sourceAssessor{readOnly};
    const auto sourceAssessment = take(sourceAssessor.assess(
        PersistenceMigrations::DatabaseKind::Project, context));
    require(countGenerationTag(readOnly, BeforeAdmissionMarker, context) == 1,
            "the admitted source did not observe the pre-admission writer commit");

    auto writable = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteExisting, context);
    auto admittedTransaction = take(
        PersistenceDetail::WinsqliteTransaction::beginImmediate(writable, context));
    PersistenceMigrations::SchemaMigrator migrator{writable};
    const auto admittedAssessment = take(migrator.assess(
        admittedTransaction, PersistenceMigrations::DatabaseKind::Project));
    require(admittedAssessment == sourceAssessment &&
                admittedAssessment.requiresOnlineBackup,
            "the admitted migration transaction changed the evidenced source schema");

    auto afterStart = createGenerationEvent(L"AfterStart");
    auto afterAttempted = createGenerationEvent(L"AfterAttempted");
    auto afterDone = createGenerationEvent(L"AfterDone");
    auto afterWriter = GenerationWriterProcess::launch(
        fixture,
        generationWriterArguments(
            directory.path(), afterStart, afterAttempted, afterDone,
            AfterAdmissionMarker));
    signalGenerationEvent(
        afterStart, "the post-admission generation writer could not be started");
    waitForGenerationEventOrProcess(
        afterAttempted, afterWriter,
        "the post-admission generation writer exited before its write attempt",
        "the post-admission generation writer did not reach SQLite step");
    requireGenerationWriterBlocked(afterDone, afterWriter);

    std::filesystem::path backupPath;
    {
        auto backup = take(
            PersistenceWindows::DatabaseBackupCoordinator::createMigrationBackup(
                readOnly,
                admittedTransaction,
                environment->namespaceLease(),
                admittedAssessment,
                context));
        backupPath = std::filesystem::path{take(
            InfrastructureDetail::strictUtf8ToUtf16(
                backup.report().backupPath.value()))};
        require(std::filesystem::is_regular_file(backupPath),
                "the admitted generation backup was not published");

        auto receipt = take(backup.bindToTransaction(
            admittedTransaction, *environment->namespaceLease(), admittedAssessment));
        take(readOnly.close(context));
        const auto migrated = migrator.migrate(
            admittedTransaction, admittedAssessment, &receipt, AppliedAt);
        require(static_cast<bool>(migrated),
                "the exact-generation project migration did not commit");
    }

    waitForGenerationEventOrProcess(
        afterDone, afterWriter,
        "the post-admission generation writer exited before reporting commit",
        "the post-admission generation writer did not resume after migration commit");
    require(afterWriter.wait(ChildWaitMilliseconds) == 0U,
            "the post-admission generation writer did not exit successfully");

    require(queryInteger(writable, "PRAGMA main.user_version;", context) == 3 &&
                countGenerationTag(writable, BeforeAdmissionMarker, context) == 1 &&
                countGenerationTag(writable, AfterAdmissionMarker, context) == 1,
            "the live migrated database did not serialize the post-admission writer after commit");
    {
        auto backupEnvironment = KernelEnvironment::create(
            directory.path(), backupPath.filename().native());
        auto backupConnection = openDatabase(
            *backupEnvironment, WinsqliteOpenMode::ReadOnlyExisting, context);
        require(queryInteger(
                    backupConnection, "PRAGMA main.user_version;", context) == 1 &&
                    countGenerationTag(
                        backupConnection, BeforeAdmissionMarker, context) == 1 &&
                    countGenerationTag(
                        backupConnection, AfterAdmissionMarker, context) == 0,
                "the migration backup did not retain the exact admitted source generation");
        take(backupConnection.close(context));
    }
    take(writable.close(context));
    require(environment->openFileCount() == 0U,
            "exact migration-generation verification retained a VFS file owner");
}

} // namespace

void registerProjectMigrationTests(
    TestRegistry& tests,
    const std::filesystem::path& fixtures)
{
    addTest(tests, "persistence.project.fresh-idempotent",
            testFreshProjectSchemaAndIdempotentReopen);
    addTest(tests, "persistence.project.p003-index-retry-resume",
            testProjectActiveRolloverIndexAndRetryResumeState);
    addTest(tests, "persistence.project.v2-p003-preservation",
            [fixtures] {
                testProjectVersion2MigrationAppendsP003AndPreservesData(fixtures);
            });
    addTest(tests, "persistence.project.v1-preservation-fts-backfill",
            [fixtures] {
                testProjectVersion1MigrationPreservesDataAndBackfillsFts(fixtures);
            });
    addTest(tests, "persistence.project.fts-lifecycle-metadata",
            testProjectFtsLifecycleAndPinnedMetadata);
    addTest(tests, "persistence.project.reject-read-only",
            [fixtures] {
                testProjectRejectsUnsupportedFutureAmbiguousAndMalformedLayouts(fixtures);
            });
    addTest(tests, "persistence.project.reject-comment-split-schema",
            [fixtures] {
                testProjectRejectsCommentSplitSchemaModifiersReadOnly(fixtures);
            });
    addTest(tests, "persistence.project.rollback-integrity",
            [fixtures] {
                testProjectMigrationRollsBackAfterIntegrityFailure(fixtures);
            });
    addTest(tests, "persistence.project.backup-receipt-lease",
            [fixtures] {
                testMigrationBackupArtifactRemainsPinnedThroughMigration(fixtures);
            });
    addTest(tests, "persistence.project.backup-provenance-rejection",
            [fixtures] {
                testMigrationBackupRejectsForeignSameSchemaSource(fixtures);
            });
    addTest(tests, "persistence.project.backup-generation",
            [fixtures] {
                testMigrationBackupCapturesExactAdmittedGeneration(fixtures);
            });
}

} // namespace ForgeConductor::Tests
