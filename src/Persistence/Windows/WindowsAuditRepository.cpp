#include "ForgeConductor/Persistence/Windows/WindowsAuditRepository.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "Detail/WinsqliteTransaction.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
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

constexpr std::size_t MaximumTimestampBytes = 64U;
constexpr std::size_t MaximumToolBytes = 128U;
constexpr std::size_t MaximumStatusBytes = 128U;
constexpr std::size_t MaximumErrorBytes = 4U * 1024U;
constexpr std::string_view ProjectionColumns =
    "COALESCE(occurred_at,timestamp),client_id,tool,args_digest,"
    "COALESCE(status,'ok'),duration_ms,COALESCE(error_code,error)";

static_assert(
    WindowsAuditRepository::MaximumRetainedEvents > 0U &&
    WindowsAuditRepository::MaximumRetainedEvents <=
        static_cast<std::size_t>(INT64_MAX));

struct RepositoryFailure final {
    Domain::Error error;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw RepositoryFailure{std::move(error)};
}

[[noreturn]] void invalid(std::string message)
{
    fail(Domain::makeError(Domain::ErrorCodes::InvalidRequest, std::move(message)));
}

[[noreturn]] void payloadTooLarge(std::string message)
{
    fail(Domain::makeError(Domain::ErrorCodes::PayloadTooLarge, std::move(message)));
}

[[noreturn]] void integrity(std::string message)
{
    fail(Domain::makeError(Domain::ErrorCodes::IntegrityFailure, std::move(message)));
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

template <typename Callable>
[[nodiscard]] Domain::Result<void> guardedVoid(Callable&& callable) noexcept
{
    try {
        std::forward<Callable>(callable)();
        return Domain::Result<void>::success();
    } catch (RepositoryFailure& failure) {
        return Domain::Result<void>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The audit repository operation failed safely."));
    }
}

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> guarded(Callable&& callable) noexcept
{
    try {
        return Domain::Result<T>::success(std::forward<Callable>(callable)());
    } catch (RepositoryFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The audit repository operation failed safely."));
    }
}

template <typename Callable>
class DatabaseOperation final : public Detail::IWindowsDatabaseOperation {
public:
    explicit DatabaseOperation(Callable callable)
        : callable_{std::move(callable)}
    {
    }

private:
    [[nodiscard]] Domain::Result<void> execute(
        WinsqliteConnection& connection) noexcept override
    {
        try {
            return callable_(connection);
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The audit database callback failed safely."));
        }
    }

    Callable callable_;
};

template <typename Callable>
[[nodiscard]] Domain::Result<void> runOnStore(
    Detail::WindowsDatabaseStore& store,
    const std::string_view action,
    const Domain::OperationContext& context,
    Callable&& callable) noexcept
{
    using Operation = DatabaseOperation<std::decay_t<Callable>>;
    Operation operation{std::forward<Callable>(callable)};
    return store.runExclusive(operation, action, context);
}

[[nodiscard]] Detail::WindowsDatabaseStore& requireStore(
    Detail::WindowsDatabaseStore* const store)
{
    if (store == nullptr) {
        invalid("The audit repository is closed.");
    }
    return *store;
}

void stepDone(WinsqliteStatement& statement)
{
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        integrity("An audit write unexpectedly returned a row.");
    }
}

void requireInputText(
    const std::string_view value,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    if (value.empty()) {
        invalid(std::string{field} + " must be nonempty.");
    }
    if (value.size() > maximumBytes) {
        payloadTooLarge(std::string{field} + " exceeds its UTF-8 byte limit.");
    }
    if (value.find('\0') != std::string_view::npos || !Domain::isValidUtf8(value)) {
        invalid(std::string{field} + " must be valid UTF-8 without null bytes.");
    }
}

