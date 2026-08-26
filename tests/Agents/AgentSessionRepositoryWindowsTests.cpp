#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"
#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace InfrastructureDetail =
    ForgeConductor::Infrastructure::Windows::Detail;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

using namespace std::chrono_literals;

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::PathText pathText(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::UtcTimePoint utcTime(
    const int year,
    const unsigned month,
    const unsigned day,
    const int hour,
    const int minute,
    const int second)
{
    const auto date = std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::month{month} /
        std::chrono::day{day}};
    return Domain::UtcTimePoint{
        date.time_since_epoch() + std::chrono::hours{hour} +
        std::chrono::minutes{minute} + std::chrono::seconds{second}};
}

class MutableClock final : public Contracts::IClock {
public:
    MutableClock(
        const Domain::UtcTimePoint utc,
        const Domain::MonotonicTimePoint monotonic) noexcept
        : utc_{utc}, monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return monotonic_;
    }

    template <typename Rep, typename Period>
    void advance(const std::chrono::duration<Rep, Period> amount) noexcept
    {
        std::lock_guard lock{mutex_};
        utc_ += amount;
        monotonic_ += amount;
    }

private:
    mutable std::mutex mutex_;
    Domain::UtcTimePoint utc_;
    Domain::MonotonicTimePoint monotonic_;
};

[[nodiscard]] Domain::OperationContext activeContext(
    const MutableClock& clock,
    const std::string_view correlation,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("b1000000-0000-4000-8000-000000000001"),
        clock.monotonicNow() + 2min,
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::OperationContext expiredContext(
    const MutableClock& clock,
    const std::string_view correlation)
{
    static_cast<void>(clock);
    return Domain::OperationContext{
        parse<Domain::OperationId>("b1000000-0000-4000-8000-000000000002"),
        std::chrono::steady_clock::now() - 1ms,
        {},
        parse<Domain::CorrelationId>(correlation)};
}

struct RepositoryFixture final {
    explicit RepositoryFixture(
        const std::filesystem::path& directory,
        const Domain::UtcTimePoint initialUtc =
            utcTime(2026, 8U, 26U, 12, 0, 0))
        : directory{directory},
          clock{std::make_shared<MutableClock>(
              initialUtc, std::chrono::steady_clock::now())},
          paths{std::make_shared<Fakes::RecordingApplicationPathsFake>()},
          diagnostics{std::make_shared<Fakes::RuntimeDiagnosticsFake>(
              clock->monotonicNow())}
    {
        paths->setNow(clock->monotonicNow());
        paths->dataRootResult.set(Domain::Result<Domain::PathText>::success(
            Support::pathText(directory)));
        open("p10-agent-repository-open");
    }

    ~RepositoryFixture() noexcept
    {
        close();
    }

    void open(const std::string_view correlation)
    {
        require(!repository, "repository is already open");
        repository = take(PersistenceWindows::WindowsAgentSessionRepository::open(
            paths,
            diagnostics,
            clock,
            activeContext(*clock, correlation)));
    }

    void close() noexcept
    {
        if (repository) {
            repository->close();
            repository.reset();
        }
    }

    std::filesystem::path directory;
    std::shared_ptr<MutableClock> clock;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<PersistenceWindows::WindowsAgentSessionRepository> repository;
};

class SqliteDatabase final {
public:
    SqliteDatabase(const std::filesystem::path& path, const bool create)
    {
        const auto encoded = Support::pathText(path).value();
        sqlite3* opened{};
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_NOFOLLOW | (create ? SQLITE_OPEN_CREATE : 0);
        const int status = sqlite3_open_v2(encoded.c_str(), &opened, flags, nullptr);
        if (status != SQLITE_OK) {
            const std::string message = opened ? sqlite3_errmsg(opened) :
                "Winsqlite returned no database handle";
            if (opened) {
                static_cast<void>(sqlite3_close_v2(opened));
            }
            throw std::runtime_error{"Could not open test database: " + message};
        }
        database_ = opened;
    }

    ~SqliteDatabase() noexcept
    {
        if (database_) {
            static_cast<void>(sqlite3_close_v2(database_));
        }
    }

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    void execute(const std::string_view sql)
    {
        char* error{};
        const int status = sqlite3_exec(
            database_, std::string{sql}.c_str(), nullptr, nullptr, &error);
        if (status != SQLITE_OK) {
            const std::string message = error ? error : "unknown Winsqlite error";
            if (error) {
                sqlite3_free(error);
            }
            throw std::runtime_error{"Test SQL failed: " + message};
        }
    }

    [[nodiscard]] std::int64_t integer(const std::string_view sql)
    {
        sqlite3_stmt* statement{};
        if (sqlite3_prepare_v2(
                database_, std::string{sql}.c_str(), -1, &statement, nullptr) !=
            SQLITE_OK) {
            throw std::runtime_error{"Could not prepare integer query."};
        }
        const int stepped = sqlite3_step(statement);
        const auto value = stepped == SQLITE_ROW
            ? sqlite3_column_int64(statement, 0)
            : (std::numeric_limits<std::int64_t>::min)();
        static_cast<void>(sqlite3_finalize(statement));
        if (stepped != SQLITE_ROW) {
            throw std::runtime_error{"Integer query returned no row."};
        }
        return value;
    }

    [[nodiscard]] std::optional<std::string> optionalText(
        const std::string_view sql)
    {
        sqlite3_stmt* statement{};
        if (sqlite3_prepare_v2(
                database_, std::string{sql}.c_str(), -1, &statement, nullptr) !=
            SQLITE_OK) {
            throw std::runtime_error{"Could not prepare text query."};
        }
        const int stepped = sqlite3_step(statement);
        if (stepped == SQLITE_DONE) {
            static_cast<void>(sqlite3_finalize(statement));
            return std::nullopt;
        }
        if (stepped != SQLITE_ROW || sqlite3_column_type(statement, 0) == SQLITE_NULL) {
            static_cast<void>(sqlite3_finalize(statement));
            throw std::runtime_error{"Text query returned an invalid row."};
        }
        const auto* const data = sqlite3_column_text(statement, 0);
        const int bytes = sqlite3_column_bytes(statement, 0);
        if (data == nullptr || bytes < 0) {
            static_cast<void>(sqlite3_finalize(statement));
            throw std::runtime_error{"Text query returned invalid UTF-8 storage."};
        }
        std::string value{
            reinterpret_cast<const char*>(data), static_cast<std::size_t>(bytes)};
        static_cast<void>(sqlite3_finalize(statement));
        return value;
    }

private:
    sqlite3* database_{};
};

[[nodiscard]] std::string runProjectionKey(const Domain::SessionId& sessionId)
{
    return "agent_run/" + sessionId.value();
}

[[nodiscard]] std::string activeProjectionKey(const Domain::ClientId& clientId)
{
    return "agent_active/" + clientId.value();
}

[[nodiscard]] std::optional<std::string> projectionBody(
    SqliteDatabase& database,
    const std::string_view key)
{
    return database.optionalText(
        "SELECT body FROM memory_notes WHERE key='" + std::string{key} + "'");
}

[[nodiscard]] std::string requiredProjection(
    SqliteDatabase& database,
    const std::string_view key)
{
    const auto body = projectionBody(database, key);
    require(body.has_value(), "required agent projection is absent");
    require(body->size() >= 2U && body->front() == '{' && body->back() == '}',
            "agent projection is not a JSON object");
    return *body;
}

[[nodiscard]] std::string quotedProjectionText(const std::string_view value)
{
    std::string encoded{"\""};
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            encoded += "\\\"";
            break;
        case '\\':
            encoded += "\\\\";
            break;
        case '\b':
            encoded += "\\b";
            break;
        case '\f':
            encoded += "\\f";
            break;
        case '\n':
            encoded += "\\n";
            break;
        case '\r':
            encoded += "\\r";
            break;
        case '\t':
            encoded += "\\t";
            break;
        default:
            require(character >= 0x20U,
                    "test projection text contains an unsupported control byte");
            encoded.push_back(static_cast<char>(character));
            break;
        }
    }
    encoded.push_back('"');
    return encoded;
}

