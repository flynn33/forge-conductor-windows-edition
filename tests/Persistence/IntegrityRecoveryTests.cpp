#include "PersistenceTestSupport.h"

#include "Fakes/DiagnosticsFakes.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "Persistence/Windows/Detail/WindowsDatabaseStore.h"

#include <nlohmann/json.hpp>
#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Tests {
namespace {

namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;
namespace PersistenceMigrations = ForgeConductor::Persistence::Windows::Migrations;
namespace TestFakes = ForgeConductor::Tests::Fakes;
using namespace std::chrono_literals;
using PersistenceSupport::FixedClock;
using PersistenceSupport::ScopedTestDirectory;

inline constexpr std::wstring_view CentralMainBasename = L"store.sqlite";
inline constexpr std::wstring_view CentralMigrationLockBasename =
    L"store.sqlite.migration.lock";

struct CohortRole final {
    std::string_view name;
    std::wstring_view suffix;
};

inline constexpr std::array CohortRoles{
    CohortRole{"main", L""},
    CohortRole{"wal", L"-wal"},
    CohortRole{"shared_memory", L"-shm"},
    CohortRole{"journal", L"-journal"}};

using CapturedCohort =
    std::array<std::optional<std::string>, CohortRoles.size()>;

[[nodiscard]] std::filesystem::path canonicalDirectory(
    const std::filesystem::path& directory)
{
    std::error_code error;
    const auto canonical = std::filesystem::canonical(directory, error);
    require(!error, "the integrity-recovery test directory could not be canonicalized");
    return canonical;
}

[[nodiscard]] std::filesystem::path sidecarPath(
    const std::filesystem::path& mainPath,
    const std::wstring_view suffix)
{
    return std::filesystem::path{mainPath.native() + std::wstring{suffix}};
}

[[nodiscard]] std::filesystem::path quarantinePathFor(
    const std::filesystem::path& directory,
    const std::string_view operationId)
{
    return canonicalDirectory(directory) /
        (std::wstring{CentralMainBasename} + L".corrupt." +
         std::wstring{operationId.begin(), operationId.end()} + L".sqlite");
}

void writeFixture(
    const std::filesystem::path& path,
    const std::string_view bytes)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    require(output.good(),
            "the integrity-recovery fixture could not be opened for writing");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    require(output.good(),
            "the complete integrity-recovery fixture could not be written");
}

[[nodiscard]] std::string sha256(const std::string_view bytes)
{
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    return take(hasher.sha256(std::as_bytes(
        std::span<const char>{bytes.data(), bytes.size()}))).value();
}

[[nodiscard]] CapturedCohort captureCohort(
    const std::filesystem::path& mainPath)
{
    CapturedCohort captured;
    for (std::size_t index = 0U; index < CohortRoles.size(); ++index) {
        const auto& role = CohortRoles[index];
        const std::filesystem::path path = role.suffix.empty()
            ? mainPath
            : sidecarPath(mainPath, role.suffix);
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        require(!error,
                "an integrity-recovery source role could not be inspected");
        if (exists) {
            require(std::filesystem::is_regular_file(path, error) && !error,
                    "an integrity-recovery source role is not a regular file");
            captured[index].emplace(
                PersistenceSupport::readFixture(path, 16U * 1024U * 1024U));
        }
    }
    require(captured.front().has_value(),
            "the integrity-recovery source cohort has no main file");
    return captured;
}

class IntegrityRecoveryObserver final
    : public PersistenceDetail::IDatabaseIntegrityRecoveryObserver {
public:
    using Callback = std::function<void()>;

    explicit IntegrityRecoveryObserver(Callback callback)
        : callback_{std::move(callback)}
    {
    }

    void onIntegrityFailureDetected() noexcept override
    {
        ++callCount_;
        if (!callback_ || callbackFailure_ != nullptr) {
            return;
        }
        try {
            callback_();
        } catch (...) {
            callbackFailure_ = std::current_exception();
        }
    }

    void rethrowCallbackFailure() const
    {
        if (callbackFailure_ != nullptr) {
            std::rethrow_exception(callbackFailure_);
        }
    }

    [[nodiscard]] std::size_t callCount() const noexcept { return callCount_; }

private:
    Callback callback_;
    std::exception_ptr callbackFailure_;
    std::size_t callCount_{};
};

