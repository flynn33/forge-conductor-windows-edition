#include "PersistenceTestSupport.h"

#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"
#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;
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
constexpr std::string_view CentralLedger =
    "1:C001:6d34b6a07a3d74440b598f2ca8b73ce84b615f99b814911b0f23e517e77c3eeb|"
    "2:C002:3c6fed9dd5aad4cda6d1bf511c48bfb27e450b68cba7b9446e6ddc9ef0d60315|"
    "3:C003:600c16d28acd5f54a53a900d20e9ca51392a764e4bc9cdcb0b0b895a335173d9|"
    "4:C004:653de9cd69b5a570b2269304715742375958e80335fead0a708362a134328936|"
    "5:C005:e710c085f429574b82013d1bd5d711418147fdb15b91a1de7f74a83e14703cba|"
    "6:C006:2f4ebc81ba122ca1a471504ce69fad1b11e7cbeecedd972024a521ebc849c427";

const std::vector<std::string> CentralTables{
    "agent_sessions",
    "audit_events",
    "client_presence",
    "context_handoffs",
    "memory_notes",
    "presence",
    "schema_migrations",
    "schema_version"};

const std::vector<std::string> CentralIndexes{
    "idx_audit_events_event_id",
    "idx_audit_events_occurred_at",
    "idx_context_handoffs_client_sequence",
    "idx_context_handoffs_sequence",
    "idx_context_handoffs_updated"};

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
            std::string{vfs_->vfsName()}, openMode, WinsqliteSynchronousMode::Full,
            PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
            namespaceLease_};
    }

    [[nodiscard]] const std::wstring& databasePath() const noexcept
    {
        return namespaceLease_->canonicalMainDatabasePath();
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

struct CentralDependencies final {
    explicit CentralDependencies(const std::filesystem::path& directory)
    {
        const auto now = std::chrono::steady_clock::now();
        paths = std::make_shared<TestFakes::RecordingApplicationPathsFake>();
        paths->setNow(now);
        paths->dataRootResult.set(Domain::Result<Domain::PathText>::success(
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
            "central fixture no longer contains its documented foreign-key pragma");
    sql.erase(position, ForeignKeysPragma.size());
    return sql;
}

void createFixture(
    const std::filesystem::path& directory,
    const std::filesystem::path& fixture)
{
    const auto context = activeContext("p07-central-create-fixture");
    auto environment = KernelEnvironment::create(directory, L"store.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadWriteCreate, context);
    take(connection.execute(fixtureSql(fixture), context));
    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "central fixture creation leaked a VFS file owner");
}

[[nodiscard]] std::int64_t queryInteger(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == WinsqliteStepResult::Row,
            "central integer query returned no row");
    const std::int64_t value = take(statement.columnInt64(0));
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "central integer query returned more than one row");
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
            "central text query returned no row");
    auto value = take(statement.columnText(0, maximumBytes));
    require(value.has_value(), "central text query unexpectedly returned null");
    std::string copied = std::move(value).value();
    require(take(statement.step()) == WinsqliteStepResult::Done,
            "central text query returned more than one row");
    return copied;
}