[[nodiscard]] std::string projectionTextArray(
    const std::vector<std::string>& values)
{
    std::string encoded{"["};
    bool first{true};
    for (const auto& value : values) {
        if (!first) {
            encoded.push_back(',');
        }
        first = false;
        encoded += quotedProjectionText(value);
    }
    encoded.push_back(']');
    return encoded;
}

void requireProjectionField(
    const std::string_view projection,
    const std::string_view key,
    const std::string_view expectedJson)
{
    const auto token = quotedProjectionText(key) + ":" +
        std::string{expectedJson};
    require(projection.find(token) != std::string_view::npos,
            "agent projection field does not match durable state");
}

void requireOptionalProjectionText(
    const std::string_view projection,
    const std::string_view key,
    const std::optional<std::string>& expected)
{
    const auto memberToken = quotedProjectionText(key) + ":";
    if (!expected) {
        require(projection.find(memberToken) == std::string_view::npos,
                "agent projection fabricated an optional text field");
        return;
    }
    requireProjectionField(projection, key, quotedProjectionText(*expected));
}

void requireRunProjectionMatches(
    SqliteDatabase& database,
    const Domain::AgentRunRecord& run)
{
    const auto projection = requiredProjection(
        database, runProjectionKey(run.session.id));
    requireProjectionField(
        projection, "session_id", quotedProjectionText(run.session.id.value()));
    requireProjectionField(
        projection, "agent_id", quotedProjectionText(run.session.agentId.value()));
    requireProjectionField(
        projection,
        "status",
        quotedProjectionText(Domain::wireName(run.session.status)));
    requireOptionalProjectionText(
        projection,
        "project_id",
        run.projectId ? std::optional<std::string>{run.projectId->value()} :
                        std::nullopt);
    requireOptionalProjectionText(projection, "goal", run.goal);
    requireOptionalProjectionText(
        projection,
        "cwd",
        run.workingDirectory
            ? std::optional<std::string>{run.workingDirectory->value()}
            : std::nullopt);
    requireProjectionField(
        projection, "output_schema", projectionTextArray(run.outputSchema));
    requireProjectionField(
        projection, "first_moves", projectionTextArray(run.firstMoves));
    require(projection.find("\"report\":") == std::string::npos,
            "run projection duplicated authoritative report JSON");
}

void requireRunProjectionAbsent(
    SqliteDatabase& database,
    const Domain::SessionId& sessionId)
{
    require(!projectionBody(database, runProjectionKey(sessionId)),
            "unexpected run projection survived transaction rollback");
}

void requireActiveProjectionMatches(
    SqliteDatabase& database,
    const Domain::ClientId& clientId,
    const Domain::ActiveBinding& binding)
{
    const auto projection = requiredProjection(
        database, activeProjectionKey(clientId));
    requireProjectionField(
        projection, "session_id", quotedProjectionText(binding.sessionId.value()));
    requireProjectionField(
        projection, "agent_id", quotedProjectionText(binding.agentId.value()));
    requireProjectionField(
        projection, "goal", quotedProjectionText(binding.goal));
    requireProjectionField(
        projection, "tools_primary", projectionTextArray(binding.toolsPrimary));
    requireProjectionField(
        projection, "tools_forbidden", projectionTextArray(binding.toolsForbidden));
    requireProjectionField(
        projection, "output_schema", projectionTextArray(binding.outputSchema));
    requireProjectionField(
        projection, "done_definition", projectionTextArray(binding.doneDefinition));
    requireOptionalProjectionText(
        projection,
        "cwd",
        binding.workingDirectory
            ? std::optional<std::string>{binding.workingDirectory->value()}
            : std::nullopt);
}

void requireActiveProjectionAbsent(
    SqliteDatabase& database,
    const Domain::ClientId& clientId)
{
    require(!projectionBody(database, activeProjectionKey(clientId)),
            "unexpected active projection survived the transaction");
}

[[nodiscard]] Domain::AgentRunStartMutation startMutation(
    const Domain::SessionId& sessionId,
    const Domain::ClientId& clientId,
    const Domain::UtcTimePoint now,
    const std::string_view goal = "implement durable agent sessions")
{
    const auto agentId = parse<Domain::AgentId>("implement");
    const auto projectId = parse<Domain::ProjectId>(
        "22000000-0000-4000-8000-000000000001");
    const auto cwd = pathText("D:/work/forge");
    Domain::AgentRunRecord run{
        Domain::AgentSession{
            sessionId,
            agentId,
            clientId,
            Domain::SessionStatus::Open,
            std::nullopt,
            now,
            now},
        projectId,
        std::string{goal},
        cwd,
        {"result", "evidence"},
        {"inspect", "implement"},
        std::nullopt};
    Domain::ActiveBinding binding{
        sessionId,
        agentId,
        std::string{goal},
        {"read", "write"},
        {"shell"},
        {"result", "evidence"},
        {"tests pass"},
        cwd};
    return Domain::AgentRunStartMutation{
        std::move(run),
        std::move(binding),
        Domain::makeAgentSupersedeSummary(
            "Closed because a new agent session started",
            std::optional<Domain::AgentId>{agentId},
            std::nullopt)};
}

[[nodiscard]] Domain::AgentRunStartPersistenceOutcome start(
    RepositoryFixture& fixture,
    const Domain::SessionId& sessionId,
    const Domain::ClientId& clientId,
    const std::string_view correlation,
    const std::string_view goal = "implement durable agent sessions")
{
    return take(fixture.repository->startRun(
        startMutation(sessionId, clientId, fixture.clock->utcNow(), goal),
        activeContext(*fixture.clock, correlation)));
}

