#include "ForgeConductor/Persistence/Windows/WindowsLegacyMemoryRepository.h"

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
#include <map>
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
constexpr std::size_t MaximumPersistedTagsJsonBytes = 1024U * 1024U;
constexpr std::string_view PurgeAction = "purge_legacy_memory";
constexpr std::string_view PurgeScope = "legacy-global-memory";
constexpr std::string_view PurgeToken = "PURGE LEGACY GLOBAL MEMORY";

constexpr std::string_view FullProjectionColumns =
    "key,body,length(CAST(body AS BLOB)),tags_json,created_at,updated_at";
constexpr std::string_view SummaryProjectionColumns =
    "key,NULL,length(CAST(body AS BLOB)),tags_json,created_at,updated_at";
constexpr std::string_view VisibleMemoryPredicate =
    " AND key NOT LIKE 'agent\\_run/%' ESCAPE '\\'"
    " AND key NOT LIKE 'agent\\_active/%' ESCAPE '\\'"
    " AND key NOT LIKE 'continuity/%' ESCAPE '\\'";

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
        return Domain::Result<T>::success(std::forward<Callable>(callable)());
    } catch (RepositoryFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The legacy memory repository operation failed safely."));
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
        if (!outcome_.has_value()) {
            return Domain::Result<T>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The legacy memory database operation produced no result."));
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
                "The legacy memory database callback failed safely.");
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
    return Domain::makeError(Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] std::string requiredText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto value = take(statement.columnText(column, maximumBytes));
    if (!value.has_value()) {
        fail(integrityError(
            "A required legacy memory column is null: " + std::string{field} + '.'));
    }
    return std::move(*value);
}

void requirePersistedUtf8(
    const std::string_view value,
    const std::string_view field)
{
    if (value.find('\0') != std::string_view::npos ||
        !Infrastructure::Windows::Detail::strictUtf8ToUtf16(value)) {
        fail(integrityError(
            "Persisted legacy memory contains invalid UTF-8: " +
            std::string{field} + '.'));
    }
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        timestamp.time_since_epoch());
    const __time64_t time = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &time) != 0) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The legacy memory timestamp is outside the supported UTC range."));
    }
    std::array<char, 21U> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec);
    if (written != 20) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The legacy memory timestamp could not be formatted."));
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
        fail(integrityError(
            "A persisted legacy memory timestamp is not canonical UTC."));
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
        fail(integrityError("A persisted legacy memory timestamp is invalid."));
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
            "A persisted legacy memory timestamp is outside range."));
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
           std::chrono::milliseconds{*millisecond};
}

[[nodiscard]] std::vector<std::string> decodeTags(
    const std::string_view encoded)
{
    const Json parsed = Json::parse(encoded.begin(), encoded.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return {};
    }
    if (parsed.size() > Domain::LegacyMemoryLimits::MaximumTagCount) {
        fail(integrityError(
            "Persisted legacy memory contains too many tags."));
    }
    std::vector<std::string> tags;
    tags.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) {
            return {};
        }
        auto tag = item.get<std::string>();
        if (tag.size() > Domain::LegacyMemoryLimits::MaximumTagBytes) {
            fail(integrityError(
                "A persisted legacy memory tag exceeds its byte limit."));
        }
        requirePersistedUtf8(tag, "tag");
        tags.push_back(std::move(tag));
    }
    return tags;
}

[[nodiscard]] std::string encodeTags(const std::vector<std::string>& tags)
{
    return Json(tags).dump();
}

[[nodiscard]] std::vector<std::string> canonicalizeWriteTags(
    const std::vector<std::string>& tags,
    const Contracts::IUnicodeCanonicalizer& canonicalizer)
{
    auto prepared = take(Domain::prepareLegacyMemoryTags(tags));
    std::map<Contracts::NfcUtf8Key, std::string> unique;
    for (auto& tag : prepared) {
        auto key = take(canonicalizer.nfcKey(tag));
        unique.try_emplace(std::move(key), std::move(tag));
    }
    std::vector<std::string> result;
    result.reserve(unique.size());
    for (auto& [key, original] : unique) {
        static_cast<void>(key);
        result.push_back(std::move(original));
    }
    return result;
}