[[nodiscard]] std::string queryPlanDetails(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    std::string details;
    for (std::size_t row = 0U; row < 16U; ++row) {
        const WinsqliteStepResult stepped = take(statement.step());
        if (stepped == WinsqliteStepResult::Done) {
            return details;
        }
        require(stepped == WinsqliteStepResult::Row,
                "central query plan returned an invalid step result");
        auto detail = take(statement.columnText(3, 1024U));
        require(detail.has_value(), "central query plan returned a null detail");
        if (!details.empty()) {
            details.push_back('|');
        }
        details += *detail;
    }
    require(false, "central query plan exceeded its 16-row bound");
    return {};
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

[[nodiscard]] std::unique_ptr<PersistenceWindows::WindowsCentralDatabase> openCentral(
    CentralDependencies& dependencies,
    const Domain::OperationContext& context)
{
    return take(PersistenceWindows::WindowsCentralDatabase::open(
        dependencies.paths, dependencies.diagnostics, dependencies.clock, context));
}

void requireCurrentSnapshot(
    PersistenceWindows::WindowsCentralDatabase& database,
    const Domain::OperationContext& context)
{
    const auto snapshot = take(database.schemaSnapshot(context));
    require(snapshot.kind == PersistenceWindows::DatabaseStoreKind::Central,
            "central facade reported the wrong database kind");
    require(snapshot.physicalVersion == 6 &&
                snapshot.sourceCompatibilityVersion == 5,
            "central facade reported the wrong schema versions");
    require(!snapshot.fts5Enabled,
            "central facade reported a project FTS schema");
    require(snapshot.tables == CentralTables,
            "central facade did not expose the exact target table set");
    require(snapshot.indexes == CentralIndexes,
            "central facade did not expose the exact target index set");
    require(snapshot.triggers.empty(),
            "central facade exposed an unexpected trigger");
    take(database.quickCheck(context));
}

void requireCurrentLedgerAndConstraints(
    const std::filesystem::path& directory,
    const std::string_view expectedContextColumns)
{
    const auto context = activeContext("p07-central-inspection");
    auto environment = KernelEnvironment::create(directory, L"store.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);

    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM schema_version WHERE version = 6;",
                         context) == 1,
            "central target does not have exactly one version-6 marker");
    require(queryInteger(connection, "SELECT COUNT(*) FROM schema_version;", context) == 1,
            "central target retained an ambiguous version ledger");
    require(queryText(
                connection,
                "SELECT group_concat(version || ':' || identifier || ':' || "
                "content_sha256, '|') FROM (SELECT * FROM schema_migrations "
                "ORDER BY version);",
                context) == CentralLedger,
            "central migration ledger identifiers or checksums changed");
    require(queryInteger(connection,
                         "SELECT COUNT(DISTINCT applied_at) FROM schema_migrations;",
                         context) == 1 &&
                queryText(connection,
                          "SELECT MIN(applied_at) FROM schema_migrations;",
                          context) == AppliedAt,
            "central migration ledger did not retain the injected applied-at text");

    require(queryText(connection, columnSignatureSql("schema_migrations"), context) ==
                "0:version:INTEGER:0:<null>:1:0|1:identifier:TEXT:1:<null>:0:0|"
                "2:applied_at:TEXT:1:<null>:0:0|3:content_sha256:TEXT:1:<null>:0:0",
            "central migration-ledger columns changed");
    require(queryText(connection, columnSignatureSql("agent_sessions"), context) ==
                "0:id:TEXT:0:<null>:1:0|1:agent_id:TEXT:1:<null>:0:0|"
                "2:client_id:TEXT:0:<null>:0:0|3:status:TEXT:1:<null>:0:0|"
                "4:summary:TEXT:0:<null>:0:0|5:created_at:TEXT:1:<null>:0:0|"
                "6:updated_at:TEXT:1:<null>:0:0|7:project_id:TEXT:0:<null>:0:0|"
                "8:goal:TEXT:0:<null>:0:0|9:cwd:TEXT:0:<null>:0:0|"
                "10:report_json:TEXT:0:<null>:0:0",
            "central agent-session columns changed");
    require(queryText(connection, columnSignatureSql("context_handoffs"), context) ==
                expectedContextColumns,
            "central handoff columns changed or were reordered");
    require(queryText(connection, columnSignatureSql("audit_events"), context) ==
                "0:id:INTEGER:0:<null>:1:0|1:timestamp:TEXT:1:<null>:0:0|"
                "2:client_id:TEXT:0:<null>:0:0|3:tool:TEXT:1:<null>:0:0|"
                "4:args_digest:TEXT:0:<null>:0:0|5:args_json:TEXT:0:<null>:0:0|"
                "6:status:TEXT:0:<null>:0:0|7:duration_ms:INTEGER:0:<null>:0:0|"
                "8:error:TEXT:0:<null>:0:0|9:event_id:TEXT:0:<null>:0:0|"
                "10:occurred_at:TEXT:0:<null>:0:0|11:arguments_json:TEXT:0:<null>:0:0|"
                "12:error_code:TEXT:0:<null>:0:0|13:mutating:INTEGER:0:<null>:0:0",
            "central audit-event columns changed");
    require(queryText(connection, columnSignatureSql("client_presence"), context) ==
                "0:client_id:TEXT:0:<null>:1:0|1:role:TEXT:1:<null>:0:0|"
                "2:deployment_id:TEXT:0:<null>:0:0|3:process_id:INTEGER:0:<null>:0:0|"
                "4:first_seen_at:TEXT:1:<null>:0:0|5:last_seen_at:TEXT:1:<null>:0:0",
            "central client-presence columns changed");

    require(queryText(connection,
                      "SELECT [unique] || ':' || origin || ':' || partial FROM "
                      "pragma_index_list('audit_events') WHERE "
                      "name = 'idx_audit_events_event_id';",
                      context) == "1:c:1" &&
                queryText(connection,
                          indexKeySql("idx_audit_events_event_id"), context) ==
                    "event_id:0",
            "central event-id index lost its unique partial property");
    require(queryText(connection,
                      indexKeySql("idx_context_handoffs_client_sequence"), context) ==
                "client_id:0|write_sequence:1",
            "central client-sequence index columns changed");
    require(queryText(connection,
                      indexKeySql("idx_audit_events_occurred_at"), context) ==
                "occurred_at:1",
            "central audit ordering index columns changed");
    const std::string auditOrderingPlan = queryPlanDetails(
        connection,
        "EXPLAIN QUERY PLAN "
        "SELECT id FROM audit_events ORDER BY occurred_at DESC LIMIT 8;",
        context);
    require(auditOrderingPlan.find("idx_audit_events_occurred_at") != std::string::npos,
            "central audit ordering query did not use its occurred-at index");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM pragma_index_list('schema_migrations') "
                         "WHERE [unique] = 1 AND origin = 'u';",
                         context) == 1,
            "central ledger identifier UNIQUE constraint changed");
    require(queryInteger(connection,
                         "SELECT instr(sql, 'CHECK (mutating IN (0, 1))') FROM "
                         "sqlite_schema WHERE type = 'table' AND name = 'audit_events';",
                         context) > 0,
            "central audit mutating CHECK constraint changed");
    require(queryInteger(connection,
                         "SELECT (SELECT COUNT(*) FROM "
                         "pragma_foreign_key_list('agent_sessions')) + "
                         "(SELECT COUNT(*) FROM "
                         "pragma_foreign_key_list('context_handoffs'));",
                         context) == 0,
            "central target gained an unexpected foreign key");
    require(queryText(connection, "PRAGMA main.quick_check(1);", context) == "ok",
            "central target failed its independent quick-check");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM pragma_foreign_key_check;", context) == 0,
            "central target failed its independent foreign-key check");

    take(connection.close(context));
    require(environment->openFileCount() == 0U,
            "central inspection leaked a VFS file owner");
}