void verifyRepeatedRecoveryIsStableAndNonMutating(
    RepositoryFixture& fixture,
    const Domain::ClientId& clientId,
    const Domain::AgentRunRecord& expectedRun,
    const Domain::ActiveBinding& expectedBinding)
{
    SqliteDatabase database{fixture.directory / L"store.sqlite", false};
    const auto activeKey = activeProjectionKey(clientId);
    const auto runKey = runProjectionKey(expectedRun.session.id);
    const auto activeBodyBefore = projectionBody(database, activeKey);
    const auto runBodyBefore = projectionBody(database, runKey);
    const auto activeUpdatedBefore = database.optionalText(
        "SELECT updated_at FROM memory_notes WHERE key='" + activeKey + "'");
    const auto runUpdatedBefore = database.optionalText(
        "SELECT updated_at FROM memory_notes WHERE key='" + runKey + "'");
    const auto rowUpdatedBefore = database.optionalText(
        "SELECT updated_at FROM agent_sessions WHERE id='" +
        expectedRun.session.id.value() + "'");
    const auto projectionCountBefore = database.integer(
        "SELECT COUNT(*) FROM memory_notes WHERE key='" + activeKey +
        "' OR key='" + runKey + "'");
    require(activeBodyBefore && runBodyBefore && activeUpdatedBefore &&
                runUpdatedBefore && rowUpdatedBefore &&
                projectionCountBefore == 2,
            "repeated recovery precondition is incomplete");

    constexpr std::size_t RepeatedRecoveryAttemptCount = 64U;
    for (std::size_t attempt{}; attempt < RepeatedRecoveryAttemptCount; ++attempt) {
        const auto recovered = take(fixture.repository->recoverRun(
            Domain::AgentRunRecoveryRequest{clientId},
            activeContext(*fixture.clock, "p10-recovery-repeat")));
        require(recovered.usedActiveProjection &&
                    !recovered.projectionNeedsRepair && recovered.run &&
                    recovered.binding,
                "repeated recovery changed its healthy outcome flags");
        const auto& run = *recovered.run;
        const auto& binding = *recovered.binding;
        require(run.session.id == expectedRun.session.id &&
                    run.session.agentId == expectedRun.session.agentId &&
                    run.session.clientId == expectedRun.session.clientId &&
                    run.session.status == expectedRun.session.status &&
                    run.session.summary == expectedRun.session.summary &&
                    run.session.createdAt == expectedRun.session.createdAt &&
                    run.session.updatedAt == expectedRun.session.updatedAt &&
                    run.projectId == expectedRun.projectId &&
                    run.goal == expectedRun.goal &&
                    run.workingDirectory == expectedRun.workingDirectory &&
                    run.outputSchema == expectedRun.outputSchema &&
                    run.firstMoves == expectedRun.firstMoves &&
                    run.reportJson == expectedRun.reportJson,
                "repeated recovery returned unstable durable run metadata");
        require(binding.sessionId == expectedBinding.sessionId &&
                    binding.agentId == expectedBinding.agentId &&
                    binding.goal == expectedBinding.goal &&
                    binding.toolsPrimary == expectedBinding.toolsPrimary &&
                    binding.toolsForbidden == expectedBinding.toolsForbidden &&
                    binding.outputSchema == expectedBinding.outputSchema &&
                    binding.doneDefinition == expectedBinding.doneDefinition &&
                    binding.workingDirectory == expectedBinding.workingDirectory,
                "repeated recovery returned unstable active binding metadata");
    }

    require(projectionBody(database, activeKey) == activeBodyBefore &&
                projectionBody(database, runKey) == runBodyBefore &&
                database.optionalText(
                    "SELECT updated_at FROM memory_notes WHERE key='" +
                    activeKey + "'") == activeUpdatedBefore &&
                database.optionalText(
                    "SELECT updated_at FROM memory_notes WHERE key='" +
                    runKey + "'") == runUpdatedBefore &&
                database.optionalText(
                    "SELECT updated_at FROM agent_sessions WHERE id='" +
                    expectedRun.session.id.value() + "'") == rowUpdatedBefore &&
                database.integer(
                    "SELECT COUNT(*) FROM memory_notes WHERE key='" + activeKey +
                    "' OR key='" + runKey + "'") == projectionCountBefore,
            "repeated recovery mutated durable row or projection state");
    take(fixture.repository->quickCheck(
        activeContext(*fixture.clock, "p10-recovery-repeat-quick-check")));
}

[[nodiscard]] std::wstring quoteArgument(const std::wstring_view value)
{
    std::wstring result{L"\""};
    std::size_t slashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2U + 1U, L'\\');
            result.push_back(L'\"');
            slashes = 0U;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0U;
        result.push_back(character);
    }
    result.append(slashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

struct ChildProcess final {
    InfrastructureDetail::UniqueHandle process;
    InfrastructureDetail::UniqueHandle thread;
};

[[nodiscard]] ChildProcess launchChild(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments)
{
    std::wstring command = quoteArgument(executable.native());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quoteArgument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    const std::wstring application = executable.native();
    if (::CreateProcessW(
            application.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &information) == FALSE) {
        throw std::runtime_error{"Could not launch agent process fixture."};
    }
    return ChildProcess{
        InfrastructureDetail::UniqueHandle{information.hProcess},
        InfrastructureDetail::UniqueHandle{information.hThread}};
}

[[nodiscard]] DWORD waitForChild(ChildProcess& child)
{
    require(
        ::WaitForSingleObject(child.process.get(), 30'000U) == WAIT_OBJECT_0,
        "agent process fixture timed out");
    DWORD exit{};
    require(
        ::GetExitCodeProcess(child.process.get(), &exit) != FALSE,
        "agent process fixture exit code could not be read");
    return exit;
}

[[nodiscard]] std::wstring eventName(
    const std::wstring_view purpose,
    const unsigned ordinal)
{
    return L"Local\\ForgeConductor-P10-Agent-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::wstring{purpose} + L"-" + std::to_wstring(ordinal);
}

void durableStartSupersedeCompleteAndRestart()
{
    Support::ScopedTestDirectory directory{L"agent-session-lifecycle"};
    RepositoryFixture fixture{directory.path()};
    const auto client = parse<Domain::ClientId>("client-lifecycle");
    const auto firstId = parse<Domain::SessionId>(
        "31000000-0000-4000-8000-000000000001");
    const auto secondId = parse<Domain::SessionId>(
        "31000000-0000-4000-8000-000000000002");

    const auto first = start(
        fixture, firstId, client, "p10-lifecycle-first");
    require(first.supersededSessions == 0U, "first run superseded a session");
    require(first.activeBinding.has_value(), "first run lost its active binding");
    require(first.run.projectId.has_value(), "project id was not persisted");

    fixture.clock->advance(1s);
    const auto second = start(
        fixture, secondId, client, "p10-lifecycle-second");
    require(second.supersededSessions == 1U, "second run did not supersede exactly one run");
    const auto closedFirst = take(fixture.repository->getRun(
        firstId, activeContext(*fixture.clock, "p10-lifecycle-first-read")));
    require(closedFirst.has_value(), "superseded run disappeared");
    require(
        closedFirst->session.status == Domain::SessionStatus::Closed,
        "superseded run remained open");

    fixture.clock->advance(1s);
    const std::string report{"{\"evidence\":[\"native-test\"],\"result\":\"done\"}"};
    const std::string summary{
        "{\"goal\":\"implement durable agent sessions\","
        "\"missing_schema_keys\":[],\"report\":" + report + "}"};
    const Domain::AgentRunCompleteMutation completion{
        secondId,
        client,
        report,
        summary,
        {},
        fixture.clock->utcNow()};
    const auto completed = take(fixture.repository->completeRun(
        completion,
        activeContext(*fixture.clock, "p10-lifecycle-complete")));
    require(completed.activeProjectionCleared, "completion did not clear active pointer");
    require(completed.run.reportJson == report, "completion report was not persisted");

    const auto repeated = take(fixture.repository->completeRun(
        completion,
        activeContext(*fixture.clock, "p10-lifecycle-complete-idempotent")));
    require(!repeated.activeProjectionCleared, "idempotent completion cleared twice");
    auto conflicting = completion;
    conflicting.summary += "-different";
    const auto conflict = fixture.repository->completeRun(
        conflicting,
        activeContext(*fixture.clock, "p10-lifecycle-complete-conflict"));
    require(!conflict, "different completion unexpectedly succeeded");
    require(conflict.error().code == Domain::ErrorCodes::Conflict,
            "different completion returned the wrong error");

    const auto recovered = take(fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{client},
        activeContext(*fixture.clock, "p10-lifecycle-recover-closed")));
    require(!recovered.run, "closed run remained active during recovery");
    fixture.close();
    fixture.open("p10-lifecycle-reopen");
    const auto durable = take(fixture.repository->getRun(
        secondId, activeContext(*fixture.clock, "p10-lifecycle-durable-read")));
    require(durable.has_value(), "completed run did not survive restart");
    require(durable->projectId == second.run.projectId, "project id changed on restart");
    require(durable->goal == second.run.goal, "goal changed on restart");
    require(durable->workingDirectory == second.run.workingDirectory,
            "working directory changed on restart");
    require(durable->reportJson == report, "report changed on restart");
    take(fixture.repository->quickCheck(
        activeContext(*fixture.clock, "p10-lifecycle-quick-check")));
}

void recoveryRepairAndHostileProjectionValidation()
{
    Support::ScopedTestDirectory directory{L"agent-session-repair"};
    RepositoryFixture fixture{directory.path()};
    const auto client = parse<Domain::ClientId>("client-repair");
    const auto sessionId = parse<Domain::SessionId>(
        "32000000-0000-4000-8000-000000000001");
    const auto started = start(
        fixture, sessionId, client, "p10-repair-start");
    fixture.close();
    {
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        database.execute(
            "DELETE FROM memory_notes WHERE key LIKE 'agent_run/%' "
            "OR key LIKE 'agent_active/%'");
    }
    fixture.open("p10-repair-reopen-missing");
    const auto fallback = take(fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{client},
        activeContext(*fixture.clock, "p10-repair-fallback")));
    require(fallback.run.has_value(), "fallback recovery did not find open run");
    require(!fallback.binding,
            "fallback recovery fabricated catalog-owned binding fields");
    require(!fallback.usedActiveProjection, "missing projection was reported as used");
    require(fallback.projectionNeedsRepair, "missing projection did not request repair");
    const auto repaired = take(fixture.repository->repairProjection(
        Domain::AgentProjectionRepairRequest{
            client, started.run, *started.activeBinding},
        activeContext(*fixture.clock, "p10-repair-apply")));
    require(repaired.repaired, "missing projections were not repaired");
    const auto healthy = take(fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{client},
        activeContext(*fixture.clock, "p10-repair-healthy")));
    require(healthy.usedActiveProjection, "repaired active projection was not used");
    require(!healthy.projectionNeedsRepair, "repaired projections remained stale");
    verifyRepeatedRecoveryIsStableAndNonMutating(
        fixture, client, started.run, *started.activeBinding);

    fixture.close();
    {
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        database.execute(
            "UPDATE memory_notes SET body='not-json' "
            "WHERE key='agent_active/client-repair'");
    }
    fixture.open("p10-repair-reopen-hostile");
    const auto hostile = fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{client},
        activeContext(*fixture.clock, "p10-repair-hostile"));
    require(!hostile, "malformed active projection was accepted");
    require(hostile.error().code == Domain::ErrorCodes::IntegrityFailure,
            "malformed active projection returned the wrong error");
}

