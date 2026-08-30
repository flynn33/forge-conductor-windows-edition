#include "ForgeConductor/Persistence/Windows/WindowsDashboardOperationalRepository.h"

#include "ForgeConductor/Domain/ClientPresenceModels.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace ForgeConductor::Persistence::Windows {
namespace {

using Detail::WinsqliteConnection;
using Detail::WinsqliteStatement;
using Detail::WinsqliteStepResult;

constexpr std::size_t MaximumTimestampBytes = 64U;
constexpr std::size_t MaximumSummaryBytes =
    Domain::AgentSessionLimits::MaximumSummaryUnits * 4U;
constexpr std::size_t MaximumPersistedIdentifierBytes = 256U;
constexpr std::size_t MaximumBoundedRows =
    Domain::AgentSessionLimits::MaximumSessionQueryRows;

constexpr std::string_view SnapshotSql =
    "WITH open_rows AS ("
    "SELECT 0 AS record_kind,created_at AS sort_time,id AS sort_id,"
    "id,agent_id,client_id,status,summary,created_at,updated_at,"
    "NULL AS presence_client_id,NULL AS role,NULL AS process_id,"
    "NULL AS working_directory,NULL AS last_seen_at "
    "FROM agent_sessions "
    "WHERE status IN ('open','active','running','started') "
    "ORDER BY created_at DESC,id DESC LIMIT ?),"
    "recent_rows AS ("
    "SELECT 1 AS record_kind,created_at AS sort_time,id AS sort_id,"
    "id,agent_id,client_id,status,summary,created_at,updated_at,"
    "NULL AS presence_client_id,NULL AS role,NULL AS process_id,"
    "NULL AS working_directory,NULL AS last_seen_at "
    "FROM agent_sessions "
    "ORDER BY created_at DESC,id DESC LIMIT ?),"
    "presence_rows AS ("
    "SELECT 2 AS record_kind,last_seen_at AS sort_time,"
    "client_id AS sort_id,NULL AS id,NULL AS agent_id,NULL AS client_id,"
    "NULL AS status,NULL AS summary,NULL AS created_at,NULL AS updated_at,"
    "client_id AS presence_client_id,role,process_id,working_directory,"
    "last_seen_at FROM client_presence "
    "ORDER BY last_seen_at DESC,client_id DESC LIMIT ?) "
    "SELECT * FROM open_rows UNION ALL SELECT * FROM recent_rows "
    "UNION ALL SELECT * FROM presence_rows "
    "ORDER BY record_kind ASC,sort_time DESC,sort_id DESC";

struct RepositoryFailure final {
    Domain::Error error;
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

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> guarded(Callable&& callable) noexcept
{
    try {
        return Domain::Result<T>::success(
            std::forward<Callable>(callable)());
    } catch (RepositoryFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard operational repository operation failed safely."));
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
                "The dashboard operational database operation produced no result."));
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
                "The dashboard operational database callback failed safely.");
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

[[noreturn]] void failColumnRead(
    Domain::Error error,
    std::string message)
{
    if (error.code == Domain::ErrorCodes::IntegrityFailure ||
        error.code == Domain::ErrorCodes::PayloadTooLarge) {
        fail(integrityError(std::move(message)));
    }
    fail(std::move(error));
}

[[nodiscard]] Detail::WindowsDatabaseStore& requireStore(
    Detail::WindowsDatabaseStore* const store)
{
    if (!store) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard operational repository is closed."));
    }
    return *store;
}

void requirePersistedUtf8(
    const std::string_view value,
    const std::string_view field)
{
    if (value.find('\0') != std::string_view::npos ||
        !Infrastructure::Windows::Detail::strictUtf8ToUtf16(value)) {
        fail(integrityError(
            "Persisted dashboard operational data contains invalid UTF-8: " +
            std::string{field} + '.'));
    }
}

[[nodiscard]] std::string requiredText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto text = statement.columnText(column, maximumBytes);
    if (!text) {
        failColumnRead(
            std::move(text).error(),
            "A required persisted dashboard column is invalid: " +
                std::string{field} + '.');
    }
    if (!text.value()) {
        fail(integrityError(
            "A required persisted dashboard column is null: " +
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
        failColumnRead(
            std::move(text).error(),
            "An optional persisted dashboard column is invalid: " +
                std::string{field} + '.');
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
            "A persisted dashboard identifier is invalid: " +
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
            "A persisted dashboard timestamp is not canonical UTC."));
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
        fail(integrityError("A persisted dashboard timestamp is invalid."));
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
            "A persisted dashboard timestamp is outside the supported range."));
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
        std::chrono::milliseconds{*millisecond};
}