constexpr std::string_view CurrentContextColumns =
    "0:id:TEXT:0:<null>:1:0|1:created_at:TEXT:1:<null>:0:0|"
    "2:updated_at:TEXT:1:<null>:0:0|3:source:TEXT:1:<null>:0:0|"
    "4:resume_ready:INTEGER:1:0:0:0|5:packet_json:TEXT:1:<null>:0:0|"
    "6:write_sequence:INTEGER:1:0:0:0|7:client_id:TEXT:0:<null>:0:0|"
    "8:project_id:TEXT:0:<null>:0:0|9:session_id:TEXT:0:<null>:0:0|"
    "10:payload_json:TEXT:0:<null>:0:0|11:content_sha256:TEXT:0:<null>:0:0";

constexpr std::string_view AdoptedVersion5ContextColumns =
    "0:id:TEXT:0:<null>:1:0|1:created_at:TEXT:1:<null>:0:0|"
    "2:updated_at:TEXT:1:<null>:0:0|3:source:TEXT:1:<null>:0:0|"
    "4:resume_ready:INTEGER:1:0:0:0|5:packet_json:TEXT:1:<null>:0:0|"
    "6:client_id:TEXT:0:<null>:0:0|7:write_sequence:INTEGER:1:0:0:0|"
    "8:project_id:TEXT:0:<null>:0:0|9:session_id:TEXT:0:<null>:0:0|"
    "10:payload_json:TEXT:0:<null>:0:0|11:content_sha256:TEXT:0:<null>:0:0";

void testFreshCentralSchemaAndIdempotentReopen()
{
    ScopedTestDirectory directory{L"central-fresh"};
    const auto context = activeContext("p07-central-fresh");
    CentralDependencies dependencies{directory.path()};

    {
        auto database = openCentral(dependencies, context);
        requireCurrentSnapshot(*database, context);
        take(database->close(context));
    }
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "fresh central close retained runtime ownership");
    requireCurrentLedgerAndConstraints(directory.path(), CurrentContextColumns);

    const auto databasePath = directory.path() / L"store.sqlite";
    const std::string beforeReopen = PersistenceSupport::readFixture(databasePath);
    {
        auto database = openCentral(dependencies, context);
        requireCurrentSnapshot(*database, context);
        take(database->close(context));
    }
    requireCurrentLedgerAndConstraints(directory.path(), CurrentContextColumns);
    require(PersistenceSupport::readFixture(databasePath) == beforeReopen,
            "idempotent central reopen changed the main database bytes");
}