void centralV5NullCompatibilityAndHostileRows(
    const std::filesystem::path& fixtureDirectory)
{
    Support::ScopedTestDirectory directory{L"agent-session-v5"};
    std::string script = Support::readFixture(
        fixtureDirectory / L"central-v5.sql");
    constexpr std::string_view LegacyId = "legacy-session-1";
    constexpr std::string_view ValidId =
        "33000000-0000-4000-8000-000000000001";
    const auto position = script.find(LegacyId);
    require(position != std::string::npos, "central-v5 fixture session id changed");
    script.replace(position, LegacyId.size(), ValidId);
    {
        SqliteDatabase database{directory.path() / L"store.sqlite", true};
        database.execute(script);
    }
    RepositoryFixture fixture{directory.path()};
    const auto sessionId = parse<Domain::SessionId>(ValidId);
    const auto migrated = take(fixture.repository->getRun(
        sessionId, activeContext(*fixture.clock, "p10-v5-read")));
    require(migrated.has_value(), "migrated v5 session was not readable");
    require(!migrated->projectId, "v5 project id was fabricated");
    require(!migrated->goal, "v5 goal was fabricated");
    require(!migrated->workingDirectory, "v5 cwd was fabricated");
    require(!migrated->reportJson, "v5 report was fabricated");
    require(migrated->outputSchema.empty(), "v5 schema was fabricated");
    require(migrated->firstMoves.empty(), "v5 first moves were fabricated");
    const auto recovered = take(fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{
            parse<Domain::ClientId>("legacy-client-1")},
        activeContext(*fixture.clock, "p10-v5-recover-null-goal")));
    require(recovered.run.has_value(), "v5 recovery lost its open run");
    require(!recovered.binding, "v5 NULL goal fabricated an active binding");
    fixture.close();
    {
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        database.execute(
            "UPDATE agent_sessions SET status='hostile' WHERE id='" +
            std::string{ValidId} + "'");
    }
    fixture.open("p10-v5-reopen-hostile");
    const auto hostile = fixture.repository->getRun(
        sessionId, activeContext(*fixture.clock, "p10-v5-hostile-read"));
    require(!hostile, "unknown persisted status was accepted");
    require(hostile.error().code == Domain::ErrorCodes::IntegrityFailure,
            "unknown persisted status returned the wrong error");
}