[[nodiscard]] Domain::AgentSession readSession(
    const WinsqliteStatement& statement)
{
    auto sessionId = persistedIdentifier<Domain::SessionId>(
        requiredText(
            statement, 3, MaximumPersistedIdentifierBytes, "session id"),
        "session id");
    auto agentId = persistedIdentifier<Domain::AgentId>(
        requiredText(
            statement, 4, MaximumPersistedIdentifierBytes, "agent id"),
        "agent id");
    auto clientText = optionalText(
        statement, 5, MaximumPersistedIdentifierBytes, "client id");
    std::optional<Domain::ClientId> clientId;
    if (clientText) {
        clientId.emplace(persistedIdentifier<Domain::ClientId>(
            *clientText, "client id"));
    }
    const auto statusText = requiredText(statement, 6, 16U, "session status");
    auto statusResult = Domain::sessionStatusFromWire(statusText);
    if (!statusResult) {
        fail(integrityError("A persisted dashboard session status is invalid."));
    }
    auto summary = optionalText(
        statement, 7, MaximumSummaryBytes, "session summary");
    const auto createdAt = parseTimestamp(requiredText(
        statement, 8, MaximumTimestampBytes, "created timestamp"));
    const auto updatedAt = parseTimestamp(requiredText(
        statement, 9, MaximumTimestampBytes, "updated timestamp"));
    if (updatedAt < createdAt) {
        fail(integrityError(
            "A persisted dashboard session has decreasing timestamps."));
    }
    return Domain::AgentSession{
        std::move(sessionId),
        std::move(agentId),
        std::move(clientId),
        std::move(statusResult).value(),
        std::move(summary),
        createdAt,
        updatedAt};
}

[[nodiscard]] WindowsDashboardPresenceProjection readPresence(
    const WinsqliteStatement& statement)
{
    auto clientId = requiredText(
        statement, 10, 256U, "presence client id");
    auto role = requiredText(
        statement,
        11,
        Domain::ClientPresenceLimits::MaximumRoleBytes,
        "presence role");
    if (clientId.empty() || role.empty()) {
        fail(integrityError(
            "A persisted dashboard presence identity is empty."));
    }
    auto processResult = statement.columnInt64(12);
    if (!processResult) {
        failColumnRead(
            std::move(processResult).error(),
            "A persisted dashboard presence process id is not an integer.");
    }
    const auto processId = std::move(processResult).value();
    if (processId <= 0 ||
        processId > static_cast<std::int64_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        fail(integrityError(
            "A persisted dashboard presence process id is out of range."));
    }
    auto pathText = requiredText(
        statement,
        13,
        Domain::PathText::MaximumBytes,
        "presence working directory");
    auto path = Domain::PathText::create(pathText);
    if (!path) {
        fail(integrityError(
            "A persisted dashboard presence working directory is invalid."));
    }
    const auto heartbeat = parseTimestamp(requiredText(
        statement, 14, MaximumTimestampBytes, "presence heartbeat"));
    return WindowsDashboardPresenceProjection{
        std::move(clientId),
        std::move(role),
        static_cast<std::uint32_t>(processId),
        std::move(path).value(),
        heartbeat};
}

void requireValidLimit(
    const std::size_t value,
    const std::string_view field)
{
    if (value == 0U || value > MaximumBoundedRows) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{field} + " must be between 1 and 10000 rows."));
    }
}

void consumeTextBudget(
    std::size_t& remainingBytes,
    const std::string_view value,
    const std::string_view projection)
{
    if (value.size() > remainingBytes) {
        fail(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            std::string{projection} +
                " dashboard text exceeds its aggregate byte bound."));
    }
    remainingBytes -= value.size();
}

void consumeSessionBudget(
    std::size_t& remainingBytes,
    const Domain::AgentSession& session)
{
    consumeTextBudget(remainingBytes, session.id.value(), "Session");
    consumeTextBudget(remainingBytes, session.agentId.value(), "Session");
    if (session.clientId) {
        consumeTextBudget(
            remainingBytes, session.clientId->value(), "Session");
    }
    if (session.summary) {
        consumeTextBudget(remainingBytes, *session.summary, "Session");
    }
}

void consumePresenceBudget(
    std::size_t& remainingBytes,
    const WindowsDashboardPresenceProjection& presence)
{
    consumeTextBudget(remainingBytes, presence.clientId, "Presence");
    consumeTextBudget(remainingBytes, presence.hostKind, "Presence");
    consumeTextBudget(
        remainingBytes, presence.workingDirectory.value(), "Presence");
}

} // namespace

struct WindowsDashboardOperationalRepository::Impl final {
    explicit Impl(std::shared_ptr<WindowsCentralDatabase> ownedDatabase) noexcept
        : database{std::move(ownedDatabase)}
    {
    }

    [[nodiscard]] Detail::WindowsDatabaseStore* repositoryStore() noexcept
    {
        return !closed.load(std::memory_order_acquire) && database
            ? database->repositoryStore()
            : nullptr;
    }