void testCentralVersion3MigrationPreservesData(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"central-v3"};
    createFixture(directory.path(), fixtures / L"central-v3.sql");
    const auto context = activeContext("p07-central-v3");
    CentralDependencies dependencies{directory.path()};
    {
        auto database = openCentral(dependencies, context);
        requireCurrentSnapshot(*database, context);
        take(database->close(context));
    }

    const auto backupPath = directory.path() /
        L"store.sqlite.pre-migration.11111111-1111-4111-8111-111111111111.sqlite";
    require(std::filesystem::is_regular_file(backupPath),
            "central v3 migration did not publish its required online backup");
    requireCurrentLedgerAndConstraints(directory.path(), CurrentContextColumns);

    auto environment = KernelEnvironment::create(directory.path(), L"store.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM context_handoffs WHERE "
                         "id = 'legacy-v3-handoff' AND write_sequence > 0 AND "
                         "client_id IS NULL AND project_id IS NULL AND session_id IS NULL "
                         "AND content_sha256 IS NULL AND payload_json = packet_json AND "
                         "instr(packet_json, 'central-v3-handoff-sentinel') > 0;",
                         context) == 1,
            "central v3 handoff was not preserved and backfilled exactly");
    take(connection.close(context));
}

void testMinimalCentralVersion3MigrationPreservesData(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"central-v3-minimal"};
    createFixture(directory.path(), fixtures / L"central-v3-minimal.sql");
    const auto context = activeContext("p07-central-v3-minimal");
    CentralDependencies dependencies{directory.path()};
    {
        auto database = openCentral(dependencies, context);
        requireCurrentSnapshot(*database, context);
        take(database->close(context));
    }

    const auto backupPath = directory.path() /
        L"store.sqlite.pre-migration.11111111-1111-4111-8111-111111111111.sqlite";
    require(std::filesystem::is_regular_file(backupPath),
            "minimal central v3 migration did not publish its required online backup");
    {
        auto backupEnvironment = KernelEnvironment::create(
            directory.path(), backupPath.filename().native());
        const auto backupContext = activeContext("p07-central-v3-minimal-backup");
        auto backup = openDatabase(
            *backupEnvironment, WinsqliteOpenMode::ReadOnlyExisting, backupContext);
        require(queryInteger(backup,
                             "SELECT COUNT(*) FROM sqlite_schema WHERE "
                             "type IN ('table', 'index', 'trigger', 'view') AND "
                             "substr(name, 1, 7) <> 'sqlite_';",
                             backupContext) == 2,
                "minimal central v3 backup gained a non-source user object");
        require(queryInteger(backup,
                             "SELECT COUNT(*) FROM schema_version WHERE version = 3;",
                             backupContext) == 1 &&
                    queryInteger(backup,
                                 "SELECT COUNT(*) FROM context_handoffs WHERE "
                                 "id = 'legacy-v3-handoff' AND "
                                 "instr(packet_json, "
                                 "'central-v3-minimal-handoff-sentinel') > 0;",
                                 backupContext) == 1,
                "minimal central v3 backup did not retain the exact source generation");
        take(backup.close(backupContext));
    }
    requireCurrentLedgerAndConstraints(directory.path(), CurrentContextColumns);

    auto environment = KernelEnvironment::create(directory.path(), L"store.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM context_handoffs WHERE "
                         "id = 'legacy-v3-handoff' AND write_sequence > 0 AND "
                         "client_id IS NULL AND project_id IS NULL AND session_id IS NULL "
                         "AND content_sha256 IS NULL AND payload_json = packet_json AND "
                         "instr(packet_json, 'central-v3-minimal-handoff-sentinel') > 0;",
                         context) == 1,
            "minimal central v3 handoff was not preserved and backfilled exactly");
    require(queryInteger(connection,
                         "SELECT (SELECT COUNT(*) FROM memory_notes) + "
                         "(SELECT COUNT(*) FROM agent_sessions) + "
                         "(SELECT COUNT(*) FROM presence) + "
                         "(SELECT COUNT(*) FROM audit_events);",
                         context) == 0,
            "minimal central v3 migration did not create empty missing core tables");
    take(connection.close(context));
    environment.reset();

    const auto databasePath = directory.path() / L"store.sqlite";
    const std::string beforeReopen = PersistenceSupport::readFixture(databasePath);
    CentralDependencies reopenedDependencies{directory.path()};
    {
        auto database = openCentral(reopenedDependencies, context);
        requireCurrentSnapshot(*database, context);
        take(database->close(context));
    }
    require(PersistenceSupport::readFixture(databasePath) == beforeReopen,
            "minimal central v3 idempotent reopen changed the main database bytes");
}

