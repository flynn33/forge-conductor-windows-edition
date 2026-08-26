#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"

#include "ForgeConductor/Domain/LegacyMemoryModels.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "Detail/WinsqliteTransaction.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows {
namespace {

using Detail::WinsqliteConnection;
using Detail::WinsqliteStatement;
using Detail::WinsqliteStepResult;
using Detail::WinsqliteTransaction;
using Json = nlohmann::json;

constexpr std::size_t MaximumTimestampBytes = 64U;
constexpr std::size_t MaximumSummaryBytes =
    Domain::AgentSessionLimits::MaximumSummaryUnits * 4U;
constexpr std::size_t MaximumProjectionBytes =
    Domain::AgentSessionLimits::MaximumReportJsonBytes;
constexpr std::size_t MaximumTagsJsonBytes = 16U * 1024U;
constexpr std::size_t MaximumPersistedIdentifierBytes = 256U;
constexpr std::size_t MaximumBoundedRows =
    Domain::AgentSessionLimits::MaximumSessionQueryRows;
constexpr std::string_view OpenStatusPredicate =
    "status IN ('open','active','running','started')";
constexpr std::string_view RunColumns =
    "id,agent_id,client_id,status,summary,created_at,updated_at,"
    "project_id,goal,cwd,report_json";

struct RepositoryFailure final {
    Domain::Error error;
};

struct StoredMemoryProjection final {
    std::string body;
    std::vector<std::string> tags;
    Domain::UtcTimePoint createdAt;
    Domain::UtcTimePoint updatedAt;
};

struct DecodedRunProjection final {
    Domain::SessionId sessionId;
    Domain::AgentId agentId;
    Domain::SessionStatus status{Domain::SessionStatus::Open};
    std::optional<Domain::ProjectId> projectId;
    std::optional<std::string> goal;
    std::optional<Domain::PathText> workingDirectory;
    std::vector<std::string> outputSchema;
    std::vector<std::string> firstMoves;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw RepositoryFailure{std::move(error)};
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        fail(std::move(result).error());
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        fail(std::move(result).error());
    }
}

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> guarded(Callable&& callable) noexcept
{
    try {
        if constexpr (std::is_void_v<T>) {
            std::forward<Callable>(callable)();
            return Domain::Result<void>::success();
        } else {
            return Domain::Result<T>::success(
                std::forward<Callable>(callable)());
        }
    } catch (RepositoryFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The agent-session repository operation failed safely."));
    }
}

template <typename T, typename Callable>
class TypedDatabaseOperation final : public Detail::IWindowsDatabaseOperation {
public:
    explicit TypedDatabaseOperation(Callable callable)
        : callable_{std::move(callable)}
    {
    }

    [[nodiscard]] Domain::Result<T> takeOutcome()
    {
        if (!outcome_) {
            return Domain::Result<T>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The agent-session database operation produced no result."));
        }
        return std::move(*outcome_);
    }

private:
    [[nodiscard]] Domain::Result<void> execute(
        WinsqliteConnection& connection) noexcept override
    {
        try {
            outcome_.emplace(callable_(connection));
            if (!*outcome_) {
                return Domain::Result<void>::failure(outcome_->error());
            }
            return Domain::Result<void>::success();
        } catch (...) {
            auto error = Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The agent-session database callback failed safely.");
            outcome_.emplace(Domain::Result<T>::failure(error));
            return Domain::Result<void>::failure(std::move(error));
        }
    }

    Callable callable_;
    std::optional<Domain::Result<T>> outcome_;
};

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> runOnStore(
    Detail::WindowsDatabaseStore& store,
    const std::string_view action,
    const Domain::OperationContext& context,
    Callable&& callable) noexcept
{
    using Operation = TypedDatabaseOperation<T, std::decay_t<Callable>>;
    Operation operation{std::forward<Callable>(callable)};
    auto completed = store.runExclusive(operation, action, context);
    if (!completed) {
        return Domain::Result<T>::failure(std::move(completed).error());
    }
    return operation.takeOutcome();
}

[[nodiscard]] Domain::Error integrityError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] Domain::Error sessionNotFound(const Domain::SessionId& sessionId)
{
    return Domain::makeError(
        Domain::ErrorCodes::SessionNotFound,
        "Unknown agent session: " + sessionId.value(),
        true);
}

[[nodiscard]] Domain::Error ownershipConflict(const Domain::SessionId& sessionId)
{
    return Domain::makeError(
        Domain::ErrorCodes::OwnershipConflict,
        "Agent session ownership changed: " + sessionId.value(),
        true);
}

void requirePersistedUtf8(
    const std::string_view value,
    const std::string_view field)
{
    if (value.find('\0') != std::string_view::npos ||
        !Infrastructure::Windows::Detail::strictUtf8ToUtf16(value)) {
        fail(integrityError(
            "Persisted agent data contains invalid UTF-8: " +
            std::string{field} + '.'));
    }
}

void requireInputText(
    const std::string_view value,
    const std::size_t maximumBytes,
    const bool allowEmpty,
    const std::string_view field)
{
    if ((!allowEmpty && value.empty()) || value.size() > maximumBytes ||
        value.find('\0') != std::string_view::npos ||
        !Infrastructure::Windows::Detail::strictUtf8ToUtf16(value)) {
        fail(Domain::makeError(
            value.size() > maximumBytes
                ? Domain::ErrorCodes::PayloadTooLarge
                : Domain::ErrorCodes::InvalidRequest,
            std::string{field} + " violates its UTF-8 or byte boundary."));
    }
}

[[nodiscard]] std::string requiredText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto text = statement.columnText(column, maximumBytes);
    if (!text || !text.value()) {
        fail(integrityError(
            "A required persisted agent column is null or oversized: " +
            std::string{field} + '.'));
    }
    auto value = std::move(text).value().value();
    requirePersistedUtf8(value, field);
    return value;
}

[[nodiscard]] std::optional<std::string> optionalText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto text = statement.columnText(column, maximumBytes);
    if (!text) {
        fail(integrityError(
            "An optional persisted agent column is oversized: " +
            std::string{field} + '.'));
    }
    auto value = std::move(text).value();
    if (value) {
        requirePersistedUtf8(*value, field);
    }
    return value;
}

template <typename Identifier>
[[nodiscard]] Identifier persistedIdentifier(
    const std::string_view value,
    const std::string_view field)
{
    auto parsed = Identifier::parse(value);
    if (!parsed) {
        fail(integrityError(
            "A persisted agent identifier is invalid: " +
            std::string{field} + '.'));
    }
    return std::move(parsed).value();
}

[[nodiscard]] std::optional<int> decimalComponent(
    const std::string_view text,
    const std::size_t offset,
    const std::size_t count) noexcept
{
    if (offset > text.size() || count > text.size() - offset) {
        return std::nullopt;
    }
    int value{};
    for (std::size_t index = offset; index < offset + count; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
        value = value * 10 + (text[index] - '0');
    }
    return value;
}