[[nodiscard]] bool containsCanonicalTag(
    const std::vector<std::string>& tags,
    const Contracts::NfcUtf8Key& candidate,
    const Contracts::IUnicodeCanonicalizer& canonicalizer)
{
    return std::any_of(tags.begin(), tags.end(), [&](const std::string& tag) {
        return take(canonicalizer.nfcKey(tag)) == candidate;
    });
}

[[nodiscard]] Domain::MemoryNote readNote(const WinsqliteStatement& statement)
{
    auto key = requiredText(
        statement, 0, Domain::LegacyMemoryLimits::MaximumKeyBytes, "key");
    auto body = requiredText(
        statement, 1, Domain::LegacyMemoryLimits::MaximumBodyBytes, "body");
    const auto tagsJson = requiredText(
        statement, 2, MaximumPersistedTagsJsonBytes, "tags_json");
    const auto createdAt = requiredText(
        statement, 3, MaximumTimestampBytes, "created_at");
    const auto updatedAt = requiredText(
        statement, 4, MaximumTimestampBytes, "updated_at");
    requirePersistedUtf8(key, "key");
    requirePersistedUtf8(body, "body");
    auto tags = decodeTags(tagsJson);
    Domain::MemoryNote note{
        std::move(key),
        std::move(body),
        std::move(tags),
        parseTimestamp(createdAt),
        parseTimestamp(updatedAt)};
    auto valid = Domain::validateMemoryNote(note);
    if (!valid) {
        fail(integrityError(
            "A persisted legacy memory note violates its semantic bounds."));
    }
    return note;
}

[[nodiscard]] Domain::LegacyMemoryNoteProjection readProjection(
    const WinsqliteStatement& statement,
    const bool includeBody)
{
    auto key = requiredText(
        statement, 0, Domain::LegacyMemoryLimits::MaximumKeyBytes, "key");
    auto body = take(statement.columnText(
        1, Domain::LegacyMemoryLimits::MaximumBodyBytes));
    const auto bodyBytesValue = take(statement.columnInt64(2));
    const auto tagsJson = requiredText(
        statement, 3, MaximumPersistedTagsJsonBytes, "tags_json");
    const auto createdAt = requiredText(
        statement, 4, MaximumTimestampBytes, "created_at");
    const auto updatedAt = requiredText(
        statement, 5, MaximumTimestampBytes, "updated_at");
    if (bodyBytesValue < 0 ||
        bodyBytesValue > static_cast<std::int64_t>(
                             Domain::LegacyMemoryLimits::MaximumBodyBytes)) {
        fail(integrityError(
            "A persisted legacy memory body exceeds its byte limit."));
    }
    if (includeBody && !body.has_value()) {
        fail(integrityError(
            "A required projected legacy memory body is null."));
    }
    if (!includeBody && body.has_value()) {
        fail(integrityError(
            "A summary-only legacy memory query materialized a body."));
    }
    requirePersistedUtf8(key, "key");
    if (body) {
        requirePersistedUtf8(*body, "body");
        if (body->size() != static_cast<std::size_t>(bodyBytesValue)) {
            fail(integrityError(
                "A projected legacy memory body byte count is inconsistent."));
        }
    }
    return Domain::LegacyMemoryNoteProjection{
        std::move(key),
        std::move(body),
        static_cast<std::size_t>(bodyBytesValue),
        decodeTags(tagsJson),
        parseTimestamp(createdAt),
        parseTimestamp(updatedAt)};
}

void stepDone(WinsqliteStatement& statement)
{
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        fail(integrityError(
            "A legacy memory write unexpectedly returned a row."));
    }
}

[[nodiscard]] std::int64_t scalarInteger(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        fail(integrityError(
            "A legacy memory scalar query returned no row."));
    }
    return take(statement.columnInt64(0));
}

[[nodiscard]] std::int64_t scalarInteger(
    WinsqliteTransaction& transaction,
    const std::string_view sql)
{
    auto statement = take(transaction.prepare(sql));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        fail(integrityError(
            "A transactional legacy memory scalar query returned no row."));
    }
    return take(statement.columnInt64(0));
}

[[nodiscard]] std::string escapeLikeLiteral(const std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '%' || character == '_') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

void requireRepositoryKey(const std::string_view key)
{
    Domain::MemoryNote note{
        std::string{key}, {}, {}, Domain::UtcTimePoint{}, Domain::UtcTimePoint{}};
    auto valid = Domain::validateMemoryNote(note);
    if (!valid) {
        fail(std::move(valid).error());
    }
}