    std::shared_ptr<WindowsCentralDatabase> database;
    std::atomic_bool closed{};
};

WindowsDashboardOperationalRepository::WindowsDashboardOperationalRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsDashboardOperationalRepository::~WindowsDashboardOperationalRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsDashboardOperationalRepository>>
WindowsDashboardOperationalRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database) noexcept
{
    try {
        if (!database) {
            return Domain::Result<
                std::shared_ptr<WindowsDashboardOperationalRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard operational repository requires a shared database."));
        }
        auto implementation = std::make_unique<Impl>(std::move(database));
        return Domain::Result<
            std::shared_ptr<WindowsDashboardOperationalRepository>>::success(
            std::shared_ptr<WindowsDashboardOperationalRepository>{
                new WindowsDashboardOperationalRepository{
                    std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<WindowsDashboardOperationalRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard operational repository could not be constructed."));
    }
}

Domain::Result<WindowsDashboardOperationalProjection>
WindowsDashboardOperationalRepository::snapshot(
    const std::size_t maximumOpenSessions,
    const std::size_t maximumRecentSessions,
    const std::size_t maximumPresenceRecords,
    const Domain::OperationContext& context) noexcept
{
    return guarded<WindowsDashboardOperationalProjection>([&]() {
        requireValidLimit(maximumOpenSessions, "Maximum open sessions");
        requireValidLimit(maximumRecentSessions, "Maximum recent sessions");
        requireValidLimit(maximumPresenceRecords, "Maximum presence records");
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<WindowsDashboardOperationalProjection>(
            store,
            "Read dashboard operational snapshot",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<WindowsDashboardOperationalProjection>([&]() {
                    auto statement = take(connection.prepare(SnapshotSql, context));
                    take(statement.bindInt64(
                        1,
                        static_cast<std::int64_t>(maximumOpenSessions + 1U)));
                    take(statement.bindInt64(
                        2,
                        static_cast<std::int64_t>(maximumRecentSessions)));
                    take(statement.bindInt64(
                        3,
                        static_cast<std::int64_t>(maximumPresenceRecords + 1U)));

                    WindowsDashboardOperationalProjection projection;
                    projection.openSessions.reserve(maximumOpenSessions);
                    projection.recentSessions.reserve(maximumRecentSessions);
                    projection.presence.reserve(maximumPresenceRecords);
                    std::unordered_set<std::string> openIds;
                    std::unordered_set<std::string> recentIds;
                    std::unordered_set<std::string> presenceIds;
                    std::size_t remainingSessionTextBytes =
                        WindowsDashboardOperationalLimits::
                            MaximumSessionTextBytes;
                    std::size_t remainingPresenceTextBytes =
                        WindowsDashboardOperationalLimits::
                            MaximumPresenceTextBytes;

                    for (;;) {
                        if (take(statement.step()) == WinsqliteStepResult::Done) {
                            break;
                        }
                        const auto kind = take(statement.columnInt64(0));
                        if (kind == 0) {
                            auto session = readSession(statement);
                            consumeSessionBudget(
                                remainingSessionTextBytes, session);
                            if (!openIds.emplace(session.id.value()).second) {
                                fail(integrityError(
                                    "The dashboard open-session projection contains a duplicate."));
                            }
                            projection.openSessions.push_back(std::move(session));
                        } else if (kind == 1) {
                            auto session = readSession(statement);
                            consumeSessionBudget(
                                remainingSessionTextBytes, session);
                            if (!recentIds.emplace(session.id.value()).second) {
                                fail(integrityError(
                                    "The dashboard recent-session projection contains a duplicate."));
                            }
                            projection.recentSessions.push_back(std::move(session));
                        } else if (kind == 2) {
                            auto presence = readPresence(statement);
                            consumePresenceBudget(
                                remainingPresenceTextBytes, presence);
                            if (!presenceIds.emplace(presence.clientId).second) {
                                fail(integrityError(
                                    "The dashboard presence projection contains a duplicate."));
                            }
                            projection.presence.push_back(std::move(presence));
                        } else {
                            fail(integrityError(
                                "The dashboard operational projection tag is invalid."));
                        }
                    }

                    if (projection.openSessions.size() > maximumOpenSessions) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The open-session dashboard projection exceeds its bound."));
                    }
                    if (projection.presence.size() > maximumPresenceRecords) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The presence dashboard projection exceeds its bound."));
                    }
                    return projection;
                });
            }));
    });
}

void WindowsDashboardOperationalRepository::close() noexcept
{
    try {
        if (implementation_) {
            implementation_->closed.store(true, std::memory_order_release);
        }
    } catch (...) {
    }
}

} // namespace ForgeConductor::Persistence::Windows