[[nodiscard]] Domain::UtcTimePoint parseTimestamp(const std::string_view text)
{
    const bool wholeSeconds = text.size() == 20U && text[19] == 'Z';
    const bool fractional =
        text.size() == 24U && text[19] == '.' && text[23] == 'Z';
    if ((!wholeSeconds && !fractional) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        fail(integrityError(
            "A persisted agent timestamp is not canonical UTC."));
    }
    const auto year = decimalComponent(text, 0U, 4U);
    const auto month = decimalComponent(text, 5U, 2U);
    const auto day = decimalComponent(text, 8U, 2U);
    const auto hour = decimalComponent(text, 11U, 2U);
    const auto minute = decimalComponent(text, 14U, 2U);
    const auto second = decimalComponent(text, 17U, 2U);
    const auto millisecond = fractional
        ? decimalComponent(text, 20U, 3U)
        : std::optional<int>{0};
    if (!year || !month || !day || !hour || !minute || !second ||
        !millisecond || *year < 1970 || *month < 1 || *month > 12 ||
        *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59) {
        fail(integrityError("A persisted agent timestamp is invalid."));
    }
    std::tm utc{};
    utc.tm_year = *year - 1900;
    utc.tm_mon = *month - 1;
    utc.tm_mday = *day;
    utc.tm_hour = *hour;
    utc.tm_min = *minute;
    utc.tm_sec = *second;
    const __time64_t encoded = ::_mkgmtime64(&utc);
    std::tm roundTrip{};
    if (encoded < 0 || ::_gmtime64_s(&roundTrip, &encoded) != 0 ||
        roundTrip.tm_year != utc.tm_year || roundTrip.tm_mon != utc.tm_mon ||
        roundTrip.tm_mday != utc.tm_mday || roundTrip.tm_hour != utc.tm_hour ||
        roundTrip.tm_min != utc.tm_min || roundTrip.tm_sec != utc.tm_sec) {
        fail(integrityError(
            "A persisted agent timestamp is outside the supported range."));
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
        std::chrono::milliseconds{*millisecond};
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto totalMilliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(timestamp.time_since_epoch());
    if (totalMilliseconds.count() < 0) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Agent timestamps must be at or after the Unix epoch."));
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        totalMilliseconds);
    const auto milliseconds = totalMilliseconds -
        std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
    const __time64_t time = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &time) != 0) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "An agent timestamp is outside the supported UTC range."));
    }
    std::array<char, 25U> buffer{};
    const int written = milliseconds.count() == 0
        ? std::snprintf(
              buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec)
        : std::snprintf(
              buffer.data(), buffer.size(),
              "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec,
              static_cast<long long>(milliseconds.count()));
    if ((milliseconds.count() == 0 && written != 20) ||
        (milliseconds.count() != 0 && written != 24)) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "An agent timestamp could not be formatted."));
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] bool knownStatus(const Domain::SessionStatus status) noexcept
{
    switch (status) {
    case Domain::SessionStatus::Open:
    case Domain::SessionStatus::Active:
    case Domain::SessionStatus::Running:
    case Domain::SessionStatus::Started:
    case Domain::SessionStatus::Closed:
    case Domain::SessionStatus::Completed:
    case Domain::SessionStatus::Failed:
        return true;
    }
    return false;
}

void validateSession(const Domain::AgentSession& session)
{
    if (!knownStatus(session.status) || session.updatedAt < session.createdAt) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The agent session has an invalid status or timestamp order."));
    }
    if (session.summary) {
        requireInputText(
            *session.summary, MaximumSummaryBytes, true, "Agent summary");
    }
    static_cast<void>(timestampText(session.createdAt));
    static_cast<void>(timestampText(session.updatedAt));
}

void validateRun(const Domain::AgentRunRecord& run)
{
    validateSession(run.session);
    take(Domain::validateAgentRunRecord(run));
    if (run.reportJson) {
        const Json parsed = Json::parse(
            run.reportJson->begin(), run.reportJson->end(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Agent report JSON must contain one valid JSON object."));
        }
    }
}

void validateSummary(
    const std::string_view summary,
    const std::string_view field)
{
    requireInputText(summary, MaximumSummaryBytes, true, field);
}

template <typename SqlOwner>
[[nodiscard]] Domain::Result<WinsqliteStatement> prepare(
    SqlOwner& owner,
    const std::string_view sql,
    const Domain::OperationContext& context) noexcept
{
    if constexpr (std::is_same_v<SqlOwner, WinsqliteConnection>) {
        return owner.prepare(sql, context);
    } else {
        static_cast<void>(context);
        return owner.prepare(sql);
    }
}

void stepDone(WinsqliteStatement& statement)
{
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        fail(integrityError(
            "An agent-session mutation unexpectedly returned a row."));
    }
}

template <typename SqlOwner>
[[nodiscard]] std::int64_t scalarInteger(
    SqlOwner& owner,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(prepare(owner, sql, context));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        fail(integrityError(
            "An agent-session scalar query returned no row."));
    }
    return take(statement.columnInt64(0));
}

template <typename SqlOwner>
[[nodiscard]] std::int64_t changes(
    SqlOwner& owner,
    const Domain::OperationContext& context)
{
    return scalarInteger(owner, "SELECT changes()", context);
}

[[nodiscard]] std::vector<std::string> decodeStringArray(
    const Json& object,
    const std::string_view key,
    const std::size_t maximumItems,
    const bool required)
{
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        if (required) {
            fail(integrityError(
                "An agent projection is missing array field: " +
                std::string{key} + '.'));
        }
        return {};
    }
    if (!found->is_array() || found->size() > maximumItems) {
        fail(integrityError(
            "An agent projection array is malformed or oversized: " +
            std::string{key} + '.'));
    }
    std::vector<std::string> values;
    values.reserve(found->size());
    for (const auto& item : *found) {
        if (!item.is_string()) {
            fail(integrityError(
                "An agent projection array contains a non-string item."));
        }
        auto value = item.get<std::string>();
        if (value.empty() ||
            value.size() > Domain::AgentSessionLimits::MaximumItemBytes) {
            fail(integrityError(
                "An agent projection array item violates its byte boundary."));
        }
        requirePersistedUtf8(value, key);
        values.push_back(std::move(value));
    }
    return values;
}

[[nodiscard]] std::string requiredJsonString(
    const Json& object,
    const std::string_view key,
    const std::size_t maximumBytes)
{
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) {
        fail(integrityError(
            "An agent projection is missing string field: " +
            std::string{key} + '.'));
    }
    auto value = found->get<std::string>();
    if (value.empty() || value.size() > maximumBytes) {
        fail(integrityError(
            "An agent projection string violates its byte boundary: " +
            std::string{key} + '.'));
    }
    requirePersistedUtf8(value, key);
    return value;
}

[[nodiscard]] std::optional<std::string> optionalJsonString(
    const Json& object,
    const std::string_view key,
    const std::size_t maximumBytes,
    const bool allowEmpty)
{
    const auto found = object.find(std::string{key});
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_string()) {
        fail(integrityError(
            "An optional agent projection field is not a string: " +
            std::string{key} + '.'));
    }
    auto value = found->get<std::string>();
    if ((!allowEmpty && value.empty()) || value.size() > maximumBytes) {
        fail(integrityError(
            "An optional agent projection string violates its byte boundary: " +
            std::string{key} + '.'));
    }
    requirePersistedUtf8(value, key);
    return value;
}

[[nodiscard]] Json parseProjectionObject(
    const std::string_view body,
    const std::string_view kind)
{
    const Json parsed = Json::parse(body.begin(), body.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        fail(integrityError(
            "A persisted " + std::string{kind} +
            " projection is not a JSON object."));
    }
    return parsed;
}

[[nodiscard]] Json parseReportObject(
    const std::string_view report,
    const std::string_view field)
{
    const Json parsed = Json::parse(report.begin(), report.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        fail(integrityError(
            "Persisted " + std::string{field} +
            " is not a JSON object."));
    }
    return parsed;
}