void requireRepositoryListQuery(const Domain::LegacyMemoryListQuery& request)
{
    if (request.limit == 0U ||
        request.limit > Domain::LegacyMemoryLimits::MaximumQueryLimit) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The normalized legacy memory list limit is outside 1 through 200."));
    }
    for (const auto& value : {request.prefix, request.tag}) {
        if (value &&
            (value->size() > Domain::LegacyMemoryLimits::MaximumFilterBytes ||
             value->find('\0') != std::string::npos ||
             !Infrastructure::Windows::Detail::strictUtf8ToUtf16(*value))) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A normalized legacy memory list filter is invalid."));
        }
    }
}

void requireRepositorySearchQuery(const Domain::LegacyMemorySearchQuery& request)
{
    if (request.query.empty() ||
        request.query.size() > Domain::LegacyMemoryLimits::MaximumQueryBytes ||
        request.query.find('\0') != std::string::npos ||
        !Infrastructure::Windows::Detail::strictUtf8ToUtf16(request.query) ||
        request.limit == 0U ||
        request.limit > Domain::LegacyMemoryLimits::MaximumQueryLimit) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The normalized legacy memory search query is invalid."));
    }
}

[[nodiscard]] Detail::WindowsDatabaseStore& requireStore(
    Detail::WindowsDatabaseStore* store)
{
    if (store == nullptr) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The legacy memory repository is closed."));
    }
    return *store;
}

} // namespace

struct WindowsLegacyMemoryRepository::Impl final {
    Impl(
        std::shared_ptr<WindowsCentralDatabase> ownedDatabase,
        const bool closeOwnedDatabase,
        std::shared_ptr<Contracts::IClock> ownedClock,
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            ownedUnicodeCanonicalizer) noexcept
        : database{std::move(ownedDatabase)},
          closeDatabaseOnClose{closeOwnedDatabase},
          clock{std::move(ownedClock)},
          unicodeCanonicalizer{std::move(ownedUnicodeCanonicalizer)}
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
    std::atomic_bool closed{};
    std::shared_ptr<Contracts::IClock> clock;
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer;
};