void timestampOrderingMonotonicTouchAndMaximumReport()
{
    Support::ScopedTestDirectory directory{L"agent-session-timestamp-report"};
    RepositoryFixture fixture{
        directory.path(), utcTime(2026, 8U, 26U, 13, 0, 0)};
    const auto client = parse<Domain::ClientId>("client-mixed-time");
    const auto agent = parse<Domain::AgentId>("test");
    const auto wholeId = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000001");
    const auto fractionalId = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000002");
    const auto base = fixture.clock->utcNow();
    take(fixture.repository->save(
        Domain::AgentSession{
            wholeId,
            agent,
            client,
            Domain::SessionStatus::Running,
            std::nullopt,
            base,
            base},
        activeContext(*fixture.clock, "p10-mixed-time-whole")));
    take(fixture.repository->save(
        Domain::AgentSession{
            fractionalId,
            agent,
            client,
            Domain::SessionStatus::Started,
            std::nullopt,
            base,
            base + 500ms},
        activeContext(*fixture.clock, "p10-mixed-time-fractional")));
    const auto latest = take(fixture.repository->latestOpenRun(
        client,
        activeContext(*fixture.clock, "p10-mixed-time-latest")));
    require(latest && latest->session.id == fractionalId,
            "mixed ISO timestamp forms were ordered lexically");
    const bool movedBackward = take(fixture.repository->touchRun(
        fractionalId,
        base + 250ms,
        activeContext(*fixture.clock, "p10-touch-backward")));
    require(!movedBackward, "touchRun moved updated_at backward");
    const auto unchanged = take(fixture.repository->getRun(
        fractionalId,
        activeContext(*fixture.clock, "p10-touch-backward-read")));
    require(unchanged && unchanged->session.updatedAt == base + 500ms,
            "a rejected touch changed updated_at");

    const auto stale = take(fixture.repository->closeStale(
        Domain::AgentStaleCloseRequest{base + 2s, base + 250ms, 10U},
        activeContext(*fixture.clock, "p10-mixed-time-stale")));
    require(stale.closedRuns.size() == 1U,
            "mixed timestamp stale comparison closed the wrong row count");
    require(stale.closedRuns.front().session.id == wholeId,
            "mixed timestamp stale comparison chose the wrong row");

    const auto supersedeClient =
        parse<Domain::ClientId>("client-clock-rollback-supersede");
    const auto supersededId = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000010");
    const auto replacementId = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000011");
    const auto supersededTimestamp = base + 20s;
    static_cast<void>(take(fixture.repository->startRun(
        startMutation(supersededId, supersedeClient, supersededTimestamp),
        activeContext(*fixture.clock, "p10-rollback-supersede-seed"))));
    const auto replacement = take(fixture.repository->startRun(
        startMutation(replacementId, supersedeClient, base + 10s),
        activeContext(*fixture.clock, "p10-rollback-supersede")));
    require(replacement.supersededSessions == 1U,
            "clock rollback did not supersede the prior client run");
    const auto superseded = take(fixture.repository->getRun(
        supersededId,
        activeContext(*fixture.clock, "p10-rollback-superseded-read")));
    require(superseded &&
                superseded->session.status == Domain::SessionStatus::Closed &&
                superseded->session.updatedAt == supersededTimestamp,
            "supersede moved a durable run or its projection backward");

    const auto reattachClient =
        parse<Domain::ClientId>("client-clock-rollback-reattach");
    const auto reattachId = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000012");
    const auto reattachTimestamp = base + 30s;
    const auto reattachSeed = take(fixture.repository->startRun(
        startMutation(reattachId, reattachClient, reattachTimestamp),
        activeContext(*fixture.clock, "p10-rollback-reattach-seed")));
    require(reattachSeed.activeBinding.has_value(),
            "rollback reattach seed did not create an active binding");
    auto changedBinding = *reattachSeed.activeBinding;
    changedBinding.toolsPrimary = {"rollback-safe-read"};
    const auto reattached = take(fixture.repository->reattachRun(
        Domain::AgentRunReattachMutation{
            reattachId,
            reattachClient,
            reattachClient,
            changedBinding,
            "clock rollback reattach supersede",
            base + 15s},
        activeContext(*fixture.clock, "p10-rollback-reattach")));
    require(reattached.run.session.updatedAt == reattachTimestamp,
            "reattach moved the durable run timestamp backward");
    const auto reattachRecovery = take(fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{reattachClient},
        activeContext(*fixture.clock, "p10-rollback-reattach-recover")));
    require(reattachRecovery.run && reattachRecovery.binding &&
                reattachRecovery.run->session.updatedAt == reattachTimestamp &&
                reattachRecovery.binding->toolsPrimary ==
                    changedBinding.toolsPrimary,
            "reattach moved or invalidated the replaced active projection");

    const auto completionClient =
        parse<Domain::ClientId>("client-clock-rollback-complete");
    const auto completionId = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000013");
    const auto completionTimestamp = base + 40s;
    static_cast<void>(take(fixture.repository->startRun(
        startMutation(completionId, completionClient, completionTimestamp),
        activeContext(*fixture.clock, "p10-rollback-complete-seed"))));
    const auto rollbackCompletion = take(fixture.repository->completeRun(
        Domain::AgentRunCompleteMutation{
            completionId,
            completionClient,
            "{\"result\":\"clock-rollback-safe\"}",
            "clock rollback completion",
            {},
            base + 25s},
        activeContext(*fixture.clock, "p10-rollback-complete")));
    require(rollbackCompletion.run.session.updatedAt == completionTimestamp,
            "completion moved the durable run timestamp backward");
    const auto completedAfterRollback = take(fixture.repository->getRun(
        completionId,
        activeContext(*fixture.clock, "p10-rollback-complete-read")));
    require(completedAfterRollback &&
                completedAfterRollback->session.updatedAt == completionTimestamp,
            "completion moved or invalidated the run projection timestamp");

    fixture.clock->advance(3s);
    const auto reportClient = parse<Domain::ClientId>("client-max-report");
    const auto reportSession = parse<Domain::SessionId>(
        "37000000-0000-4000-8000-000000000003");
    static_cast<void>(start(
        fixture,
        reportSession,
        reportClient,
        "p10-max-report-start"));
    std::string report{"{\"payload\":\""};
    require(report.size() + 2U <
                Domain::AgentSessionLimits::MaximumReportJsonBytes,
            "maximum-report test prefix is invalid");
    report.append(
        Domain::AgentSessionLimits::MaximumReportJsonBytes -
            report.size() - 2U,
        'x');
    report += "\"}";
    require(report.size() == Domain::AgentSessionLimits::MaximumReportJsonBytes,
            "maximum-report test did not reach the exact boundary");
    const auto completed = take(fixture.repository->completeRun(
        Domain::AgentRunCompleteMutation{
            reportSession,
            reportClient,
            report,
            "maximum-report-complete",
            {},
            fixture.clock->utcNow()},
        activeContext(*fixture.clock, "p10-max-report-complete")));
    require(completed.run.reportJson &&
                completed.run.reportJson->size() ==
                    Domain::AgentSessionLimits::MaximumReportJsonBytes,
            "maximum valid report was not persisted exactly");
    fixture.close();
    SqliteDatabase database{directory.path() / L"store.sqlite", false};
    require(
        database.integer(
            "SELECT length(CAST(body AS BLOB)) FROM memory_notes "
            "WHERE key='agent_run/37000000-0000-4000-8000-000000000003'") <
            static_cast<std::int64_t>(
                Domain::AgentSessionLimits::MaximumReportJsonBytes),
        "maximum report was duplicated into the bounded run projection");
}

void staleCloseCancellationDeadlineAndSharedShutdown()
{
    Support::ScopedTestDirectory directory{L"agent-session-bounds"};
    RepositoryFixture fixture{directory.path()};
    const auto firstClient = parse<Domain::ClientId>("client-stale-first");
    const auto secondClient = parse<Domain::ClientId>("client-stale-second");
    const auto firstId = parse<Domain::SessionId>(
        "34000000-0000-4000-8000-000000000001");
    const auto secondId = parse<Domain::SessionId>(
        "34000000-0000-4000-8000-000000000002");
    static_cast<void>(start(
        fixture, firstId, firstClient, "p10-stale-first"));
    fixture.clock->advance(30s);
    static_cast<void>(start(
        fixture, secondId, secondClient, "p10-stale-second"));
    fixture.clock->advance(30s);
    const auto closed = take(fixture.repository->closeStale(
        Domain::AgentStaleCloseRequest{
            fixture.clock->utcNow(),
            fixture.clock->utcNow() - 20s,
            1U},
        activeContext(*fixture.clock, "p10-stale-close")));
    require(closed.closedRuns.size() == 1U, "stale close ignored its row bound");
    require(closed.closedRuns.front().session.id == firstId,
            "stale close did not choose the oldest run");
    const auto stillOpen = take(fixture.repository->latestOpenRun(
        secondClient,
        activeContext(*fixture.clock, "p10-stale-latest")));
    require(stillOpen && stillOpen->session.id == secondId,
            "stale close removed the newer run");

    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled = fixture.repository->getRun(
        secondId,
        activeContext(*fixture.clock, "p10-cancelled", cancellation.get_token()));
    require(!cancelled, "cancelled repository request succeeded");
    require(cancelled.error().code == Domain::ErrorCodes::Cancelled,
            "cancelled repository request returned the wrong error");
    const auto expired = fixture.repository->getRun(
        secondId, expiredContext(*fixture.clock, "p10-expired"));
    require(!expired, "expired repository request succeeded");
    require(expired.error().code == Domain::ErrorCodes::DeadlineExceeded,
            "expired repository request returned the wrong error");

    {
        SqliteDatabase writeLock{directory.path() / L"store.sqlite", false};
        writeLock.execute("PRAGMA busy_timeout=0; BEGIN IMMEDIATE;");
        const auto busyStarted = std::chrono::steady_clock::now();
        const auto busy = fixture.repository->touchRun(
            secondId,
            fixture.clock->utcNow(),
            activeContext(*fixture.clock, "p10-database-busy"));
        const auto busyElapsed = std::chrono::steady_clock::now() - busyStarted;
        require(!busy, "write-locked repository operation unexpectedly succeeded");
        require(busy.error().code == Domain::ErrorCodes::DatabaseBusy,
                "bounded write-lock failure was not mapped to database_busy");
        require(busyElapsed >= 2500ms && busyElapsed < 4500ms,
                "database busy handling exceeded its bounded retry window");
        writeLock.execute("ROLLBACK;");
    }
    require(take(fixture.repository->touchRun(
                secondId,
                fixture.clock->utcNow(),
                activeContext(*fixture.clock, "p10-database-busy-recovered"))),
            "repository did not recover after the bounded database busy failure");
    fixture.close();

    auto central = take(PersistenceWindows::WindowsCentralDatabase::open(
        fixture.paths,
        fixture.diagnostics,
        fixture.clock,
        activeContext(*fixture.clock, "p10-shared-central-open")));
    auto sharedCentral = std::shared_ptr<PersistenceWindows::WindowsCentralDatabase>{
        std::move(central)};
    auto agentRepository = take(
        PersistenceWindows::WindowsAgentSessionRepository::attach(
            sharedCentral, fixture.clock));
    auto canonicalizer = std::make_shared<
        InfrastructureWindows::WindowsUnicodeCanonicalizer>();
    auto legacyRepository = take(
        PersistenceWindows::WindowsLegacyMemoryRepository::attach(
            sharedCentral, fixture.clock, canonicalizer));
    agentRepository->close();
    const auto rejected = agentRepository->getRun(
        secondId,
        activeContext(*fixture.clock, "p10-shared-agent-closed"));
    require(!rejected, "closed attached agent repository admitted work");
    static_cast<void>(take(legacyRepository->upsert(
        Domain::LegacyMemoryUpsert{"shared/probe", "alive", {}},
        activeContext(*fixture.clock, "p10-shared-legacy-write"))));
    legacyRepository->close();
    take(sharedCentral->quickCheck(
        activeContext(*fixture.clock, "p10-shared-central-quick-check")));
    take(sharedCentral->close(
        activeContext(*fixture.clock, "p10-shared-central-close")));
}