[[nodiscard]] Domain::AgentRunRecord readRunRow(
    const WinsqliteStatement& statement)
{
    auto sessionId = persistedIdentifier<Domain::SessionId>(
        requiredText(
            statement, 0, MaximumPersistedIdentifierBytes, "session id"),
        "session id");
    auto agentId = persistedIdentifier<Domain::AgentId>(
        requiredText(
            statement, 1, MaximumPersistedIdentifierBytes, "agent id"),
        "agent id");
    auto clientText = optionalText(
        statement, 2, MaximumPersistedIdentifierBytes, "client id");
    std::optional<Domain::ClientId> clientId;
    if (clientText) {
        clientId.emplace(persistedIdentifier<Domain::ClientId>(
            *clientText, "client id"));
    }
    const auto statusText = requiredText(
        statement, 3, 16U, "session status");
    auto status = take(Domain::sessionStatusFromWire(statusText));
    auto summary = optionalText(
        statement, 4, MaximumSummaryBytes, "session summary");
    const auto createdAt = parseTimestamp(requiredText(
        statement, 5, MaximumTimestampBytes, "created timestamp"));
    const auto updatedAt = parseTimestamp(requiredText(
        statement, 6, MaximumTimestampBytes, "updated timestamp"));
    if (updatedAt < createdAt) {
        fail(integrityError(
            "A persisted agent session has decreasing timestamps."));
    }
    auto projectText = optionalText(
        statement, 7, MaximumPersistedIdentifierBytes, "project id");
    std::optional<Domain::ProjectId> projectId;
    if (projectText) {
        projectId.emplace(persistedIdentifier<Domain::ProjectId>(
            *projectText, "project id"));
    }
    auto goal = optionalText(
        statement,
        8,
        Domain::AgentSessionLimits::MaximumGoalBytes,
        "goal");
    auto cwdText = optionalText(
        statement, 9, Domain::PathText::MaximumBytes, "working directory");
    std::optional<Domain::PathText> workingDirectory;
    if (cwdText) {
        auto path = Domain::PathText::create(*cwdText);
        if (!path) {
            fail(integrityError(
                "A persisted agent working directory is invalid."));
        }
        workingDirectory.emplace(std::move(path).value());
    }
    auto reportJson = optionalText(
        statement,
        10,
        Domain::AgentSessionLimits::MaximumReportJsonBytes,
        "report JSON");
    if (reportJson) {
        static_cast<void>(parseReportObject(*reportJson, "agent report JSON"));
    }
    Domain::AgentRunRecord run{
        Domain::AgentSession{
            std::move(sessionId),
            std::move(agentId),
            std::move(clientId),
            status,
            std::move(summary),
            createdAt,
            updatedAt},
        std::move(projectId),
        std::move(goal),
        std::move(workingDirectory),
        {},
        {},
        std::move(reportJson)};
    auto valid = Domain::validateAgentRunRecord(run);
    if (!valid) {
        fail(integrityError(
            "A persisted agent run violates its semantic bounds."));
    }
    return run;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::AgentRunRecord> selectRunById(
    SqlOwner& owner,
    const Domain::SessionId& sessionId,
    const Domain::OperationContext& context)
{
    std::string sql{"SELECT "};
    sql += RunColumns;
    sql += " FROM agent_sessions WHERE id=?";
    auto statement = take(prepare(owner, sql, context));
    take(statement.bindText(1, sessionId.value()));
    if (take(statement.step()) == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    auto run = readRunRow(statement);
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        fail(integrityError(
            "A primary-key agent-session query returned duplicate rows."));
    }
    return run;
}

[[nodiscard]] std::vector<std::string> decodeTags(
    const std::string_view encoded)
{
    const Json parsed = Json::parse(
        encoded.begin(), encoded.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.size() > 16U) {
        fail(integrityError(
            "Persisted agent projection tags are malformed or oversized."));
    }
    std::vector<std::string> tags;
    tags.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) {
            fail(integrityError(
                "Persisted agent projection tags contain a non-string item."));
        }
        auto tag = item.get<std::string>();
        if (tag.empty() || tag.size() >
                Domain::AgentSessionLimits::MaximumItemBytes) {
            fail(integrityError(
                "A persisted agent projection tag violates its byte boundary."));
        }
        requirePersistedUtf8(tag, "projection tag");
        tags.push_back(std::move(tag));
    }
    return tags;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<StoredMemoryProjection> selectMemoryProjection(
    SqlOwner& owner,
    const std::string_view key,
    const Domain::OperationContext& context)
{
    auto statement = take(prepare(
        owner,
        "SELECT body,tags_json,created_at,updated_at "
        "FROM memory_notes WHERE key=?",
        context));
    take(statement.bindText(1, key));
    if (take(statement.step()) == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    StoredMemoryProjection projection{
        requiredText(statement, 0, MaximumProjectionBytes, "projection body"),
        decodeTags(requiredText(
            statement, 1, MaximumTagsJsonBytes, "projection tags")),
        parseTimestamp(requiredText(
            statement, 2, MaximumTimestampBytes, "projection created timestamp")),
        parseTimestamp(requiredText(
            statement, 3, MaximumTimestampBytes, "projection updated timestamp"))};
    if (projection.updatedAt < projection.createdAt) {
        fail(integrityError(
            "A persisted agent projection has decreasing timestamps."));
    }
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        fail(integrityError(
            "A primary-key agent projection query returned duplicate rows."));
    }
    return projection;
}

[[nodiscard]] std::string runProjectionKey(
    const Domain::SessionId& sessionId)
{
    return "agent_run/" + sessionId.value();
}

[[nodiscard]] std::string activeProjectionKey(
    const Domain::ClientId& clientId)
{
    return "agent_active/" + clientId.value();
}

[[nodiscard]] std::string encodeTags(
    const std::string_view kind,
    const Domain::AgentId& agentId)
{
    Json tags = Json::array();
    tags.push_back(kind);
    tags.push_back(agentId.value());
    return tags.dump();
}

[[nodiscard]] std::string encodeRunProjection(
    const Domain::AgentRunRecord& run)
{
    Json projection = Json::object();
    projection["session_id"] = run.session.id.value();
    projection["agent_id"] = run.session.agentId.value();
    projection["status"] = Domain::wireName(run.session.status);
    if (run.projectId) {
        projection["project_id"] = run.projectId->value();
    }
    if (run.goal) {
        projection["goal"] = *run.goal;
    }
    if (run.workingDirectory) {
        projection["cwd"] = run.workingDirectory->value();
    }
    projection["output_schema"] = run.outputSchema;
    projection["first_moves"] = run.firstMoves;
    return projection.dump();
}

[[nodiscard]] std::string encodeActiveProjection(
    const Domain::ActiveBinding& binding)
{
    Json projection = Json::object();
    projection["session_id"] = binding.sessionId.value();
    projection["agent_id"] = binding.agentId.value();
    projection["goal"] = binding.goal;
    projection["tools_primary"] = binding.toolsPrimary;
    projection["tools_forbidden"] = binding.toolsForbidden;
    projection["output_schema"] = binding.outputSchema;
    projection["done_definition"] = binding.doneDefinition;
    if (binding.workingDirectory) {
        projection["cwd"] = binding.workingDirectory->value();
    }
    return projection.dump();
}

[[nodiscard]] DecodedRunProjection decodeRunProjection(
    const StoredMemoryProjection& stored)
{
    const Json object = parseProjectionObject(stored.body, "agent run");
    auto sessionId = persistedIdentifier<Domain::SessionId>(
        requiredJsonString(
            object, "session_id", MaximumPersistedIdentifierBytes),
        "run projection session id");
    auto agentId = persistedIdentifier<Domain::AgentId>(
        requiredJsonString(
            object, "agent_id", MaximumPersistedIdentifierBytes),
        "run projection agent id");
    const auto status = take(Domain::sessionStatusFromWire(
        requiredJsonString(object, "status", 16U)));
    std::optional<Domain::ProjectId> projectId;
    if (auto project = optionalJsonString(
            object, "project_id", MaximumPersistedIdentifierBytes, false)) {
        projectId.emplace(persistedIdentifier<Domain::ProjectId>(
            *project, "run projection project id"));
    }
    auto goal = optionalJsonString(
        object,
        "goal",
        Domain::AgentSessionLimits::MaximumGoalBytes,
        true);
    std::optional<Domain::PathText> workingDirectory;
    if (auto cwd = optionalJsonString(
            object, "cwd", Domain::PathText::MaximumBytes, false)) {
        auto path = Domain::PathText::create(*cwd);
        if (!path) {
            fail(integrityError(
                "The run projection working directory is invalid."));
        }
        workingDirectory.emplace(std::move(path).value());
    }
    auto outputSchema = decodeStringArray(
        object,
        "output_schema",
        Domain::AgentSessionLimits::MaximumSchemaItems,
        false);
    auto firstMoves = decodeStringArray(
        object,
        "first_moves",
        Domain::AgentSessionLimits::MaximumBindingItems,
        false);
    // Older projections may contain a report copy. Validate but ignore it:
    // central-v6 report_json is authoritative and can consume the full report
    // budget without making the bounded legacy note exceed that same budget.
    const auto report = object.find("report");
    if (report != object.end() && !report->is_null()) {
        if (!report->is_object()) {
            fail(integrityError(
                "The run projection report is not a JSON object."));
        }
        auto encoded = report->dump();
        if (encoded.size() > Domain::AgentSessionLimits::MaximumReportJsonBytes) {
            fail(integrityError(
                "The run projection report exceeds its byte boundary."));
        }
    }
    return DecodedRunProjection{
        std::move(sessionId),
        std::move(agentId),
        status,
        std::move(projectId),
        std::move(goal),
        std::move(workingDirectory),
        std::move(outputSchema),
        std::move(firstMoves)};
}