void testMinimalCentralVersion3MigrationIsConcurrentAndIdempotent(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"central-v3-minimal-concurrent"};
    createFixture(directory.path(), fixtures / L"central-v3-minimal.sql");

    std::array<std::string, 2> failures;
    {
        std::barrier start{3};
        const auto initialize = [&](const std::size_t index,
                                    const std::string_view correlation) noexcept {
            try {
                CentralDependencies dependencies{directory.path()};
                const auto context = activeContext(correlation);
                start.arrive_and_wait();
                auto opened = PersistenceWindows::WindowsCentralDatabase::open(
                    dependencies.paths, dependencies.diagnostics,
                    dependencies.clock, context);
                if (!opened) {
                    failures[index] = opened.error().code + ": " +
                                      opened.error().message;
                    return;
                }
                auto database = std::move(opened).value();
                auto closed = database->close(context);
                if (!closed) {
                    failures[index] = closed.error().code + ": " +
                                      closed.error().message;
                }
            } catch (const std::exception& exception) {
                failures[index] = exception.what();
            } catch (...) {
                failures[index] = "unknown initializer failure";
            }
        };

        std::jthread first{initialize, 0U, "p07-central-v3-minimal-a"};
        std::jthread second{initialize, 1U, "p07-central-v3-minimal-b"};
        start.arrive_and_wait();
    }
    require(failures[0].empty() && failures[1].empty(),
            "two minimal central-v3 initializers did not migrate idempotently: " +
                failures[0] + " " + failures[1]);
    requireCurrentLedgerAndConstraints(directory.path(), CurrentContextColumns);
}

void testCentralVersion5MigrationPreservesData(
    const std::filesystem::path& fixtures)
{
    ScopedTestDirectory directory{L"central-v5"};
    createFixture(directory.path(), fixtures / L"central-v5.sql");
    const auto context = activeContext("p07-central-v5");
    CentralDependencies dependencies{directory.path()};
    {
        auto database = openCentral(dependencies, context);
        requireCurrentSnapshot(*database, context);
        take(database->close(context));
    }

    const auto backupPath = directory.path() /
        L"store.sqlite.pre-migration.11111111-1111-4111-8111-111111111111.sqlite";
    require(std::filesystem::is_regular_file(backupPath),
            "central v5 migration did not publish its required online backup");
    requireCurrentLedgerAndConstraints(
        directory.path(), AdoptedVersion5ContextColumns);

    auto environment = KernelEnvironment::create(directory.path(), L"store.sqlite");
    auto connection = openDatabase(
        *environment, WinsqliteOpenMode::ReadOnlyExisting, context);
    require(queryText(connection,
                      "SELECT body FROM memory_notes WHERE key = 'legacy/note';",
                      context) == "central-v5-preservation-sentinel",
            "central v5 memory note was not preserved");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM agent_sessions WHERE "
                         "id = 'legacy-session-1' AND project_id IS NULL AND goal IS NULL "
                         "AND cwd IS NULL AND report_json IS NULL;",
                         context) == 1,
            "central v5 agent session was not preserved with null new fields");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM client_presence WHERE "
                         "client_id = 'legacy-client-1' AND role = 'cli' AND "
                         "process_id = 4242 AND first_seen_at = '2025-01-02T03:04:09Z' "
                         "AND last_seen_at = first_seen_at;",
                         context) == 1,
            "central v5 presence row was not copied into client_presence");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM audit_events WHERE id = 7 AND "
                         "timestamp = '2025-01-02T03:04:10Z' AND occurred_at = timestamp "
                         "AND arguments_json = args_json AND event_id IS NULL AND "
                         "error_code IS NULL AND mutating IS NULL;",
                         context) == 1,
            "central v5 audit row was not preserved and backfilled");
    require(queryInteger(connection,
                         "SELECT COUNT(*) FROM context_handoffs WHERE "
                         "id = 'legacy-handoff-1' AND client_id = 'legacy-client-1' "
                         "AND write_sequence = 41 AND payload_json = packet_json AND "
                         "instr(payload_json, 'central-v5-handoff-sentinel') > 0 "
                         "AND project_id IS NULL AND session_id IS NULL AND "
                         "content_sha256 IS NULL;",
                         context) == 1,
            "central v5 handoff was not preserved and backfilled");
    take(connection.close(context));
}