[[nodiscard]] std::string requiredPersistedText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto value = take(statement.columnText(column, maximumBytes));
    if (!value) {
        integrity("A required audit column is null: " + std::string{field} + '.');
    }
    if (value->empty() || value->find('\0') != std::string::npos ||
        !Domain::isValidUtf8(*value)) {
        integrity("A persisted audit column is invalid UTF-8: " +
                  std::string{field} + '.');
    }
    return std::move(*value);
}

[[nodiscard]] std::optional<std::string> optionalPersistedText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto value = take(statement.columnText(column, maximumBytes));
    if (value &&
        (value->empty() || value->find('\0') != std::string::npos ||
         !Domain::isValidUtf8(*value))) {
        integrity("A persisted optional audit column is invalid UTF-8: " +
                  std::string{field} + '.');
    }
    return value;
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto elapsed = timestamp.time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    if (milliseconds.count() < 0) {
        invalid("Audit timestamps must not precede the Unix epoch.");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fractional = milliseconds -
        std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0) {
        invalid("The audit timestamp is outside the supported UTC range.");
    }
    std::array<char, 25U> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        static_cast<long long>(fractional.count()));
    if (written != 24) {
        invalid("The audit timestamp could not be formatted canonically.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
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
        integrity("A persisted audit timestamp is not canonical UTC.");
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
        *day < 1 || *day > 31 || *hour > 23 || *minute > 59 ||
        *second > 59) {
        integrity("A persisted audit timestamp is invalid.");
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
        integrity("A persisted audit timestamp is outside range.");
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
        std::chrono::milliseconds{*millisecond};
}

void validateEvent(const Domain::AuditEvent& event)
{
    static_cast<void>(timestampText(event.timestamp));
    requireInputText(event.tool, MaximumToolBytes, "Audit tool");
    requireInputText(event.status, MaximumStatusBytes, "Audit status");
    if (event.error) {
        requireInputText(*event.error, MaximumErrorBytes, "Audit error code");
    }
    if (event.duration && event.duration->count() < 0) {
        invalid("Audit duration must not be negative.");
    }
}

void bindOptionalText(
    WinsqliteStatement& statement,
    const int parameter,
    const std::optional<std::string>& value)
{
    if (value) {
        take(statement.bindText(parameter, *value));
    } else {
        take(statement.bindNull(parameter));
    }
}

[[nodiscard]] Domain::AuditEvent readEvent(const WinsqliteStatement& statement)
{
    const auto timestamp = parseTimestamp(requiredPersistedText(
        statement, 0, MaximumTimestampBytes, "occurred_at"));
    std::optional<Domain::ClientId> clientId;
    if (auto value = optionalPersistedText(statement, 1, 128U, "client_id")) {
        auto parsed = Domain::ClientId::parse(*value);
        if (!parsed) {
            integrity("A persisted audit client identifier is invalid.");
        }
        clientId.emplace(std::move(parsed).value());
    }
    auto tool = requiredPersistedText(
        statement, 2, MaximumToolBytes, "tool");
    std::optional<Domain::Sha256Digest> argumentsDigest;
    if (auto value = optionalPersistedText(
            statement, 3, 64U, "args_digest")) {
        auto parsed = Domain::Sha256Digest::parse(*value);
        if (!parsed) {
            integrity("A persisted audit argument digest is invalid.");
        }
        argumentsDigest.emplace(std::move(parsed).value());
    }
    auto status = requiredPersistedText(
        statement, 4, MaximumStatusBytes, "status");
    std::optional<std::chrono::milliseconds> duration;
    if (!take(statement.columnIsNull(5))) {
        const auto milliseconds = take(statement.columnInt64(5));
        if (milliseconds < 0) {
            integrity("A persisted audit duration is negative.");
        }
        duration.emplace(milliseconds);
    }
    auto error = optionalPersistedText(
        statement, 6, MaximumErrorBytes, "error_code");
    return Domain::AuditEvent{
        timestamp,
        std::move(clientId),
        std::move(tool),
        std::move(argumentsDigest),
        std::move(status),
        duration,
        std::move(error)};
}

} // namespace