[[nodiscard]] Domain::ActiveBinding decodeActiveProjection(
    const StoredMemoryProjection& stored)
{
    const Json object = parseProjectionObject(stored.body, "active agent");
    auto sessionId = persistedIdentifier<Domain::SessionId>(
        requiredJsonString(
            object, "session_id", MaximumPersistedIdentifierBytes),
        "active projection session id");
    auto agentId = persistedIdentifier<Domain::AgentId>(
        requiredJsonString(
            object, "agent_id", MaximumPersistedIdentifierBytes),
        "active projection agent id");
    auto goal = optionalJsonString(
        object,
        "goal",
        Domain::AgentSessionLimits::MaximumGoalBytes,
        true).value_or(std::string{});
    auto toolsPrimary = decodeStringArray(
        object,
        "tools_primary",
        Domain::AgentSessionLimits::MaximumBindingItems,
        false);
    auto toolsForbidden = decodeStringArray(
        object,
        "tools_forbidden",
        Domain::AgentSessionLimits::MaximumBindingItems,
        false);
    auto outputSchema = decodeStringArray(
        object,
        "output_schema",
        Domain::AgentSessionLimits::MaximumBindingItems,
        false);
    auto doneDefinition = decodeStringArray(
        object,
        "done_definition",
        Domain::AgentSessionLimits::MaximumBindingItems,
        false);
    std::optional<Domain::PathText> workingDirectory;
    if (auto cwd = optionalJsonString(
            object, "cwd", Domain::PathText::MaximumBytes, false)) {
        auto path = Domain::PathText::create(*cwd);
        if (!path) {
            fail(integrityError(
                "The active projection working directory is invalid."));
        }
        workingDirectory.emplace(std::move(path).value());
    }
    Domain::ActiveBinding binding{
        std::move(sessionId),
        std::move(agentId),
        std::move(goal),
        std::move(toolsPrimary),
        std::move(toolsForbidden),
        std::move(outputSchema),
        std::move(doneDefinition),
        std::move(workingDirectory)};
    auto valid = Domain::validateActiveBinding(binding);
    if (!valid) {
        fail(integrityError(
            "A persisted active-agent projection violates semantic bounds."));
    }
    return binding;
}

[[nodiscard]] bool exactProjectionTags(
    const StoredMemoryProjection& projection,
    const std::string_view kind,
    const Domain::AgentId& agentId)
{
    return projection.tags.size() == 2U &&
        projection.tags[0] == kind &&
        projection.tags[1] == agentId.value();
}

[[nodiscard]] bool optionalJsonEqual(
    const std::optional<std::string>& left,
    const std::optional<std::string>& right)
{
    if (!left || !right) {
        return left.has_value() == right.has_value();
    }
    return parseReportObject(*left, "agent report JSON") ==
        parseReportObject(*right, "agent report JSON");
}

[[nodiscard]] bool runProjectionMatches(
    const DecodedRunProjection& projection,
    const Domain::AgentRunRecord& run)
{
    return projection.sessionId == run.session.id &&
        projection.agentId == run.session.agentId &&
        projection.status == run.session.status &&
        projection.projectId == run.projectId &&
        projection.goal == run.goal &&
        projection.workingDirectory == run.workingDirectory;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::AgentRunRecord> selectEnrichedRunById(
    SqlOwner& owner,
    const Domain::SessionId& sessionId,
    const Domain::OperationContext& context,
    bool* const projectionConsistent = nullptr)
{
    auto run = selectRunById(owner, sessionId, context);
    if (!run) {
        if (projectionConsistent) {
            *projectionConsistent = false;
        }
        return std::nullopt;
    }
    const auto projection = selectMemoryProjection(
        owner, runProjectionKey(sessionId), context);
    bool consistent{};
    if (projection) {
        const auto decoded = decodeRunProjection(*projection);
        consistent = exactProjectionTags(
                         *projection, "agent_run", run->session.agentId) &&
            runProjectionMatches(decoded, *run);
        if (consistent) {
            run->outputSchema = decoded.outputSchema;
            run->firstMoves = decoded.firstMoves;
        }
    }
    if (projectionConsistent) {
        *projectionConsistent = consistent;
    }
    return run;
}

template <typename SqlOwner>
void upsertMemoryProjection(
    SqlOwner& owner,
    const std::string_view key,
    const std::string_view body,
    const std::string_view tagsJson,
    const Domain::UtcTimePoint timestamp,
    const Domain::OperationContext& context)
{
    requireInputText(key, Domain::LegacyMemoryLimits::MaximumKeyBytes, false, "Projection key");
    requireInputText(body, MaximumProjectionBytes, false, "Projection body");
    requireInputText(tagsJson, MaximumTagsJsonBytes, false, "Projection tags");
    const auto timestampValue = timestampText(timestamp);
    auto statement = take(prepare(
        owner,
        "INSERT INTO memory_notes(key,body,tags_json,created_at,updated_at) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET "
        "body=excluded.body,tags_json=excluded.tags_json,"
        "updated_at=CASE "
        "WHEN julianday(excluded.updated_at)>julianday(memory_notes.updated_at) "
        "AND julianday(excluded.updated_at)>julianday(memory_notes.created_at) "
        "THEN excluded.updated_at "
        "WHEN julianday(memory_notes.updated_at)>=julianday(memory_notes.created_at) "
        "THEN memory_notes.updated_at ELSE memory_notes.created_at END",
        context));
    take(statement.bindText(1, key));
    take(statement.bindText(2, body));
    take(statement.bindText(3, tagsJson));
    take(statement.bindText(4, timestampValue));
    take(statement.bindText(5, timestampValue));
    stepDone(statement);
}

template <typename SqlOwner>
void writeRunProjection(
    SqlOwner& owner,
    const Domain::AgentRunRecord& run,
    const Domain::UtcTimePoint timestamp,
    const Domain::OperationContext& context)
{
    upsertMemoryProjection(
        owner,
        runProjectionKey(run.session.id),
        encodeRunProjection(run),
        encodeTags("agent_run", run.session.agentId),
        timestamp,
        context);
}

template <typename SqlOwner>
void writeActiveProjection(
    SqlOwner& owner,
    const Domain::ClientId& clientId,
    const Domain::ActiveBinding& binding,
    const Domain::UtcTimePoint timestamp,
    const Domain::OperationContext& context)
{
    upsertMemoryProjection(
        owner,
        activeProjectionKey(clientId),
        encodeActiveProjection(binding),
        encodeTags("agent_active", binding.agentId),
        timestamp,
        context);
}

template <typename SqlOwner>
[[nodiscard]] bool deleteMatchingActiveProjection(
    SqlOwner& owner,
    const Domain::ClientId& clientId,
    const Domain::SessionId& sessionId,
    const Domain::OperationContext& context)
{
    const auto key = activeProjectionKey(clientId);
    const auto stored = selectMemoryProjection(owner, key, context);
    if (!stored) {
        return false;
    }
    const auto binding = decodeActiveProjection(*stored);
    if (binding.sessionId != sessionId) {
        return false;
    }
    auto deletion = take(prepare(
        owner,
        "DELETE FROM memory_notes WHERE key=? AND body=?",
        context));
    take(deletion.bindText(1, key));
    take(deletion.bindText(2, stored->body));
    stepDone(deletion);
    return changes(owner, context) == 1;
}

void bindRunColumns(
    WinsqliteStatement& statement,
    const Domain::AgentRunRecord& run)
{
    take(statement.bindText(1, run.session.id.value()));
    take(statement.bindText(2, run.session.agentId.value()));
    if (run.session.clientId) {
        take(statement.bindText(3, run.session.clientId->value()));
    } else {
        take(statement.bindNull(3));
    }
    take(statement.bindText(4, Domain::wireName(run.session.status)));
    if (run.session.summary) {
        take(statement.bindText(5, *run.session.summary));
    } else {
        take(statement.bindNull(5));
    }
    take(statement.bindText(6, timestampText(run.session.createdAt)));
    take(statement.bindText(7, timestampText(run.session.updatedAt)));
    if (run.projectId) {
        take(statement.bindText(8, run.projectId->value()));
    } else {
        take(statement.bindNull(8));
    }
    if (run.goal) {
        take(statement.bindText(9, *run.goal));
    } else {
        take(statement.bindNull(9));
    }
    if (run.workingDirectory) {
        take(statement.bindText(10, run.workingDirectory->value()));
    } else {
        take(statement.bindNull(10));
    }
    if (run.reportJson) {
        take(statement.bindText(11, *run.reportJson));
    } else {
        take(statement.bindNull(11));
    }
}

template <typename SqlOwner>
[[nodiscard]] std::vector<Domain::AgentRunRecord> selectOpenRunsForClient(
    SqlOwner& owner,
    const Domain::ClientId& clientId,
    const std::optional<Domain::SessionId>& except,
    const Domain::OperationContext& context)
{
    std::string sql{"SELECT "};
    sql += RunColumns;
    sql += " FROM agent_sessions WHERE client_id=? AND ";
    sql += OpenStatusPredicate;
    if (except) {
        sql += " AND id<>?";
    }
    sql += " ORDER BY julianday(updated_at) DESC,id DESC LIMIT ?";
    auto statement = take(prepare(owner, sql, context));
    take(statement.bindText(1, clientId.value()));
    int parameter = 2;
    if (except) {
        take(statement.bindText(parameter++, except->value()));
    }
    take(statement.bindInt64(
        parameter, static_cast<std::int64_t>(MaximumBoundedRows + 1U)));
    std::vector<Domain::AgentRunRecord> runs;
    for (;;) {
        const auto stepped = take(statement.step());
        if (stepped == WinsqliteStepResult::Done) {
            break;
        }
        if (runs.size() == MaximumBoundedRows) {
            fail(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Open agent sessions exceed the bounded transaction limit."));
        }
        runs.push_back(readRunRow(statement));
    }
    return runs;
}

[[nodiscard]] std::size_t closeClientRuns(
    WinsqliteTransaction& transaction,
    const Domain::ClientId& clientId,
    const std::optional<Domain::SessionId>& except,
    const std::string_view summary,
    const Domain::UtcTimePoint closedAt,
    const Domain::OperationContext& context)
{
    auto runs = selectOpenRunsForClient(
        transaction, clientId, except, context);
    for (auto& run : runs) {
        const auto effectiveClosedAt =
            (std::max)(closedAt, run.session.updatedAt);
        const auto timestamp = timestampText(effectiveClosedAt);
        auto enriched = selectEnrichedRunById(
            transaction, run.session.id, context);
        if (enriched) {
            run.outputSchema = std::move(enriched->outputSchema);
            run.firstMoves = std::move(enriched->firstMoves);
        }
        auto update = take(transaction.prepare(
            "UPDATE agent_sessions SET status='closed',summary=?,updated_at=? "
            "WHERE id=? AND status IN ('open','active','running','started')"));
        take(update.bindText(1, summary));
        take(update.bindText(2, timestamp));
        take(update.bindText(3, run.session.id.value()));
        stepDone(update);
        if (changes(transaction, context) != 1) {
            fail(integrityError(
                "An open agent session changed during serialized closure."));
        }
        run.session.status = Domain::SessionStatus::Closed;
        run.session.summary = std::string{summary};
        run.session.updatedAt = effectiveClosedAt;
        writeRunProjection(transaction, run, effectiveClosedAt, context);
        static_cast<void>(deleteMatchingActiveProjection(
            transaction, clientId, run.session.id, context));
    }
    return runs.size();
}

void notifyObserver(
    IAgentSessionTransactionObserver* const observer,
    const AgentSessionTransactionKind kind,
    const AgentSessionTransactionCheckpoint checkpoint) noexcept
{
    if (observer) {
        observer->onAgentSessionTransactionCheckpoint(kind, checkpoint);
    }
}

[[nodiscard]] Detail::WindowsDatabaseStore& requireStore(
    Detail::WindowsDatabaseStore* const store)
{
    if (!store) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The agent-session repository is closed."));
    }
    return *store;
}

} // namespace