struct StoreDependencies final {
    StoreDependencies()
    {
        const auto monotonic = std::chrono::steady_clock::now();
        diagnostics = std::make_shared<TestFakes::RuntimeDiagnosticsFake>(monotonic);
        clock = std::make_shared<FixedClock>(
            Domain::UtcTimePoint{std::chrono::seconds{1'767'326'645}},
            monotonic);
    }

    std::shared_ptr<TestFakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<FixedClock> clock;
};

[[nodiscard]] Domain::OperationContext recoveryContext(
    const std::string_view operationId,
    const std::string_view correlationId,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operationId),
        std::chrono::steady_clock::now() + 2min,
        cancellation,
        parse<Domain::CorrelationId>(correlationId)};
}

[[nodiscard]] Domain::Result<std::unique_ptr<PersistenceDetail::WindowsDatabaseStore>>
openCorruptStore(
    const std::filesystem::path& directory,
    StoreDependencies& dependencies,
    IntegrityRecoveryObserver& observer,
    const PersistenceMigrations::DatabaseKind databaseKind,
    const Domain::OperationContext& context)
{
    return PersistenceDetail::WindowsDatabaseStore::open(
        PersistenceSupport::pathText(canonicalDirectory(directory)),
        CentralMainBasename,
        CentralMigrationLockBasename,
        dependencies.diagnostics,
        dependencies.clock,
        PersistenceDetail::WindowsDatabaseStoreOptions{
            databaseKind,
            PersistenceDetail::WinsqliteSynchronousMode::Full,
            false,
            &observer},
        context);
}

class RawSqliteDatabase final {
public:
    [[nodiscard]] static std::unique_ptr<RawSqliteDatabase> open(
        const std::filesystem::path& path)
    {
        sqlite3* database{};
        const std::string pathUtf8 = PersistenceSupport::pathText(path).value();
        const int opened = ::sqlite3_open_v2(
            pathUtf8.c_str(), &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, nullptr);
        if (opened != SQLITE_OK) {
            const std::string message = database != nullptr
                ? ::sqlite3_errmsg(database)
                : ::sqlite3_errstr(opened);
            if (database != nullptr) {
                static_cast<void>(::sqlite3_close_v2(database));
            }
            throw TestFailure{
                "the raw project corruption fixture could not be opened: " + message};
        }
        return std::unique_ptr<RawSqliteDatabase>{new RawSqliteDatabase{database}};
    }

    ~RawSqliteDatabase() noexcept
    {
        if (database_ != nullptr) {
            static_cast<void>(::sqlite3_close_v2(database_));
        }
    }

    RawSqliteDatabase(const RawSqliteDatabase&) = delete;
    RawSqliteDatabase& operator=(const RawSqliteDatabase&) = delete;

    void retainWalOnClose()
    {
        int observed{};
        const int configured = ::sqlite3_db_config(
            database_, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &observed);
        require(configured == SQLITE_OK && observed == 1,
                "the raw corruption fixture could not retain WAL on close");
    }

    void execute(const std::string_view sql)
    {
        const std::string terminated{sql};
        char* nativeMessage{};
        const int executed = ::sqlite3_exec(
            database_, terminated.c_str(), nullptr, nullptr, &nativeMessage);
        const std::string message = nativeMessage != nullptr
            ? std::string{nativeMessage}
            : std::string{::sqlite3_errstr(executed)};
        if (nativeMessage != nullptr) {
            ::sqlite3_free(nativeMessage);
        }
        require(executed == SQLITE_OK,
                "the raw project corruption fixture failed: " + message);
    }

    void close()
    {
        const int closed = ::sqlite3_close(database_);
        require(closed == SQLITE_OK,
                "the raw project corruption fixture could not close cleanly");
        database_ = nullptr;
    }

private:
    explicit RawSqliteDatabase(sqlite3* database) noexcept
        : database_{database}
    {
    }

    sqlite3* database_{};
};