void crashRollbackAndCommittedRecovery(
    const std::filesystem::path& processFixture)
{
    struct CrashCase final {
        std::wstring mode;
        std::wstring suffix;
        std::string sessionId;
        std::string predecessorSessionId;
        std::string clientId;
        bool shouldCommit{};
    };
    const std::array cases{
        CrashCase{
            L"--crash-start-before-commit",
            L"before",
            "35000000-0000-4000-8000-000000000001",
            "35000000-0000-4000-8000-000000000011",
            "client-crash-before",
            false},
        CrashCase{
            L"--crash-start-after-commit",
            L"after",
            "35000000-0000-4000-8000-000000000002",
            "35000000-0000-4000-8000-000000000012",
            "client-crash-after",
            true}};
    unsigned ordinal{1U};
    for (const auto& testCase : cases) {
        Support::ScopedTestDirectory directory{
            L"agent-session-crash-" + testCase.suffix};
        const auto sessionId = parse<Domain::SessionId>(testCase.sessionId);
        const auto predecessorSessionId =
            parse<Domain::SessionId>(testCase.predecessorSessionId);
        const auto clientId = parse<Domain::ClientId>(testCase.clientId);
        RepositoryFixture predecessorFixture{directory.path()};
        const auto predecessorStarted = start(
            predecessorFixture,
            predecessorSessionId,
            clientId,
            "p10-crash-predecessor-seed",
            "crash-predecessor-goal");
        require(predecessorStarted.activeBinding.has_value(),
                "start crash predecessor has no active binding");
        predecessorFixture.close();

        const auto readyName = eventName(L"crash-ready", ordinal++);
        const auto releaseName = eventName(L"crash-release", ordinal++);
        InfrastructureDetail::UniqueHandle ready{
            ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())};
        InfrastructureDetail::UniqueHandle release{
            ::CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str())};
        require(ready && release, "crash-test events could not be created");
        auto child = launchChild(
            processFixture,
            {testCase.mode,
             directory.path().native(),
             L"reserved",
             readyName,
             releaseName,
             take(InfrastructureDetail::strictUtf8ToUtf16(sessionId.value())),
             take(InfrastructureDetail::strictUtf8ToUtf16(clientId.value()))});
        require(
            ::WaitForSingleObject(ready.get(), 30'000U) == WAIT_OBJECT_0,
            "process fixture did not reach the requested transaction checkpoint");
        require(
            ::TerminateProcess(child.process.get(), 0xD10U) != FALSE,
            "process fixture could not be terminated");
        static_cast<void>(waitForChild(child));

        RepositoryFixture fixture{directory.path()};
        take(fixture.repository->quickCheck(
            activeContext(*fixture.clock, "p10-crash-quick-check")));
        const auto run = take(fixture.repository->getRun(
            sessionId,
            activeContext(*fixture.clock, "p10-crash-reopen-run")));
        require(run.has_value() == testCase.shouldCommit,
                "crash checkpoint exposed a partial transaction state");
        const auto predecessor = take(fixture.repository->getRun(
            predecessorSessionId,
            activeContext(*fixture.clock, "p10-crash-predecessor-read")));
        require(predecessor.has_value(),
                "start crash lost the committed same-client predecessor");
        if (testCase.shouldCommit) {
            const auto expectedSummary = Domain::makeAgentSupersedeSummary(
                "Closed because a process-fixture run started",
                std::optional<Domain::AgentId>{
                    parse<Domain::AgentId>("implement")},
                std::nullopt);
            require(run && Domain::isOpen(run->session.status),
                    "post-commit start crash lost the new open run");
            require(predecessor->session.status == Domain::SessionStatus::Closed &&
                        predecessor->session.summary == expectedSummary,
                    "post-commit start crash did not close its predecessor exactly");
        } else {
            require(Domain::isOpen(predecessor->session.status) &&
                        !predecessor->session.summary,
                    "pre-commit start crash changed its predecessor");
        }
        const auto recovered = take(fixture.repository->recoverRun(
            Domain::AgentRunRecoveryRequest{clientId},
            activeContext(*fixture.clock, "p10-crash-reopen-recover")));
        require(recovered.run && recovered.binding,
                "start crash lost the surviving active projection");
        const auto expectedActiveSession = testCase.shouldCommit
            ? sessionId
            : predecessorSessionId;
        require(recovered.run->session.id == expectedActiveSession &&
                    recovered.binding->sessionId == expectedActiveSession,
                "start crash row and active projection disagree");

        fixture.close();
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        requireRunProjectionMatches(database, *predecessor);
        if (run) {
            requireRunProjectionMatches(database, *run);
        } else {
            requireRunProjectionAbsent(database, sessionId);
        }
        requireActiveProjectionMatches(database, clientId, *recovered.binding);
        require(database.integer(
                    "SELECT COUNT(*) FROM memory_notes WHERE key LIKE "
                    "'agent_active/%'") == 1,
                "start crash left an atomic active-projection mismatch");
        require(database.integer(
                    "SELECT COUNT(*) FROM memory_notes WHERE key LIKE "
                    "'agent_run/%'") == (testCase.shouldCommit ? 2 : 1),
                "start crash left an atomic run-projection mismatch");
    }
}