struct WindowsAgentSessionRepository::Impl final {
    Impl(
        std::shared_ptr<WindowsCentralDatabase> ownedDatabase,
        const bool closeOwnedDatabase,
        std::shared_ptr<Contracts::IClock> ownedClock,
        IAgentSessionTransactionObserver* const observer) noexcept
        : database{std::move(ownedDatabase)},
          closeDatabaseOnClose{closeOwnedDatabase},
          clock{std::move(ownedClock)},
          transactionObserver{observer}
    {
    }

    [[nodiscard]] Detail::WindowsDatabaseStore* repositoryStore() noexcept
    {
        return !closed.load(std::memory_order_acquire) && database
            ? database->repositoryStore()
            : nullptr;
    }

    std::shared_ptr<WindowsCentralDatabase> database;
    bool closeDatabaseOnClose{};
    std::shared_ptr<Contracts::IClock> clock;
    IAgentSessionTransactionObserver* transactionObserver{};
    std::atomic_bool closed{};
};

WindowsAgentSessionRepository::WindowsAgentSessionRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsAgentSessionRepository::~WindowsAgentSessionRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>
WindowsAgentSessionRepository::open(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IClock> clock,
    const Domain::OperationContext& context) noexcept
{
    if (!applicationPaths || !runtimeDiagnostics || !clock) {
        return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The agent-session repository requires paths, diagnostics, and a clock."));
    }
    auto database = WindowsCentralDatabase::open(
        std::move(applicationPaths),
        std::move(runtimeDiagnostics),
        clock,
        context);
    if (!database) {
        return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::failure(
            std::move(database).error());
    }
    return create(std::move(database).value(), std::move(clock));
}

Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>
WindowsAgentSessionRepository::create(
    std::unique_ptr<WindowsCentralDatabase> database,
    std::shared_ptr<Contracts::IClock> clock,
    IAgentSessionTransactionObserver* const transactionObserver) noexcept
{
    try {
        if (!database || !clock) {
            return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The agent-session repository requires an owned database and clock."));
        }
        auto implementation = std::make_unique<Impl>(
            std::shared_ptr<WindowsCentralDatabase>{std::move(database)},
            true,
            std::move(clock),
            transactionObserver);
        return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::success(
            std::shared_ptr<WindowsAgentSessionRepository>{
                new WindowsAgentSessionRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The agent-session repository could not be constructed."));
    }
}

Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>
WindowsAgentSessionRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database,
    std::shared_ptr<Contracts::IClock> clock) noexcept
{
    try {
        if (!database || !clock) {
            return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The agent-session repository requires a shared database and clock."));
        }
        auto implementation = std::make_unique<Impl>(
            std::move(database), false, std::move(clock), nullptr);
        return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::success(
            std::shared_ptr<WindowsAgentSessionRepository>{
                new WindowsAgentSessionRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsAgentSessionRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The shared agent-session repository could not be constructed."));
    }
}

Domain::Result<void> WindowsAgentSessionRepository::save(
    const Domain::AgentSession& session,
    const Domain::OperationContext& context) noexcept
{
    return guarded<void>([&]() {
        validateSession(session);
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        take(runOnStore<void>(
            store,
            "Save agent session",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<void>([&]() {
                    auto statement = take(connection.prepare(
                        "INSERT INTO agent_sessions("
                        "id,agent_id,client_id,status,summary,created_at,updated_at) "
                        "VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                        "agent_id=excluded.agent_id,client_id=excluded.client_id,"
                        "status=excluded.status,summary=excluded.summary,"
                        "created_at=excluded.created_at,updated_at=excluded.updated_at",
                        context));
                    take(statement.bindText(1, session.id.value()));
                    take(statement.bindText(2, session.agentId.value()));
                    if (session.clientId) {
                        take(statement.bindText(3, session.clientId->value()));
                    } else {
                        take(statement.bindNull(3));
                    }
                    take(statement.bindText(4, Domain::wireName(session.status)));
                    if (session.summary) {
                        take(statement.bindText(5, *session.summary));
                    } else {
                        take(statement.bindNull(5));
                    }
                    take(statement.bindText(6, timestampText(session.createdAt)));
                    take(statement.bindText(7, timestampText(session.updatedAt)));
                    stepDone(statement);
                });
            }));
    });
}

Domain::Result<std::optional<Domain::AgentSession>>
WindowsAgentSessionRepository::get(
    const Domain::SessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::AgentSession>>([&]() {
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<std::optional<Domain::AgentSession>>(
            store,
            "Get agent session",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::AgentSession>>([&]() {
                    auto run = selectRunById(connection, sessionId, context);
                    if (!run) {
                        return std::optional<Domain::AgentSession>{};
                    }
                    return std::optional<Domain::AgentSession>{
                        std::move(run->session)};
                });
            }));
    });
}