void createProjectForeignKeyViolation(
    const std::filesystem::path& directory,
    StoreDependencies& dependencies)
{
    constexpr std::string_view SetupOperationId =
        "71000000-0000-4000-8000-000000000003";
    const auto setupContext = recoveryContext(
        SetupOperationId, "p07-integrity-recovery-project-setup");
    auto setup = take(PersistenceDetail::WindowsDatabaseStore::open(
        PersistenceSupport::pathText(canonicalDirectory(directory)),
        CentralMainBasename,
        CentralMigrationLockBasename,
        dependencies.diagnostics,
        dependencies.clock,
        PersistenceDetail::WindowsDatabaseStoreOptions{
            PersistenceMigrations::DatabaseKind::Project,
            PersistenceDetail::WinsqliteSynchronousMode::Full,
            false,
            nullptr},
        setupContext));
    take(setup->close(setupContext));
    setup.reset();

    const std::filesystem::path mainPath =
        canonicalDirectory(directory) / CentralMainBasename;
    auto raw = RawSqliteDatabase::open(mainPath);
    raw->retainWalOnClose();
    raw->execute(
        "PRAGMA foreign_keys=OFF;"
        "PRAGMA journal_mode=WAL;"
        "INSERT INTO memory_record_tags(record_id, tag_id) "
        "VALUES('missing-record', 9001);");
    raw->close();

    const std::filesystem::path walPath = sidecarPath(mainPath, L"-wal");
    const std::filesystem::path sharedMemoryPath = sidecarPath(mainPath, L"-shm");
    require(std::filesystem::is_regular_file(walPath) &&
                std::filesystem::file_size(walPath) > 32U &&
                std::filesystem::is_regular_file(sharedMemoryPath) &&
                std::filesystem::file_size(sharedMemoryPath) > 0U,
            "the connected-recovery fixture did not retain WAL and shared memory");
}

void verifyCommittedRecoveryEvidence(
    const std::filesystem::path& directory,
    const std::string_view operationId,
    const CapturedCohort& expectedCohort,
    const Domain::Error& error)
{
    const std::filesystem::path sourcePath =
        canonicalDirectory(directory) / CentralMainBasename;
    const std::filesystem::path evidencePath =
        quarantinePathFor(directory, operationId);
    const std::filesystem::path manifestPath =
        sidecarPath(evidencePath, L".manifest.json");
    const std::string evidenceId =
        "p07-quarantine:" + std::string{operationId};

    require(error.code == Domain::ErrorCodes::IntegrityFailure,
            "integrity recovery replaced the primary integrity-failure code");
    require(error.evidenceId.has_value() && error.evidenceId.value() == evidenceId,
            "integrity recovery did not report its committed quarantine evidence");
    require(error.message.find("Integrity recovery also failed:") ==
                std::string::npos,
            "integrity recovery appended a secondary recovery failure");
    require(error.message.find(PersistenceSupport::pathText(evidencePath).value()) !=
                std::string::npos &&
                error.message.find(PersistenceSupport::pathText(manifestPath).value()) !=
                    std::string::npos,
            "integrity recovery did not report both retained evidence paths");
    require(std::filesystem::is_regular_file(manifestPath),
            "integrity recovery did not retain its committed manifest");

    const auto manifest = nlohmann::json::parse(
        PersistenceSupport::readFixture(manifestPath, 64U * 1024U));
    require(manifest.at("schema_version").get<int>() == 1 &&
                manifest.at("evidence_id").get<std::string>() == evidenceId,
            "integrity recovery retained a manifest with the wrong identity");
    const auto& files = manifest.at("files");
    std::size_t expectedFileCount{};
    for (const auto& role : expectedCohort) {
        if (role.has_value()) {
            ++expectedFileCount;
        }
    }
    require(files.is_array() && files.size() == expectedFileCount,
            "integrity recovery retained the wrong cohort role set");
    for (std::size_t index = 0U; index < CohortRoles.size(); ++index) {
        const auto& role = CohortRoles[index];
        const std::filesystem::path originalPath = role.suffix.empty()
            ? sourcePath
            : sidecarPath(sourcePath, role.suffix);
        const std::filesystem::path retainedPath = role.suffix.empty()
            ? evidencePath
            : sidecarPath(evidencePath, role.suffix);
        require(!std::filesystem::exists(originalPath),
                "integrity recovery left a captured source role in service");
        if (!expectedCohort[index].has_value()) {
            require(!std::filesystem::exists(retainedPath),
                    "integrity recovery invented an absent evidence role");
            continue;
        }
        const std::string& expectedBytes = expectedCohort[index].value();
        require(PersistenceSupport::readFixture(
                    retainedPath, expectedBytes.size() + 1U) == expectedBytes,
                "integrity recovery changed a quarantined cohort role");
        const auto manifested = std::find_if(
            files.begin(), files.end(), [&](const nlohmann::json& file) {
                return file.at("role").get<std::string>() == role.name;
            });
        require(manifested != files.end(),
                "integrity recovery omitted a captured manifest role");
        require(manifested->at("path").get<std::string>() ==
                    PersistenceSupport::pathText(retainedPath).value() &&
                    manifested->at("bytes").get<std::uint64_t>() ==
                        expectedBytes.size() &&
                    manifested->at("sha256").get<std::string>() ==
                        sha256(expectedBytes),
                "integrity recovery retained a role with the wrong bytes or digest");
        require(!manifested->at("source_file_identity").get<std::string>().empty() &&
                    !manifested->at("file_identity").get<std::string>().empty(),
                "integrity recovery omitted source or evidence file identity");
    }
}