void requireRejectedWithoutMainMutation(
    const std::filesystem::path& directory,
    const std::string_view expectedCode,
    const std::string_view message)
{
    const auto databasePath = directory / L"store.sqlite";
    const std::string before = PersistenceSupport::readFixture(databasePath);
    const auto context = activeContext("p07-central-rejection");
    CentralDependencies dependencies{directory};
    const auto opened = PersistenceWindows::WindowsCentralDatabase::open(
        dependencies.paths, dependencies.diagnostics, dependencies.clock, context);
    requireError(opened, expectedCode, message);
    require(PersistenceSupport::readFixture(databasePath) == before, message);
    require(dependencies.diagnostics->activeOwnership(
                Contracts::RuntimeOwnerKind::OpenDatabase) == 0U,
            "rejected central open retained runtime ownership");
}

void testCentralRejectsUnsupportedFutureAndAmbiguousLayouts(
    const std::filesystem::path& fixtures)
{
    {
        ScopedTestDirectory directory{L"central-unsupported-v4"};
        createFixture(directory.path(), fixtures / L"central-v3.sql");
        const auto context = activeContext("p07-central-create-v4");
        auto environment = KernelEnvironment::create(directory.path(), L"store.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute("UPDATE schema_version SET version = 4;", context));
        take(connection.close(context));
        environment.reset();
        requireRejectedWithoutMainMutation(
            directory.path(), Domain::ErrorCodes::UnsupportedVersion,
            "unsupported central v4 was not rejected without changing the main file");
    }
    {
        ScopedTestDirectory directory{L"central-future"};
        createFixture(directory.path(), fixtures / L"central-v5.sql");
        const auto context = activeContext("p07-central-create-future");
        auto environment = KernelEnvironment::create(directory.path(), L"store.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute("UPDATE schema_version SET version = 7;", context));
        take(connection.close(context));
        environment.reset();
        requireRejectedWithoutMainMutation(
            directory.path(), Domain::ErrorCodes::UnsupportedVersion,
            "future central schema was not rejected without changing the main file");
    }
    {
        ScopedTestDirectory directory{L"central-ambiguous"};
        createFixture(directory.path(), fixtures / L"central-v5.sql");
        const auto context = activeContext("p07-central-create-ambiguous");
        auto environment = KernelEnvironment::create(directory.path(), L"store.sqlite");
        auto connection = openDatabase(
            *environment, WinsqliteOpenMode::ReadWriteExisting, context);
        take(connection.execute(
            "CREATE TABLE foreign_extra(value TEXT NOT NULL);", context));
        take(connection.close(context));
        environment.reset();
        requireRejectedWithoutMainMutation(
            directory.path(), Domain::ErrorCodes::MigrationFailed,
            "ambiguous central schema was not rejected without changing the main file");
    }
}

} // namespace

void registerCentralMigrationTests(
    TestRegistry& tests,
    const std::filesystem::path& fixtures)
{
    addTest(tests, "persistence.central.fresh-idempotent",
            testFreshCentralSchemaAndIdempotentReopen);
    addTest(tests, "persistence.central.v3-preservation",
            [fixtures] { testCentralVersion3MigrationPreservesData(fixtures); });
    addTest(tests, "persistence.central.v3-minimal-preservation",
            [fixtures] {
                testMinimalCentralVersion3MigrationPreservesData(fixtures);
            });
    addTest(tests, "persistence.central.v3-minimal-concurrent-idempotent",
            [fixtures] {
                testMinimalCentralVersion3MigrationIsConcurrentAndIdempotent(fixtures);
            });
    addTest(tests, "persistence.central.v5-preservation",
            [fixtures] { testCentralVersion5MigrationPreservesData(fixtures); });
    addTest(tests, "persistence.central.reject-read-only",
            [fixtures] {
                testCentralRejectsUnsupportedFutureAndAmbiguousLayouts(fixtures);
            });
}

} // namespace ForgeConductor::Tests