Domain::Result<std::vector<Domain::AgentSession>>
WindowsAgentSessionRepository::list(
    const std::optional<Domain::AgentId>& agentId,
    const std::optional<Domain::SessionStatus>& status,
    const std::size_t maximumCount,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::vector<Domain::AgentSession>>([&]() {
        if (maximumCount == 0U || maximumCount > MaximumBoundedRows ||
            (status && !knownStatus(*status))) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The agent-session list limit or status is invalid."));
        }
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<std::vector<Domain::AgentSession>>(
            store,
            "List agent sessions",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::vector<Domain::AgentSession>>([&]() {
                    std::string sql{"SELECT "};
                    sql += RunColumns;
                    sql += " FROM agent_sessions WHERE 1=1";
                    if (agentId) {
                        sql += " AND agent_id=?";
                    }
                    if (status) {
                        sql += " AND status=?";
                    }
                    sql += " ORDER BY julianday(created_at) DESC,id DESC LIMIT ?";
                    auto statement = take(connection.prepare(sql, context));
                    int parameter = 1;
                    if (agentId) {
                        take(statement.bindText(parameter++, agentId->value()));
                    }
                    if (status) {
                        take(statement.bindText(
                            parameter++, Domain::wireName(*status)));
                    }
                    take(statement.bindInt64(
                        parameter, static_cast<std::int64_t>(maximumCount)));
                    std::vector<Domain::AgentSession> sessions;
                    sessions.reserve(maximumCount);
                    for (;;) {
                        if (take(statement.step()) == WinsqliteStepResult::Done) {
                            break;
                        }
                        sessions.push_back(readRunRow(statement).session);
                    }
                    return sessions;
                });
            }));
    });
}

Domain::Result<Domain::AgentRunStartPersistenceOutcome>
WindowsAgentSessionRepository::startRun(
    const Domain::AgentRunStartMutation& mutation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::AgentRunStartPersistenceOutcome>([&]() {
        validateRun(mutation.run);
        validateSummary(mutation.supersedeSummary, "Agent supersede summary");
        const bool hasClient = mutation.run.session.clientId.has_value();
        if (hasClient != mutation.activeBinding.has_value() || !mutation.run.goal ||
            mutation.run.session.status != Domain::SessionStatus::Open ||
            mutation.run.session.summary || mutation.run.reportJson ||
            mutation.run.session.createdAt != mutation.run.session.updatedAt) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A new agent run must be open, uncompleted, binding-consistent, and timestamp-consistent."));
        }
        if (mutation.activeBinding) {
            take(Domain::validateActiveBinding(*mutation.activeBinding));
            if (mutation.activeBinding->sessionId != mutation.run.session.id ||
                mutation.activeBinding->agentId != mutation.run.session.agentId ||
                mutation.activeBinding->goal != *mutation.run.goal ||
                mutation.activeBinding->workingDirectory !=
                    mutation.run.workingDirectory ||
                mutation.activeBinding->outputSchema != mutation.run.outputSchema) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The active binding does not describe the new agent run."));
            }
        }
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        auto* const observer = implementation_->transactionObserver;
        return take(runOnStore<Domain::AgentRunStartPersistenceOutcome>(
            store,
            "Start durable agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::AgentRunStartPersistenceOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    const auto superseded = mutation.run.session.clientId
                        ? closeClientRuns(
                              transaction,
                              *mutation.run.session.clientId,
                              std::nullopt,
                              mutation.supersedeSummary,
                              mutation.run.session.createdAt,
                              context)
                        : 0U;
                    auto insert = take(transaction.prepare(
                        "INSERT INTO agent_sessions("
                        "id,agent_id,client_id,status,summary,created_at,updated_at,"
                        "project_id,goal,cwd,report_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
                    bindRunColumns(insert, mutation.run);
                    stepDone(insert);
                    writeRunProjection(
                        transaction,
                        mutation.run,
                        mutation.run.session.updatedAt,
                        context);
                    if (mutation.run.session.clientId && mutation.activeBinding) {
                        writeActiveProjection(
                            transaction,
                            *mutation.run.session.clientId,
                            *mutation.activeBinding,
                            mutation.run.session.updatedAt,
                            context);
                    }
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::Start,
                        AgentSessionTransactionCheckpoint::BeforeCommit);
                    take(transaction.commit());
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::Start,
                        AgentSessionTransactionCheckpoint::AfterCommit);
                    return Domain::AgentRunStartPersistenceOutcome{
                        mutation.run, mutation.activeBinding, superseded};
                });
            }));
    });
}

Domain::Result<std::optional<Domain::AgentRunRecord>>
WindowsAgentSessionRepository::getRun(
    const Domain::SessionId& sessionId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::AgentRunRecord>>([&]() {
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<std::optional<Domain::AgentRunRecord>>(
            store,
            "Get durable agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::AgentRunRecord>>([&]() {
                    return selectEnrichedRunById(
                        connection, sessionId, context);
                });
            }));
    });
}

Domain::Result<Domain::AgentRunReattachOutcome>
WindowsAgentSessionRepository::reattachRun(
    const Domain::AgentRunReattachMutation& mutation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::AgentRunReattachOutcome>([&]() {
        take(Domain::validateActiveBinding(mutation.binding));
        validateSummary(mutation.supersedeSummary, "Agent reattach summary");
        if (mutation.binding.sessionId != mutation.sessionId) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The reattach binding identifies another agent session."));
        }
        static_cast<void>(timestampText(mutation.attachedAt));
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        auto* const observer = implementation_->transactionObserver;
        return take(runOnStore<Domain::AgentRunReattachOutcome>(
            store,
            "Reattach durable agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::AgentRunReattachOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto existing = selectEnrichedRunById(
                        transaction, mutation.sessionId, context);
                    if (!existing) {
                        fail(sessionNotFound(mutation.sessionId));
                    }
                    if (!Domain::isOpen(existing->session.status)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The requested agent session is not open."));
                    }
                    if (existing->session.clientId != mutation.expectedClientId) {
                        fail(ownershipConflict(mutation.sessionId));
                    }
                    if (existing->session.agentId != mutation.binding.agentId ||
                        !existing->goal ||
                        mutation.binding.goal != *existing->goal ||
                        mutation.binding.workingDirectory !=
                            existing->workingDirectory) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::InvalidRequest,
                            "The reattach binding conflicts with durable run metadata."));
                    }
                    const auto effectiveAttachedAt =
                        (std::max)(mutation.attachedAt, existing->session.updatedAt);
                    const auto previousClient = existing->session.clientId;
                    const auto superseded = closeClientRuns(
                        transaction,
                        mutation.clientId,
                        mutation.sessionId,
                        mutation.supersedeSummary,
                        mutation.attachedAt,
                        context);
                    auto update = take(transaction.prepare(
                        mutation.expectedClientId
                            ? "UPDATE agent_sessions SET client_id=?,updated_at=? "
                              "WHERE id=? AND client_id=? AND "
                              "status IN ('open','active','running','started')"
                            : "UPDATE agent_sessions SET client_id=?,updated_at=? "
                              "WHERE id=? AND client_id IS NULL AND "
                              "status IN ('open','active','running','started')"));
                    take(update.bindText(1, mutation.clientId.value()));
                    take(update.bindText(2, timestampText(effectiveAttachedAt)));
                    take(update.bindText(3, mutation.sessionId.value()));
                    if (mutation.expectedClientId) {
                        take(update.bindText(4, mutation.expectedClientId->value()));
                    }
                    stepDone(update);
                    if (changes(transaction, context) != 1) {
                        fail(ownershipConflict(mutation.sessionId));
                    }
                    if (previousClient && *previousClient != mutation.clientId) {
                        static_cast<void>(deleteMatchingActiveProjection(
                            transaction,
                            *previousClient,
                            mutation.sessionId,
                            context));
                    }
                    writeActiveProjection(
                        transaction,
                        mutation.clientId,
                        mutation.binding,
                        effectiveAttachedAt,
                        context);
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::Reattach,
                        AgentSessionTransactionCheckpoint::BeforeCommit);
                    take(transaction.commit());
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::Reattach,
                        AgentSessionTransactionCheckpoint::AfterCommit);
                    existing->session.clientId = mutation.clientId;
                    existing->session.updatedAt = effectiveAttachedAt;
                    return Domain::AgentRunReattachOutcome{
                        std::move(*existing),
                        mutation.binding,
                        previousClient,
                        superseded,
                        previousClient !=
                            std::optional<Domain::ClientId>{mutation.clientId}};
                });
            }));
    });
}