void completionCrashRetryIgnoresNewTimestamp(
    const std::filesystem::path& processFixture)
{
    struct CompletionCrashCase final {
        std::wstring mode;
        std::wstring suffix;
        std::string sessionId;
        std::string clientId;
        bool shouldCommit{};
    };
    const std::array cases{
        CompletionCrashCase{
            L"--crash-complete-before-commit",
            L"before",
            "38000000-0000-4000-8000-000000000001",
            "client-complete-before",
            false},
        CompletionCrashCase{
            L"--crash-complete-after-commit",
            L"after",
            "38000000-0000-4000-8000-000000000002",
            "client-complete-after",
            true}};
    unsigned ordinal{21U};
    for (const auto& testCase : cases) {
        Support::ScopedTestDirectory directory{
            L"agent-session-complete-crash-" + testCase.suffix};
        const auto readyName = eventName(L"complete-ready", ordinal++);
        const auto releaseName = eventName(L"complete-release", ordinal++);
        InfrastructureDetail::UniqueHandle ready{
            ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())};
        InfrastructureDetail::UniqueHandle release{
            ::CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str())};
        require(ready && release, "completion crash events could not be created");
        auto child = launchChild(
            processFixture,
            {testCase.mode,
             directory.path().native(),
             L"reserved",
             readyName,
             releaseName,
             take(InfrastructureDetail::strictUtf8ToUtf16(testCase.sessionId)),
             take(InfrastructureDetail::strictUtf8ToUtf16(testCase.clientId))});
        require(::WaitForSingleObject(ready.get(), 30'000U) == WAIT_OBJECT_0,
                "completion process did not reach its transaction checkpoint");
        require(::TerminateProcess(child.process.get(), 0xD11U) != FALSE,
                "completion process could not be terminated");
        static_cast<void>(waitForChild(child));

        RepositoryFixture fixture{directory.path()};
        take(fixture.repository->quickCheck(
            activeContext(*fixture.clock, "p10-complete-crash-quick-check")));
        const auto sessionId = parse<Domain::SessionId>(testCase.sessionId);
        const auto clientId = parse<Domain::ClientId>(testCase.clientId);
        const auto durable = take(fixture.repository->getRun(
            sessionId,
            activeContext(*fixture.clock, "p10-complete-crash-read")));
        require(durable.has_value(),
                "completion crash lost the previously committed start");
        std::optional<Domain::ActiveBinding> expectedActiveBinding;
        if (!testCase.shouldCommit) {
            require(Domain::isOpen(durable->session.status),
                    "pre-commit completion crash exposed a closed row");
            require(!durable->reportJson && !durable->session.summary,
                    "pre-commit completion crash exposed completion data");
            const auto active = take(fixture.repository->recoverRun(
                Domain::AgentRunRecoveryRequest{clientId},
                activeContext(*fixture.clock, "p10-complete-before-recover")));
            require(active.run && active.binding,
                    "pre-commit completion crash lost the active binding");
            require(active.run->session.id == sessionId &&
                        active.binding->sessionId == sessionId,
                    "pre-commit completion crash active pointer changed target");
            expectedActiveBinding = *active.binding;
        } else {
            require(durable->session.status == Domain::SessionStatus::Closed &&
                        durable->session.summary ==
                            std::optional<std::string>{
                                "crash-completion-summary"},
                    "post-commit completion crash lost closed summary state");
            require(durable->reportJson ==
                        std::optional<std::string>{
                            "{\"result\":\"crash-committed\"}"},
                    "post-commit completion crash lost report data");
            const auto inactive = take(fixture.repository->recoverRun(
                Domain::AgentRunRecoveryRequest{clientId},
                activeContext(*fixture.clock, "p10-complete-after-recover")));
            require(!inactive.run && !inactive.binding,
                    "post-commit completion crash retained an active binding");
            const auto originalCompletionTime = durable->session.updatedAt;
            const auto retried = take(fixture.repository->completeRun(
                Domain::AgentRunCompleteMutation{
                    sessionId,
                    clientId,
                    "{\"result\":\"crash-committed\"}",
                    "crash-completion-summary",
                    {},
                    originalCompletionTime + 5min},
                activeContext(*fixture.clock, "p10-complete-crash-retry")));
            require(retried.run.session.updatedAt == originalCompletionTime,
                    "idempotent crash retry rewrote the committed completion time");
            require(!retried.activeProjectionCleared,
                    "idempotent crash retry found a partial active projection");
        }

        fixture.close();
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        requireRunProjectionMatches(database, *durable);
        if (expectedActiveBinding) {
            requireActiveProjectionMatches(
                database, clientId, *expectedActiveBinding);
        } else {
            requireActiveProjectionAbsent(database, clientId);
        }
        require(database.integer(
                    "SELECT COUNT(*) FROM memory_notes WHERE key LIKE "
                    "'agent_run/%'") == 1,
                "completion crash did not retain exactly one run projection");
        require(database.integer(
                    "SELECT COUNT(*) FROM memory_notes WHERE key LIKE "
                    "'agent_active/%'") ==
                    (testCase.shouldCommit ? 0 : 1),
                "completion crash active removal was not atomic");
    }
}

void reattachCrashBeforeAndAfter(
    const std::filesystem::path& processFixture)
{
    struct ReattachCrashCase final {
        std::wstring mode;
        std::wstring suffix;
        std::string sessionId;
        std::string destinationSessionId;
        std::string originalClient;
        std::string newClient;
        bool shouldCommit{};
    };
    const std::array cases{
        ReattachCrashCase{
            L"--crash-reattach-before-commit",
            L"before",
            "39000000-0000-4000-8000-000000000001",
            "39000000-0000-4000-8000-000000000011",
            "client-reattach-before-original",
            "client-reattach-before-new",
            false},
        ReattachCrashCase{
            L"--crash-reattach-after-commit",
            L"after",
            "39000000-0000-4000-8000-000000000002",
            "39000000-0000-4000-8000-000000000012",
            "client-reattach-after-original",
            "client-reattach-after-new",
            true}};
    unsigned ordinal{31U};
    for (const auto& testCase : cases) {
        Support::ScopedTestDirectory directory{
            L"agent-session-reattach-crash-" + testCase.suffix};
        RepositoryFixture fixture{directory.path()};
        const auto sessionId = parse<Domain::SessionId>(testCase.sessionId);
        const auto destinationSessionId =
            parse<Domain::SessionId>(testCase.destinationSessionId);
        const auto originalClient =
            parse<Domain::ClientId>(testCase.originalClient);
        const auto newClient = parse<Domain::ClientId>(testCase.newClient);
        static_cast<void>(start(
            fixture,
            sessionId,
            originalClient,
            "p10-reattach-crash-seed",
            "reattach-crash-goal"));
        static_cast<void>(start(
            fixture,
            destinationSessionId,
            newClient,
            "p10-reattach-crash-destination-seed",
            "reattach-destination-goal"));
        fixture.close();

        const auto readyName = eventName(L"reattach-ready", ordinal++);
        const auto releaseName = eventName(L"reattach-release", ordinal++);
        InfrastructureDetail::UniqueHandle ready{
            ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())};
        InfrastructureDetail::UniqueHandle release{
            ::CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str())};
        require(ready && release, "reattach crash events could not be created");
        auto child = launchChild(
            processFixture,
            {testCase.mode,
             directory.path().native(),
             L"reserved",
             readyName,
             releaseName,
             take(InfrastructureDetail::strictUtf8ToUtf16(testCase.sessionId)),
             take(InfrastructureDetail::strictUtf8ToUtf16(
                 testCase.originalClient)),
             take(InfrastructureDetail::strictUtf8ToUtf16(testCase.newClient))});
        require(::WaitForSingleObject(ready.get(), 30'000U) == WAIT_OBJECT_0,
                "reattach process did not reach its transaction checkpoint");
        require(::TerminateProcess(child.process.get(), 0xD12U) != FALSE,
                "reattach process could not be terminated");
        static_cast<void>(waitForChild(child));

        fixture.open("p10-reattach-crash-reopen");
        take(fixture.repository->quickCheck(
            activeContext(*fixture.clock, "p10-reattach-crash-quick-check")));
        const auto durable = take(fixture.repository->getRun(
            sessionId,
            activeContext(*fixture.clock, "p10-reattach-crash-read")));
        require(durable && durable->session.clientId,
                "reattach crash lost durable session ownership");
        const auto expectedOwner = testCase.shouldCommit
            ? testCase.newClient
            : testCase.originalClient;
        require(durable->session.clientId->value() == expectedOwner,
                "reattach crash exposed a partial ownership transfer");
        const auto destination = take(fixture.repository->getRun(
            destinationSessionId,
            activeContext(*fixture.clock, "p10-reattach-destination-read")));
        require(destination.has_value(),
                "reattach crash lost the destination-owned predecessor");
        if (testCase.shouldCommit) {
            const auto expectedSummary = Domain::makeAgentSupersedeSummary(
                "Closed because a process-fixture run was reattached",
                std::nullopt,
                std::optional<Domain::SessionId>{sessionId});
            require(destination->session.status == Domain::SessionStatus::Closed &&
                        destination->session.summary == expectedSummary,
                    "post-commit reattach did not supersede the destination run");
        } else {
            require(Domain::isOpen(destination->session.status) &&
                        !destination->session.summary,
                    "pre-commit reattach changed the destination run");
        }
        const auto originalRecovery = take(fixture.repository->recoverRun(
            Domain::AgentRunRecoveryRequest{originalClient},
            activeContext(*fixture.clock, "p10-reattach-original-recover")));
        const auto newRecovery = take(fixture.repository->recoverRun(
            Domain::AgentRunRecoveryRequest{newClient},
            activeContext(*fixture.clock, "p10-reattach-new-recover")));
        if (testCase.shouldCommit) {
            require(!originalRecovery.run && !originalRecovery.binding,
                    "post-commit reattach retained the old active pointer");
            require(newRecovery.run && newRecovery.binding &&
                        newRecovery.run->session.id == sessionId &&
                        newRecovery.binding->sessionId == sessionId,
                    "post-commit reattach did not replace the new active pointer");
        } else {
            require(originalRecovery.run && originalRecovery.binding &&
                        originalRecovery.run->session.id == sessionId &&
                        originalRecovery.binding->sessionId == sessionId,
                    "pre-commit reattach changed the old active pointer");
            require(newRecovery.run && newRecovery.binding &&
                        newRecovery.run->session.id == destinationSessionId &&
                        newRecovery.binding->sessionId == destinationSessionId,
                    "pre-commit reattach changed the destination active pointer");
        }
        fixture.close();
        SqliteDatabase database{directory.path() / L"store.sqlite", false};
        requireRunProjectionMatches(database, *durable);
        requireRunProjectionMatches(database, *destination);
        if (testCase.shouldCommit) {
            requireActiveProjectionAbsent(database, originalClient);
            requireActiveProjectionMatches(
                database, newClient, *newRecovery.binding);
        } else {
            requireActiveProjectionMatches(
                database, originalClient, *originalRecovery.binding);
            requireActiveProjectionMatches(
                database, newClient, *newRecovery.binding);
        }
        require(
            database.integer(
                "SELECT COUNT(*) FROM memory_notes "
                "WHERE key LIKE 'agent_active/%'") ==
                (testCase.shouldCommit ? 1 : 2),
            "reattach crash left a partial active projection set");
        require(
            database.integer(
                "SELECT COUNT(*) FROM memory_notes "
                "WHERE key LIKE 'agent_run/%'") == 2,
            "reattach crash left a partial run projection set");
    }
}