WindowsLegacyMemoryRepository::WindowsLegacyMemoryRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsLegacyMemoryRepository::~WindowsLegacyMemoryRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>
WindowsLegacyMemoryRepository::open(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer,
    const Domain::OperationContext& context) noexcept
{
    if (!applicationPaths || !runtimeDiagnostics || !clock ||
        !unicodeCanonicalizer) {
        return Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The legacy memory repository requires paths, diagnostics, a clock, and Unicode canonicalization."));
    }
    auto database = WindowsCentralDatabase::open(
        std::move(applicationPaths),
        std::move(runtimeDiagnostics),
        clock,
        context);
    if (!database) {
        return Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>::failure(
            std::move(database).error());
    }
    return create(
        std::move(database).value(),
        std::move(clock),
        std::move(unicodeCanonicalizer));
}

Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>
WindowsLegacyMemoryRepository::create(
    std::unique_ptr<WindowsCentralDatabase> database,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer) noexcept
{
    try {
        if (!database || !clock || !unicodeCanonicalizer) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyMemoryRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The legacy memory repository requires an owned database, clock, and Unicode canonicalization."));
        }
        auto implementation = std::make_unique<Impl>(
            std::shared_ptr<WindowsCentralDatabase>{std::move(database)},
            true,
            std::move(clock),
            std::move(unicodeCanonicalizer));
        return Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>::success(
            std::shared_ptr<WindowsLegacyMemoryRepository>{
                new WindowsLegacyMemoryRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The legacy memory repository could not be constructed."));
    }
}

Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>
WindowsLegacyMemoryRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer) noexcept
{
    try {
        if (!database || !clock || !unicodeCanonicalizer) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyMemoryRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The legacy memory repository requires a shared database, clock, and Unicode canonicalization."));
        }
        auto implementation = std::make_unique<Impl>(
            std::move(database),
            false,
            std::move(clock),
            std::move(unicodeCanonicalizer));
        return Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>::success(
            std::shared_ptr<WindowsLegacyMemoryRepository>{
                new WindowsLegacyMemoryRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsLegacyMemoryRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The shared legacy memory repository could not be constructed."));
    }
}

Domain::Result<Domain::LegacyMemorySetOutcome>
WindowsLegacyMemoryRepository::upsert(
    const Domain::LegacyMemoryUpsert& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyMemorySetOutcome>([&]() {
        if (!implementation_ || !implementation_->clock ||
            !implementation_->unicodeCanonicalizer) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The legacy memory repository is closed."));
        }
        Domain::MemoryNote candidate{
            request.key,
            request.body,
            request.tags,
            Domain::UtcTimePoint{},
            Domain::UtcTimePoint{}};
        take(Domain::validateMemoryNote(candidate));
        const auto canonicalTags = canonicalizeWriteTags(
            request.tags, *implementation_->unicodeCanonicalizer);
        if (canonicalTags != request.tags) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The legacy memory repository requires canonical write tags."));
        }
        const auto tagsJson = encodeTags(request.tags);
        const auto timestamp = timestampText(implementation_->clock->utcNow());
        auto& store = requireStore(
            implementation_->repositoryStore());
        return take(runOnStore<Domain::LegacyMemorySetOutcome>(
            store,
            "Upsert legacy memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyMemorySetOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto write = take(transaction.prepare(
                        "INSERT INTO memory_notes("
                        "key,body,tags_json,created_at,updated_at) "
                        "VALUES(?,?,?,?,?) "
                        "ON CONFLICT(key) DO UPDATE SET "
                        "body=excluded.body,tags_json=excluded.tags_json,"
                        "updated_at=excluded.updated_at"));
                    take(write.bindText(1, request.key));
                    take(write.bindText(2, request.body));
                    take(write.bindText(3, tagsJson));
                    take(write.bindText(4, timestamp));
                    take(write.bindText(5, timestamp));
                    stepDone(write);
                    auto read = take(transaction.prepare(
                        "SELECT key,body,tags_json,created_at,updated_at "
                        "FROM memory_notes WHERE key=?"));
                    take(read.bindText(1, request.key));
                    if (take(read.step()) != WinsqliteStepResult::Row) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::StoreError,
                            "The legacy memory write did not persist.",
                            true));
                    }
                    auto note = readNote(read);
                    take(transaction.commit());
                    return Domain::LegacyMemorySetOutcome{
                        std::move(note), true};
                });
            }));
    });
}

Domain::Result<Domain::LegacyMemoryGetOutcome>
WindowsLegacyMemoryRepository::get(
    const std::string_view key,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyMemoryGetOutcome>([&]() {
        requireRepositoryKey(key);
        auto& store = requireStore(
            implementation_
                ? implementation_->repositoryStore()
                : nullptr);
        return take(runOnStore<Domain::LegacyMemoryGetOutcome>(
            store,
            "Get legacy memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyMemoryGetOutcome>([&]() {
                    auto statement = take(connection.prepare(
                        "SELECT key,body,tags_json,created_at,updated_at "
                        "FROM memory_notes WHERE key=?",
                        context));
                    take(statement.bindText(1, key));
                    if (take(statement.step()) == WinsqliteStepResult::Done) {
                        return Domain::LegacyMemoryGetOutcome{
                            std::string{key}, std::nullopt};
                    }
                    return Domain::LegacyMemoryGetOutcome{
                        std::string{key}, readNote(statement)};
                });
            }));
    });
}