Domain::Result<Domain::AgentRunCompletePersistenceOutcome>
WindowsAgentSessionRepository::completeRun(
    const Domain::AgentRunCompleteMutation& mutation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::AgentRunCompletePersistenceOutcome>([&]() {
        requireInputText(
            mutation.reportJson,
            Domain::AgentSessionLimits::MaximumReportJsonBytes,
            false,
            "Agent report JSON");
        static_cast<void>(parseReportObject(
            mutation.reportJson, "agent report JSON"));
        validateSummary(mutation.summary, "Agent completion summary");
        if (mutation.missingSchemaKeys.size() >
            Domain::AgentSessionLimits::MaximumSchemaItems) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Missing agent schema keys exceed their item limit."));
        }
        for (const auto& key : mutation.missingSchemaKeys) {
            requireInputText(
                key,
                Domain::AgentSessionLimits::MaximumItemBytes,
                false,
                "Missing agent schema key");
        }
        static_cast<void>(timestampText(mutation.completedAt));
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        auto* const observer = implementation_->transactionObserver;
        return take(runOnStore<Domain::AgentRunCompletePersistenceOutcome>(
            store,
            "Complete durable agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::AgentRunCompletePersistenceOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto existing = selectEnrichedRunById(
                        transaction, mutation.sessionId, context);
                    if (!existing) {
                        fail(sessionNotFound(mutation.sessionId));
                    }
                    if (mutation.expectedClientId &&
                        existing->session.clientId != mutation.expectedClientId) {
                        fail(ownershipConflict(mutation.sessionId));
                    }
                    const bool alreadyCommitted =
                        existing->session.status == Domain::SessionStatus::Closed &&
                        existing->session.summary ==
                            std::optional<std::string>{mutation.summary} &&
                        optionalJsonEqual(
                            existing->reportJson,
                            std::optional<std::string>{mutation.reportJson});
                    if (!Domain::isOpen(existing->session.status) &&
                        !alreadyCommitted) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The agent session has already completed differently."));
                    }
                    const auto effectiveCompletedAt =
                        (std::max)(mutation.completedAt, existing->session.updatedAt);
                    if (!alreadyCommitted) {
                        auto update = take(transaction.prepare(
                            "UPDATE agent_sessions SET "
                            "status='closed',summary=?,report_json=?,updated_at=? "
                            "WHERE id=? AND "
                            "status IN ('open','active','running','started')"));
                        take(update.bindText(1, mutation.summary));
                        take(update.bindText(2, mutation.reportJson));
                        take(update.bindText(3, timestampText(effectiveCompletedAt)));
                        take(update.bindText(4, mutation.sessionId.value()));
                        stepDone(update);
                        if (changes(transaction, context) != 1) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "The agent session changed during completion."));
                        }
                    }
                    if (!alreadyCommitted) {
                        existing->session.status = Domain::SessionStatus::Closed;
                        existing->session.summary = mutation.summary;
                        existing->session.updatedAt = effectiveCompletedAt;
                        existing->reportJson = mutation.reportJson;
                    }
                    const bool cleared = existing->session.clientId
                        ? deleteMatchingActiveProjection(
                              transaction,
                              *existing->session.clientId,
                              mutation.sessionId,
                              context)
                        : false;
                    writeRunProjection(
                        transaction,
                        *existing,
                        existing->session.updatedAt,
                        context);
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::Complete,
                        AgentSessionTransactionCheckpoint::BeforeCommit);
                    take(transaction.commit());
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::Complete,
                        AgentSessionTransactionCheckpoint::AfterCommit);
                    return Domain::AgentRunCompletePersistenceOutcome{
                        std::move(*existing), cleared};
                });
            }));
    });
}

Domain::Result<bool> WindowsAgentSessionRepository::touchRun(
    const Domain::SessionId& sessionId,
    const Domain::UtcTimePoint touchedAt,
    const Domain::OperationContext& context) noexcept
{
    return guarded<bool>([&]() {
        const auto timestamp = timestampText(touchedAt);
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<bool>(
            store,
            "Touch durable agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<bool>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto run = selectRunById(transaction, sessionId, context);
                    if (!run || !Domain::isOpen(run->session.status)) {
                        take(transaction.rollback());
                        return false;
                    }
                    if (touchedAt < run->session.updatedAt) {
                        take(transaction.rollback());
                        return false;
                    }
                    auto statement = take(transaction.prepare(
                        "UPDATE agent_sessions SET updated_at=? WHERE id=? AND "
                        "status IN ('open','active','running','started')"));
                    take(statement.bindText(1, timestamp));
                    take(statement.bindText(2, sessionId.value()));
                    stepDone(statement);
                    const bool touched = changes(transaction, context) == 1;
                    take(transaction.commit());
                    return touched;
                });
            }));
    });
}

Domain::Result<std::optional<Domain::AgentRunRecord>>
WindowsAgentSessionRepository::latestOpenRun(
    const Domain::ClientId& clientId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::AgentRunRecord>>([&]() {
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<std::optional<Domain::AgentRunRecord>>(
            store,
            "Find latest open agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::AgentRunRecord>>([&]() {
                    std::string sql{"SELECT "};
                    sql += RunColumns;
                    sql += " FROM agent_sessions WHERE client_id=? AND ";
                    sql += OpenStatusPredicate;
                    sql += " ORDER BY julianday(updated_at) DESC,id DESC LIMIT 1";
                    std::optional<Domain::AgentRunRecord> selected;
                    {
                        auto statement = take(connection.prepare(sql, context));
                        take(statement.bindText(1, clientId.value()));
                        if (take(statement.step()) == WinsqliteStepResult::Done) {
                            return std::optional<Domain::AgentRunRecord>{};
                        }
                        selected.emplace(readRunRow(statement));
                    }
                    auto enriched = selectEnrichedRunById(
                        connection, selected->session.id, context);
                    return enriched ? std::move(enriched) :
                        std::move(selected);
                });
            }));
    });
}

Domain::Result<Domain::AgentRunRecoveryOutcome>
WindowsAgentSessionRepository::recoverRun(
    const Domain::AgentRunRecoveryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::AgentRunRecoveryOutcome>([&]() {
        take(Domain::validateAgentRunRecoveryRequest(request));
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<Domain::AgentRunRecoveryOutcome>(
            store,
            "Recover durable agent run",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::AgentRunRecoveryOutcome>([&]() {
                    const auto stored = selectMemoryProjection(
                        connection,
                        activeProjectionKey(request.clientId),
                        context);
                    if (stored) {
                        auto binding = decodeActiveProjection(*stored);
                        bool runProjectionConsistent{};
                        auto run = selectEnrichedRunById(
                            connection,
                            binding.sessionId,
                            context,
                            &runProjectionConsistent);
                        if (run && run->goal &&
                            Domain::isOpen(run->session.status) &&
                            run->session.clientId ==
                                std::optional<Domain::ClientId>{request.clientId} &&
                            run->session.agentId == binding.agentId) {
                            const bool bindingConsistent =
                                binding.goal == *run->goal &&
                                binding.workingDirectory == run->workingDirectory &&
                                (run->outputSchema.empty() ||
                                 binding.outputSchema == run->outputSchema);
                            const bool activeTagsMatch = exactProjectionTags(
                                *stored,
                                "agent_active",
                                run->session.agentId);
                            return Domain::AgentRunRecoveryOutcome{
                                std::move(run),
                                std::optional<Domain::ActiveBinding>{
                                    std::move(binding)},
                                true,
                                !runProjectionConsistent || !bindingConsistent ||
                                    !activeTagsMatch};
                        }
                    }

                    std::string sql{"SELECT "};
                    sql += RunColumns;
                    sql += " FROM agent_sessions WHERE client_id=? AND ";
                    sql += OpenStatusPredicate;
                    sql += " ORDER BY julianday(updated_at) DESC,id DESC LIMIT 1";
                    std::optional<Domain::AgentRunRecord> selected;
                    {
                        auto statement = take(connection.prepare(sql, context));
                        take(statement.bindText(1, request.clientId.value()));
                        if (take(statement.step()) == WinsqliteStepResult::Done) {
                            return Domain::AgentRunRecoveryOutcome{
                                std::nullopt,
                                std::nullopt,
                                false,
                                stored.has_value()};
                        }
                        selected.emplace(readRunRow(statement));
                    }
                    auto fallback = std::move(*selected);
                    auto enriched = selectEnrichedRunById(
                        connection,
                        fallback.session.id,
                        context);
                    if (enriched) {
                        fallback = std::move(*enriched);
                    }
                    return Domain::AgentRunRecoveryOutcome{
                        std::optional<Domain::AgentRunRecord>{
                            std::move(fallback)},
                        std::nullopt,
                        false,
                        true};
                });
            }));
    });
}