void testOriginalCancellationDoesNotAbortCommittedIntegrityRecovery()
{
    constexpr std::string_view OperationId =
        "71000000-0000-4000-8000-000000000001";
    constexpr std::string_view HostileBytes =
        "not-a-sqlite-database-cancellation-boundary";
    ScopedTestDirectory directory{L"integrity-recovery-cancellation"};
    const std::filesystem::path sourcePath =
        canonicalDirectory(directory.path()) / CentralMainBasename;
    writeFixture(sourcePath, HostileBytes);

    StoreDependencies dependencies;
    std::stop_source cancellation;
    auto context = recoveryContext(
        OperationId,
        "p07-integrity-recovery-cancellation",
        cancellation.get_token());
    CapturedCohort expectedCohort;
    IntegrityRecoveryObserver observer{[&] {
        expectedCohort = captureCohort(sourcePath);
        cancellation.request_stop();
    }};
    const auto opened = openCorruptStore(
        directory.path(), dependencies, observer,
        PersistenceMigrations::DatabaseKind::Central, context);
    observer.rethrowCallbackFailure();

    require(observer.callCount() == 1U && context.isCancellationRequested(),
            "the original operation was not cancelled at the integrity boundary");
    require(!opened,
            "the corrupt store opened successfully after integrity recovery");
    verifyCommittedRecoveryEvidence(
        directory.path(), OperationId, expectedCohort, opened.error());
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "cancelled-original recovery retained runtime database ownership");
}

void testOriginalDeadlineDoesNotAbortCommittedIntegrityRecovery()
{
    constexpr std::string_view OperationId =
        "71000000-0000-4000-8000-000000000002";
    ScopedTestDirectory directory{L"integrity-recovery-deadline"};
    const std::filesystem::path sourcePath =
        canonicalDirectory(directory.path()) / CentralMainBasename;

    StoreDependencies dependencies;
    createProjectForeignKeyViolation(directory.path(), dependencies);
    auto context = recoveryContext(
        OperationId, "p07-integrity-recovery-deadline");
    require(!context.isExpired(std::chrono::steady_clock::now()),
            "the original deadline was not active before store recovery began");
    CapturedCohort expectedCohort;
    IntegrityRecoveryObserver observer{[&] {
        expectedCohort = captureCohort(sourcePath);
        context.deadline = std::chrono::steady_clock::now() - 1ms;
    }};
    const auto opened = openCorruptStore(
        directory.path(), dependencies, observer,
        PersistenceMigrations::DatabaseKind::Project, context);
    observer.rethrowCallbackFailure();

    require(observer.callCount() == 1U &&
                context.isExpired(std::chrono::steady_clock::now()),
            "the original deadline did not become active at the integrity boundary");
    require(!opened,
            "the corrupt store opened successfully after integrity recovery");
    require(expectedCohort[1].has_value() && expectedCohort[2].has_value(),
            "connected recovery did not capture pre-close WAL and shared-memory bytes");
    verifyCommittedRecoveryEvidence(
        directory.path(), OperationId, expectedCohort, opened.error());
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "expired-original recovery retained runtime database ownership");
}

} // namespace

void registerIntegrityRecoveryTests(TestRegistry& tests)
{
    addTest(tests, "persistence.recovery.original-cancellation",
            testOriginalCancellationDoesNotAbortCommittedIntegrityRecovery);
    addTest(tests, "persistence.recovery.original-deadline",
            testOriginalDeadlineDoesNotAbortCommittedIntegrityRecovery);
}

} // namespace ForgeConductor::Tests