Domain::Result<Domain::LegacyMemoryListOutcome>
WindowsLegacyMemoryRepository::list(
    const Domain::LegacyMemoryListQuery& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyMemoryListOutcome>([&]() {
        requireRepositoryListQuery(request);
        if (!implementation_ || !implementation_->unicodeCanonicalizer) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The legacy memory repository is closed."));
        }
        std::optional<Contracts::NfcUtf8Key> tagKey;
        if (request.tag) {
            tagKey.emplace(take(
                implementation_->unicodeCanonicalizer->nfcKey(*request.tag)));
        }
        auto& store = requireStore(
            implementation_
                ? implementation_->repositoryStore()
                : nullptr);
        return take(runOnStore<Domain::LegacyMemoryListOutcome>(
            store,
            "List legacy memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyMemoryListOutcome>([&]() {
                    std::string countSql =
                        "SELECT COUNT(*) FROM memory_notes WHERE 1=1";
                    if (!request.includeSystem) {
                        countSql += VisibleMemoryPredicate;
                    }
                    const auto totalValue = scalarInteger(
                        connection, countSql, context);
                    if (totalValue < 0 ||
                        static_cast<std::uint64_t>(totalValue) >
                            static_cast<std::uint64_t>(
                                (std::numeric_limits<std::size_t>::max)())) {
                        fail(integrityError(
                            "The visible legacy memory count is outside range."));
                    }

                    std::string sql = "SELECT ";
                    sql += request.includeBody
                        ? FullProjectionColumns
                        : SummaryProjectionColumns;
                    sql += " FROM memory_notes WHERE 1=1";
                    if (request.prefix) {
                        sql += " AND key LIKE ? ESCAPE '\\'";
                    }
                    if (!request.includeSystem) {
                        sql += VisibleMemoryPredicate;
                    }
                    sql += " ORDER BY updated_at DESC,key ASC LIMIT ?";
                    auto statement = take(connection.prepare(sql, context));
                    int parameter = 1;
                    if (request.prefix) {
                        take(statement.bindText(
                            parameter++, escapeLikeLiteral(*request.prefix) + "%"));
                    }
                    take(statement.bindInt64(
                        parameter, static_cast<std::int64_t>(request.limit)));
                    std::vector<Domain::LegacyMemoryNoteProjection> notes;
                    notes.reserve(request.limit);
                    for (;;) {
                        const auto stepped = take(statement.step());
                        if (stepped == WinsqliteStepResult::Done) {
                            break;
                        }
                        auto projection = readProjection(
                            statement, request.includeBody);
                        if (tagKey && !containsCanonicalTag(
                                          projection.tags,
                                          *tagKey,
                                          *implementation_->unicodeCanonicalizer)) {
                            continue;
                        }
                        notes.push_back(std::move(projection));
                    }
                    return Domain::LegacyMemoryListOutcome{
                        std::move(notes),
                        static_cast<std::size_t>(totalValue)};
                });
            }));
    });
}

Domain::Result<Domain::LegacyMemoryDeleteOutcome>
WindowsLegacyMemoryRepository::remove(
    const std::string_view key,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyMemoryDeleteOutcome>([&]() {
        requireRepositoryKey(key);
        auto& store = requireStore(
            implementation_
                ? implementation_->repositoryStore()
                : nullptr);
        return take(runOnStore<Domain::LegacyMemoryDeleteOutcome>(
            store,
            "Delete legacy memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyMemoryDeleteOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto existsStatement = take(transaction.prepare(
                        "SELECT COUNT(*) FROM memory_notes WHERE key=?"));
                    take(existsStatement.bindText(1, key));
                    if (take(existsStatement.step()) != WinsqliteStepResult::Row) {
                        fail(integrityError(
                            "The legacy memory existence query returned no row."));
                    }
                    const bool existed =
                        take(existsStatement.columnInt64(0)) != 0;
                    auto deletion = take(transaction.prepare(
                        "DELETE FROM memory_notes WHERE key=?"));
                    take(deletion.bindText(1, key));
                    stepDone(deletion);
                    auto verifyStatement = take(transaction.prepare(
                        "SELECT COUNT(*) FROM memory_notes WHERE key=?"));
                    take(verifyStatement.bindText(1, key));
                    if (take(verifyStatement.step()) != WinsqliteStepResult::Row) {
                        fail(integrityError(
                            "The legacy memory deletion verification returned no row."));
                    }
                    const bool absent =
                        take(verifyStatement.columnInt64(0)) == 0;
                    if (!absent) {
                        fail(integrityError(
                            "The legacy memory deletion could not be verified."));
                    }
                    take(transaction.commit());
                    return Domain::LegacyMemoryDeleteOutcome{
                        std::string{key},
                        existed,
                        existed,
                        Domain::isSystemMemoryKey(key)};
                });
            }));
    });
}