void crossProcessReattachCas(const std::filesystem::path& processFixture)
{
    reattachCrashBeforeAndAfter(processFixture);
    Support::ScopedTestDirectory directory{L"agent-session-process-cas"};
    RepositoryFixture fixture{directory.path()};
    const auto sessionId = parse<Domain::SessionId>(
        "36000000-0000-4000-8000-000000000001");
    const auto originalClient = parse<Domain::ClientId>("client-owner-original");
    static_cast<void>(start(
        fixture,
        sessionId,
        originalClient,
        "p10-process-cas-start",
        "cross-process-goal"));
    fixture.close();

    const auto readyOneName = eventName(L"cas-ready-one", 11U);
    const auto readyTwoName = eventName(L"cas-ready-two", 12U);
    const auto startName = eventName(L"cas-start", 13U);
    InfrastructureDetail::UniqueHandle readyOne{
        ::CreateEventW(nullptr, TRUE, FALSE, readyOneName.c_str())};
    InfrastructureDetail::UniqueHandle readyTwo{
        ::CreateEventW(nullptr, TRUE, FALSE, readyTwoName.c_str())};
    InfrastructureDetail::UniqueHandle startEvent{
        ::CreateEventW(nullptr, TRUE, FALSE, startName.c_str())};
    require(readyOne && readyTwo && startEvent,
            "cross-process CAS events could not be created");
    const auto sessionWide = take(
        InfrastructureDetail::strictUtf8ToUtf16(sessionId.value()));
    const auto ownerWide = take(
        InfrastructureDetail::strictUtf8ToUtf16(originalClient.value()));
    auto first = launchChild(
        processFixture,
        {L"--reattach",
         directory.path().native(),
         readyOneName,
         startName,
         sessionWide,
         ownerWide,
         L"client-owner-one"});
    auto second = launchChild(
        processFixture,
        {L"--reattach",
         directory.path().native(),
         readyTwoName,
         startName,
         sessionWide,
         ownerWide,
         L"client-owner-two"});
    require(::WaitForSingleObject(readyOne.get(), 30'000U) == WAIT_OBJECT_0,
            "first CAS process did not become ready");
    require(::WaitForSingleObject(readyTwo.get(), 30'000U) == WAIT_OBJECT_0,
            "second CAS process did not become ready");
    require(::SetEvent(startEvent.get()) != FALSE,
            "cross-process CAS start could not be signaled");
    const DWORD firstExit = waitForChild(first);
    const DWORD secondExit = waitForChild(second);
    const std::array<DWORD, 2U> exits{firstExit, secondExit};
    require(std::count(exits.begin(), exits.end(), 0U) == 1,
            "cross-process CAS did not produce exactly one winner");
    require(std::count(exits.begin(), exits.end(), 41U) == 1,
            "cross-process CAS did not produce exactly one ownership conflict");

    fixture.open("p10-process-cas-reopen");
    const auto durable = take(fixture.repository->getRun(
        sessionId,
        activeContext(*fixture.clock, "p10-process-cas-read")));
    require(durable && durable->session.clientId,
            "cross-process CAS lost durable ownership");
    require(
        durable->session.clientId->value() == "client-owner-one" ||
            durable->session.clientId->value() == "client-owner-two",
        "cross-process CAS stored an unexpected owner");
    const auto winningClient = *durable->session.clientId;
    const auto losingClient = parse<Domain::ClientId>(
        winningClient.value() == "client-owner-one"
            ? "client-owner-two"
            : "client-owner-one");
    const auto winningRecovery = take(fixture.repository->recoverRun(
        Domain::AgentRunRecoveryRequest{winningClient},
        activeContext(*fixture.clock, "p10-process-cas-winning-recover")));
    require(winningRecovery.run && winningRecovery.binding &&
                winningRecovery.run->session.id == sessionId &&
                winningRecovery.binding->sessionId == sessionId,
            "cross-process CAS winner has no matching active pointer body");
    fixture.close();
    SqliteDatabase database{directory.path() / L"store.sqlite", false};
    requireActiveProjectionMatches(
        database, winningClient, *winningRecovery.binding);
    requireActiveProjectionAbsent(database, originalClient);
    requireActiveProjectionAbsent(database, losingClient);
    require(
        database.integer(
            "SELECT COUNT(*) FROM memory_notes "
            "WHERE key LIKE 'agent_active/%'") == 1,
        "cross-process CAS left duplicate active projections");
}

} // namespace

int main(const int argumentCount, const char* const arguments[])
{
    try {
        if (argumentCount != 3) {
            throw std::runtime_error{
                "Expected process-fixture path and persistence-fixture directory."};
        }
        const auto processFixture = std::filesystem::absolute(arguments[1]);
        const auto fixtureDirectory = std::filesystem::absolute(arguments[2]);

        durableStartSupersedeCompleteAndRestart();
        std::cout << "PASS agent_session_repository.lifecycle_restart\n";
        recoveryRepairAndHostileProjectionValidation();
        std::cout << "PASS agent_session_repository.recovery_repair\n";
        centralV5NullCompatibilityAndHostileRows(fixtureDirectory);
        std::cout << "PASS agent_session_repository.v5_hostile_rows\n";
        staleCloseCancellationDeadlineAndSharedShutdown();
        std::cout << "PASS agent_session_repository.bounds_shutdown\n";
        timestampOrderingMonotonicTouchAndMaximumReport();
        std::cout << "PASS agent_session_repository.timestamp_report_bounds\n";
        crashRollbackAndCommittedRecovery(processFixture);
        std::cout << "PASS agent_session_repository.crash_atomicity\n";
        completionCrashRetryIgnoresNewTimestamp(processFixture);
        std::cout << "PASS agent_session_repository.completion_crash_retry\n";
        crossProcessReattachCas(processFixture);
        std::cout << "PASS agent_session_repository.cross_process_cas\n";
        std::cout << "SUMMARY passed=8 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