Domain::Result<Domain::AgentProjectionRepairOutcome>
WindowsAgentSessionRepository::repairProjection(
    const Domain::AgentProjectionRepairRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::AgentProjectionRepairOutcome>([&]() {
        validateRun(request.run);
        take(Domain::validateActiveBinding(request.binding));
        if (!request.run.goal || !Domain::isOpen(request.run.session.status) ||
            request.run.session.clientId !=
                std::optional<Domain::ClientId>{request.clientId} ||
            request.binding.sessionId != request.run.session.id ||
            request.binding.agentId != request.run.session.agentId ||
            request.binding.goal != *request.run.goal ||
            request.binding.workingDirectory != request.run.workingDirectory) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The projection-repair request is internally inconsistent."));
        }
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        auto* const observer = implementation_->transactionObserver;
        return take(runOnStore<Domain::AgentProjectionRepairOutcome>(
            store,
            "Repair durable agent projection",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::AgentProjectionRepairOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto authoritative = selectRunById(
                        transaction, request.run.session.id, context);
                    if (!authoritative) {
                        fail(sessionNotFound(request.run.session.id));
                    }
                    if (!Domain::isOpen(authoritative->session.status) ||
                        authoritative->session.clientId !=
                            std::optional<Domain::ClientId>{request.clientId} ||
                        authoritative->session.agentId != request.run.session.agentId ||
                        authoritative->projectId != request.run.projectId ||
                        authoritative->goal != request.run.goal ||
                        authoritative->workingDirectory !=
                            request.run.workingDirectory ||
                        !optionalJsonEqual(
                            authoritative->reportJson, request.run.reportJson)) {
                        fail(ownershipConflict(request.run.session.id));
                    }
                    const auto runBody = encodeRunProjection(request.run);
                    const auto activeBody = encodeActiveProjection(request.binding);
                    const auto existingRun = selectMemoryProjection(
                        transaction,
                        runProjectionKey(request.run.session.id),
                        context);
                    const auto existingActive = selectMemoryProjection(
                        transaction,
                        activeProjectionKey(request.clientId),
                        context);
                    const bool repaired =
                        !existingRun || existingRun->body != runBody ||
                        !exactProjectionTags(
                            *existingRun,
                            "agent_run",
                            request.run.session.agentId) ||
                        !existingActive || existingActive->body != activeBody ||
                        !exactProjectionTags(
                            *existingActive,
                            "agent_active",
                            request.run.session.agentId);
                    if (repaired) {
                        writeRunProjection(
                            transaction,
                            request.run,
                            request.run.session.updatedAt,
                            context);
                        writeActiveProjection(
                            transaction,
                            request.clientId,
                            request.binding,
                            request.run.session.updatedAt,
                            context);
                    }
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::RepairProjection,
                        AgentSessionTransactionCheckpoint::BeforeCommit);
                    take(transaction.commit());
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::RepairProjection,
                        AgentSessionTransactionCheckpoint::AfterCommit);
                    return Domain::AgentProjectionRepairOutcome{
                        request.binding, repaired};
                });
            }));
    });
}

Domain::Result<Domain::AgentStaleCloseOutcome>
WindowsAgentSessionRepository::closeStale(
    const Domain::AgentStaleCloseRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::AgentStaleCloseOutcome>([&]() {
        if (request.maximumCount == 0U ||
            request.maximumCount > MaximumBoundedRows ||
            request.now < request.cutoff) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The stale-agent closure bounds or timestamps are invalid."));
        }
        const auto cutoff = timestampText(request.cutoff);
        static_cast<void>(timestampText(request.now));
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        auto* const observer = implementation_->transactionObserver;
        return take(runOnStore<Domain::AgentStaleCloseOutcome>(
            store,
            "Close stale durable agent runs",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::AgentStaleCloseOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    std::string sql{"SELECT "};
                    sql += RunColumns;
                    sql += " FROM agent_sessions WHERE ";
                    sql += OpenStatusPredicate;
                    std::vector<Domain::AgentRunRecord> selected;
                    selected.reserve(request.maximumCount);
                    {
                        sql += " AND julianday(updated_at)<julianday(?) "
                               "ORDER BY julianday(updated_at) ASC,id ASC LIMIT ?";
                        auto statement = take(transaction.prepare(sql));
                        take(statement.bindText(1, cutoff));
                        take(statement.bindInt64(
                            2, static_cast<std::int64_t>(request.maximumCount)));
                        for (;;) {
                            if (take(statement.step()) == WinsqliteStepResult::Done) {
                                break;
                            }
                            selected.push_back(readRunRow(statement));
                        }
                    }
                    std::vector<Domain::AgentRunRecord> closed;
                    closed.reserve(selected.size());
                    for (auto& run : selected) {
                        auto enriched = selectEnrichedRunById(
                            transaction, run.session.id, context);
                        if (enriched) {
                            run.outputSchema = std::move(enriched->outputSchema);
                            run.firstMoves = std::move(enriched->firstMoves);
                        }
                        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                            request.now - run.session.updatedAt);
                        const auto summary = Domain::makeAgentStaleSummary(
                            (std::max)(age, std::chrono::seconds::zero()));
                        auto update = take(transaction.prepare(
                            "UPDATE agent_sessions SET "
                            "status='closed',summary=?,updated_at=? "
                            "WHERE id=? AND julianday(updated_at)<julianday(?) AND "
                            "status IN ('open','active','running','started')"));
                        take(update.bindText(1, summary));
                        take(update.bindText(2, timestampText(request.now)));
                        take(update.bindText(3, run.session.id.value()));
                        take(update.bindText(4, cutoff));
                        stepDone(update);
                        if (changes(transaction, context) != 1) {
                            fail(integrityError(
                                "A stale agent session changed during serialized closure."));
                        }
                        run.session.status = Domain::SessionStatus::Closed;
                        run.session.summary = summary;
                        run.session.updatedAt = request.now;
                        if (run.session.clientId) {
                            static_cast<void>(deleteMatchingActiveProjection(
                                transaction,
                                *run.session.clientId,
                                run.session.id,
                                context));
                        }
                        writeRunProjection(
                            transaction, run, request.now, context);
                        closed.push_back(std::move(run));
                    }
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::CloseStale,
                        AgentSessionTransactionCheckpoint::BeforeCommit);
                    take(transaction.commit());
                    notifyObserver(
                        observer,
                        AgentSessionTransactionKind::CloseStale,
                        AgentSessionTransactionCheckpoint::AfterCommit);
                    return Domain::AgentStaleCloseOutcome{std::move(closed)};
                });
            }));
    });
}

Domain::Result<void> WindowsAgentSessionRepository::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->repositoryStore()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The agent-session repository is closed."));
    }
    return implementation_->database->quickCheck(context);
}

void WindowsAgentSessionRepository::close() noexcept
{
    try {
        if (!implementation_ || !implementation_->database ||
            !implementation_->clock ||
            implementation_->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (!implementation_->closeDatabaseOnClose) {
            return;
        }
        auto operationId = Domain::OperationId::parse(
            "ffffffff-ffff-4fff-bfff-fffffffffff2");
        auto correlationId = Domain::CorrelationId::parse(
            "agent-session-repository-close");
        if (!operationId || !correlationId) {
            return;
        }
        const Domain::OperationContext context{
            std::move(operationId).value(),
            implementation_->clock->monotonicNow() + std::chrono::seconds{30},
            {},
            std::move(correlationId).value()};
        static_cast<void>(implementation_->database->close(context));
    } catch (...) {
        // Idempotent best-effort noexcept boundary; central RAII is the final
        // fallback if bounded checkpointing cannot finish.
    }
}

} // namespace ForgeConductor::Persistence::Windows