Domain::Result<Domain::LegacyMemorySearchOutcome>
WindowsLegacyMemoryRepository::search(
    const Domain::LegacyMemorySearchQuery& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyMemorySearchOutcome>([&]() {
        requireRepositorySearchQuery(request);
        auto& store = requireStore(
            implementation_
                ? implementation_->repositoryStore()
                : nullptr);
        return take(runOnStore<Domain::LegacyMemorySearchOutcome>(
            store,
            "Search legacy memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyMemorySearchOutcome>([&]() {
                    std::string sql = "SELECT ";
                    sql += request.includeBody
                        ? FullProjectionColumns
                        : SummaryProjectionColumns;
                    sql +=
                        " FROM memory_notes WHERE ("
                        "key LIKE ? ESCAPE '\\' OR "
                        "body LIKE ? ESCAPE '\\' OR "
                        "tags_json LIKE ? ESCAPE '\\')";
                    if (!request.includeSystem) {
                        sql += VisibleMemoryPredicate;
                    }
                    sql += " ORDER BY updated_at DESC,key ASC LIMIT ?";
                    auto statement = take(connection.prepare(sql, context));
                    const std::string pattern =
                        "%" + escapeLikeLiteral(request.query) + "%";
                    take(statement.bindText(1, pattern));
                    take(statement.bindText(2, pattern));
                    take(statement.bindText(3, pattern));
                    take(statement.bindInt64(
                        4, static_cast<std::int64_t>(request.limit)));
                    std::vector<Domain::LegacyMemoryNoteProjection> notes;
                    notes.reserve(request.limit);
                    for (;;) {
                        const auto stepped = take(statement.step());
                        if (stepped == WinsqliteStepResult::Done) {
                            break;
                        }
                        notes.push_back(readProjection(
                            statement, request.includeBody));
                    }
                    return Domain::LegacyMemorySearchOutcome{
                        request.query, std::move(notes)};
                });
            }));
    });
}

Domain::Result<Domain::LegacyMemoryPurgeOutcome>
WindowsLegacyMemoryRepository::purge(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyMemoryPurgeOutcome>([&]() {
        take(Domain::validateDestructiveConfirmation(
            confirmation, PurgeAction, PurgeScope, PurgeToken));
        if (!implementation_ || !implementation_->clock) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The legacy memory repository is closed."));
        }
        const auto timestamp = timestampText(implementation_->clock->utcNow());
        auto& store = requireStore(
            implementation_->repositoryStore());
        return take(runOnStore<Domain::LegacyMemoryPurgeOutcome>(
            store,
            "Purge legacy memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyMemoryPurgeOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    const auto removed = scalarInteger(
                        transaction, "SELECT COUNT(*) FROM memory_notes");
                    if (removed < 0 ||
                        static_cast<std::uint64_t>(removed) >
                            static_cast<std::uint64_t>(
                                (std::numeric_limits<std::size_t>::max)())) {
                        fail(integrityError(
                            "The legacy memory purge count is outside range."));
                    }
                    take(transaction.execute("DELETE FROM memory_notes"));
                    const bool verified = scalarInteger(
                        transaction, "SELECT COUNT(*) FROM memory_notes") == 0;
                    if (!verified) {
                        fail(integrityError(
                            "The transactional legacy memory purge could not be verified."));
                    }
                    auto audit = take(transaction.prepare(
                        "INSERT INTO audit_events("
                        "timestamp,tool,args_json,status,duration_ms,"
                        "occurred_at,arguments_json,mutating) "
                        "VALUES(?,'purge_legacy_memory',?,'ok',0,?,?,1)"));
                    constexpr std::string_view Arguments =
                        "{\"scope\":\"legacy-global-memory\"}";
                    take(audit.bindText(1, timestamp));
                    take(audit.bindText(2, Arguments));
                    take(audit.bindText(3, timestamp));
                    take(audit.bindText(4, Arguments));
                    stepDone(audit);
                    take(transaction.commit());
                    return Domain::LegacyMemoryPurgeOutcome{
                        static_cast<std::size_t>(removed), true};
                });
            }));
    });
}

Domain::Result<void> WindowsLegacyMemoryRepository::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->repositoryStore()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The legacy memory repository is closed."));
    }
    return implementation_->database->quickCheck(context);
}

void WindowsLegacyMemoryRepository::close() noexcept
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
            "ffffffff-ffff-4fff-bfff-fffffffffff1");
        auto correlationId = Domain::CorrelationId::parse(
            "legacy-memory-repository-close");
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
        // Idempotent best-effort noexcept boundary. The database owner retains
        // native RAII fallback release if checkpointing cannot complete.
    }
}

} // namespace ForgeConductor::Persistence::Windows