struct WindowsAuditRepository::Impl final {
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

WindowsAuditRepository::WindowsAuditRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsAuditRepository::~WindowsAuditRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsAuditRepository>>
WindowsAuditRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database) noexcept
{
    try {
        if (!database) {
            return Domain::Result<std::shared_ptr<WindowsAuditRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The audit repository requires a shared central database."));
        }
        auto implementation = std::make_unique<Impl>(std::move(database));
        return Domain::Result<std::shared_ptr<WindowsAuditRepository>>::success(
            std::shared_ptr<WindowsAuditRepository>{
                new WindowsAuditRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsAuditRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The audit repository could not be constructed."));
    }
}

Domain::Result<void> WindowsAuditRepository::append(
    const Domain::AuditEvent& event,
    const Domain::OperationContext& context) noexcept
{
    return guardedVoid([&]() {
        validateEvent(event);
        const auto timestamp = timestampText(event.timestamp);
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        take(runOnStore(
            store,
            "Append audit event",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guardedVoid([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto statement = take(transaction.prepare(
                        "INSERT INTO audit_events("
                        "timestamp,client_id,tool,args_digest,status,duration_ms,"
                        "error,occurred_at,error_code) "
                        "VALUES(?,?,?,?,?,?,?,?,?)"));
                    take(statement.bindText(1, timestamp));
                    if (event.clientId) {
                        take(statement.bindText(2, event.clientId->value()));
                    } else {
                        take(statement.bindNull(2));
                    }
                    take(statement.bindText(3, event.tool));
                    if (event.argumentsDigest) {
                        take(statement.bindText(
                            4, event.argumentsDigest->value()));
                    } else {
                        take(statement.bindNull(4));
                    }
                    take(statement.bindText(5, event.status));
                    if (event.duration) {
                        take(statement.bindInt64(6, event.duration->count()));
                    } else {
                        take(statement.bindNull(6));
                    }
                    bindOptionalText(statement, 7, event.error);
                    take(statement.bindText(8, timestamp));
                    bindOptionalText(statement, 9, event.error);
                    stepDone(statement);

                    auto prune = take(transaction.prepare(
                        "DELETE FROM audit_events WHERE id < ("
                        "SELECT id FROM audit_events ORDER BY id DESC "
                        "LIMIT 1 OFFSET ?)"));
                    take(prune.bindInt64(
                        1,
                        static_cast<std::int64_t>(
                            MaximumRetainedEvents - 1U)));
                    stepDone(prune);
                    take(transaction.commit());
                });
            }));
    });
}

Domain::Result<std::vector<Domain::AuditEvent>> WindowsAuditRepository::recent(
    const std::size_t maximumCount,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::vector<Domain::AuditEvent>>([&]() {
        if (maximumCount > MaximumRecentEvents) {
            fail(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The requested audit history exceeds the 200-event read bound."));
        }
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        std::vector<Domain::AuditEvent> events;
        events.reserve(maximumCount);
        take(runOnStore(
            store,
            "Read recent audit events",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guardedVoid([&]() {
                    auto statement = take(connection.prepare(
                        std::string{"SELECT "} +
                        std::string{ProjectionColumns} +
                        " FROM audit_events ORDER BY id DESC LIMIT ?",
                        context));
                    take(statement.bindInt64(
                        1, static_cast<std::int64_t>(maximumCount)));
                    for (;;) {
                        const auto step = take(statement.step());
                        if (step == WinsqliteStepResult::Done) {
                            break;
                        }
                        events.push_back(readEvent(statement));
                    }
                });
            }));
        return events;
    });
}

void WindowsAuditRepository::close() noexcept
{
    try {
        if (implementation_) {
            implementation_->closed.store(true, std::memory_order_release);
        }
    } catch (...) {
    }
}

} // namespace ForgeConductor::Persistence::Windows
