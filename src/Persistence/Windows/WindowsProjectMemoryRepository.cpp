#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryArtifactStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "Detail/WinsqliteTransaction.h"
#include "Infrastructure/Windows/Detail/BoundedSerialExecutor.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/SecureBuffer.h"
#include "Infrastructure/Windows/Detail/UniqueBCryptHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/Win32Error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows {
namespace {

using Detail::WinsqliteConnection;
using Detail::WinsqliteStatement;
using Detail::WinsqliteStepResult;
using Detail::WinsqliteTransaction;
using Json = nlohmann::json;

constexpr std::size_t MaximumSourceKindBytes = 4U * 1024U;
constexpr std::size_t MaximumTimestampBytes = 64U;
constexpr std::size_t MaximumTagAggregateBytes =
    32U * ((128U * 2U) + 1U);
// Each database is owned by exactly one project. AUTOINCREMENT journal ids let
// transaction boundaries retain an exact window with one bounded primary-key
// range delete instead of a count or OFFSET scan per imported row.
constexpr std::int64_t MaximumRetainedEventRows = 10'000;

constexpr std::string_view RecordColumns = R"sql(
r.id,r.project_id,r.version,r.kind,r.title,r.summary,r.body,r.importance,r.confidence,
r.source_kind,r.source_reference,r.session_id,r.created_at,r.updated_at,r.last_accessed_at,
r.expires_at,r.content_hash,r.is_tombstone,r.schema_version,
COALESCE((SELECT group_concat(hex(ordered.name), ',') FROM
  (SELECT t.name AS name FROM memory_record_tags rt
   JOIN memory_tags t ON t.id=rt.tag_id WHERE rt.record_id=r.id ORDER BY t.name) ordered),'')
)sql";

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

void requireOperationActive(
    const Domain::OperationContext& context,
    const std::string_view action)
{
    take(Infrastructure::Windows::Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action));
}

template <typename T, typename Callable>
[[nodiscard]] Domain::Result<T> guarded(Callable&& callable) noexcept
{
    try {
        if constexpr (std::is_void_v<T>) {
            std::forward<Callable>(callable)();
            return Domain::Result<void>::success();
        } else {
            return Domain::Result<T>::success(std::forward<Callable>(callable)());
        }
    } catch (RepositoryFailure& failure) {
        return Domain::Result<T>::failure(std::move(failure.error));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project memory repository operation failed safely."));
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
                "The project memory database operation produced no result."));
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
                "The project memory database callback failed safely.");
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

void requireScope(
    const Domain::ProjectId& expected,
    const Domain::ProjectId& requested)
{
    if (expected != requested) {
        fail(Domain::makeError(
            Domain::ErrorCodes::ProjectScopeMismatch,
            "The request project does not match the opened project repository."));
    }
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
            "A required project memory column is null: " + std::string{field} + '.'));
    }
    return std::move(*value);
}

[[nodiscard]] std::optional<std::string> optionalText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes)
{
    return take(statement.columnText(column, maximumBytes));
}

template <typename StrongId>
[[nodiscard]] StrongId parseStrongId(
    const std::string_view value,
    const std::string_view field)
{
    auto parsed = StrongId::parse(value);
    if (!parsed) {
        fail(integrityError(
            "A persisted project memory identifier is invalid: " +
            std::string{field} + '.'));
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::Sha256Digest parseDigest(const std::string_view value)
{
    auto parsed = Domain::Sha256Digest::parse(value);
    if (!parsed) {
        fail(integrityError("A persisted project memory content hash is invalid."));
    }
    return std::move(parsed).value();
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
            "The project memory timestamp is outside the supported UTC range."));
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
            "The project memory timestamp could not be formatted."));
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
    const bool wholeSeconds =
        text.size() == 20U && text[19] == 'Z';
    const bool fractional =
        text.size() == 24U && text[19] == '.' && text[23] == 'Z';
    if ((!wholeSeconds && !fractional) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        fail(integrityError("A persisted project memory timestamp is not canonical UTC."));
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
        fail(integrityError("A persisted project memory timestamp is invalid."));
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
        fail(integrityError("A persisted project memory timestamp is outside range."));
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
           std::chrono::milliseconds{*millisecond};
}

[[nodiscard]] int hexadecimalValue(const char value) noexcept
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

[[nodiscard]] std::vector<std::string> decodeTags(
    const std::string_view aggregate,
    const Domain::ProjectMemoryLimits& limits)
{
    std::vector<std::string> tags;
    if (aggregate.empty()) {
        return tags;
    }
    std::size_t start{};
    while (start <= aggregate.size()) {
        const auto comma = aggregate.find(',', start);
        const auto end = comma == std::string_view::npos ? aggregate.size() : comma;
        const auto encoded = aggregate.substr(start, end - start);
        if (encoded.empty() || (encoded.size() % 2U) != 0U ||
            encoded.size() / 2U > limits.maximumTagBytes ||
            tags.size() >= limits.maximumTagCount) {
            fail(integrityError("Persisted project memory tags are malformed or unbounded."));
        }
        std::string tag(encoded.size() / 2U, '\0');
        for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
            const int high = hexadecimalValue(encoded[index]);
            const int low = hexadecimalValue(encoded[index + 1U]);
            if (high < 0 || low < 0) {
                fail(integrityError("Persisted project memory tag encoding is invalid."));
            }
            tag[index / 2U] = static_cast<char>((high << 4) | low);
        }
        tags.push_back(std::move(tag));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1U;
    }
    std::sort(tags.begin(), tags.end());
    return tags;
}

void requirePersistedUtf8(
    const std::string_view value,
    const std::string_view field)
{
    if (value.find('\0') != std::string_view::npos ||
        !Infrastructure::Windows::Detail::strictUtf8ToUtf16(value)) {
        fail(integrityError(
            "Persisted project memory contains invalid UTF-8: " +
            std::string{field} + '.'));
    }
}

[[nodiscard]] Domain::Sha256Digest contentHash(
    const Domain::ProjectMemoryWrite& write,
    Contracts::IHasher& hasher);

[[nodiscard]] Domain::Sha256Digest tombstoneContentHash(
    const Domain::ProjectMemoryWrite& write,
    const Domain::MemoryRecordId& recordId,
    Contracts::IHasher& hasher);

[[nodiscard]] Domain::ProjectMemoryRecord readRecord(
    const WinsqliteStatement& statement,
    const bool includeBody,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IHasher& hasher)
{
    const auto version = take(statement.columnInt64(2));
    const auto schemaVersion = take(statement.columnInt64(18));
    if (version <= 0 ||
        version > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        fail(integrityError("A persisted project memory version is invalid."));
    }
    if (schemaVersion != Domain::ProjectMemorySchemaVersion) {
        fail(Domain::makeError(
            Domain::ErrorCodes::UnsupportedVersion,
            "The persisted project memory record schema is not supported."));
    }
    auto body = optionalText(statement, 6, limits.maximumBodyBytes);
    auto sourceReference = optionalText(
        statement, 10, limits.maximumSourceReferenceBytes);
    const auto sessionText = optionalText(statement, 11, 64U);
    std::optional<Domain::SessionId> sessionId;
    if (sessionText) {
        sessionId.emplace(parseStrongId<Domain::SessionId>(*sessionText, "session_id"));
    }
    const auto expiresText = optionalText(statement, 15, MaximumTimestampBytes);
    std::optional<Domain::UtcTimePoint> expiresAt;
    if (expiresText) {
        expiresAt.emplace(parseTimestamp(*expiresText));
    }
    const auto tagsText = requiredText(
        statement, 19, MaximumTagAggregateBytes, "tags");
    auto tags = decodeTags(tagsText, limits);
    auto kind = requiredText(statement, 3, 64U, "kind");
    auto title = requiredText(statement, 4, limits.maximumTitleBytes, "title");
    auto summary = requiredText(statement, 5, limits.maximumSummaryBytes, "summary");
    auto sourceKind = requiredText(
        statement, 9, MaximumSourceKindBytes, "source_kind");
    for (const auto& [value, field] : {
             std::pair<std::string_view, std::string_view>{kind, "kind"},
             {title, "title"},
             {summary, "summary"},
             {sourceKind, "source_kind"}}) {
        requirePersistedUtf8(value, field);
    }
    if (body) {
        requirePersistedUtf8(*body, "body");
    }
    if (sourceReference) {
        requirePersistedUtf8(*sourceReference, "source_reference");
    }
    for (const auto& tag : tags) {
        requirePersistedUtf8(tag, "tag");
    }
    const double importance = take(statement.columnDouble(7));
    const double confidence = take(statement.columnDouble(8));
    const auto tombstone = take(statement.columnInt64(17));
    if (!std::isfinite(importance) || !std::isfinite(confidence) ||
        importance < 0.0 || importance > 1.0 ||
        confidence < 0.0 || confidence > 1.0 ||
        (tombstone != 0 && tombstone != 1) || sourceKind.empty()) {
        fail(integrityError(
            "A persisted project memory record violates its semantic bounds."));
    }

    Domain::ProjectMemoryWrite semantic{
        kind,
        title,
        summary,
        body,
        tags,
        importance,
        confidence,
        sourceKind,
        sourceReference,
        sessionId,
        expiresAt,
        {},
        std::nullopt};
    auto normalized = Domain::validateProjectMemoryWrite(
        semantic, limits);
    if (!normalized || normalized.value().kind != semantic.kind ||
        normalized.value().title != semantic.title ||
        normalized.value().summary != semantic.summary ||
        normalized.value().tags != semantic.tags) {
        fail(integrityError(
            "A persisted project memory record is not canonically normalized."));
    }
    auto recordId = parseStrongId<Domain::MemoryRecordId>(
        requiredText(statement, 0, 36U, "id"), "id");
    auto persistedHash = parseDigest(requiredText(
        statement, 16, 64U, "content_hash"));
    const auto semanticHash = contentHash(normalized.value(), hasher);
    const auto expectedHash = tombstone != 0
        ? tombstoneContentHash(normalized.value(), recordId, hasher)
        : semanticHash;
    // Development builds before the id-scoped tombstone fix stored the
    // bodyless semantic hash. Accept that integrity-valid spelling on read so
    // existing local databases remain recoverable; all new tombstones use the
    // collision-safe, id-scoped spelling.
    if (persistedHash != expectedHash &&
        (tombstone == 0 || persistedHash != semanticHash)) {
        fail(integrityError(
            "A persisted project memory content hash does not match its fields."));
    }

    Domain::ProjectMemoryRecord record{
        std::move(recordId),
        parseStrongId<Domain::ProjectId>(
            requiredText(statement, 1, 36U, "project_id"), "project_id"),
        static_cast<std::uint32_t>(version),
        std::move(kind),
        std::move(title),
        std::move(summary),
        std::move(body),
        std::move(tags),
        importance,
        confidence,
        std::move(sourceKind),
        std::move(sourceReference),
        std::move(sessionId),
        parseTimestamp(requiredText(statement, 12, MaximumTimestampBytes, "created_at")),
        parseTimestamp(requiredText(statement, 13, MaximumTimestampBytes, "updated_at")),
        parseTimestamp(requiredText(
            statement, 14, MaximumTimestampBytes, "last_accessed_at")),
        std::move(expiresAt),
        std::move(persistedHash),
        tombstone != 0,
        static_cast<std::uint32_t>(schemaVersion)};
    if (!includeBody) {
        record.body.reset();
    }
    return record;
}

void bindOptionalText(
    WinsqliteStatement& statement,
    const int index,
    const std::optional<std::string>& value)
{
    if (value) {
        take(statement.bindText(index, *value));
    } else {
        take(statement.bindNull(index));
    }
}

void stepDone(WinsqliteStatement& statement)
{
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        fail(integrityError("A project memory write unexpectedly returned a row."));
    }
}

[[nodiscard]] bool isFoundationWhitespace(const wchar_t value) noexcept
{
    const auto code = static_cast<unsigned int>(value);
    return (code >= 0x0009U && code <= 0x000DU) || code == 0x0020U ||
           code == 0x0085U || code == 0x00A0U || code == 0x1680U ||
           (code >= 0x2000U && code <= 0x200AU) || code == 0x2028U ||
           code == 0x2029U || code == 0x202FU || code == 0x205FU ||
           code == 0x3000U;
}

[[nodiscard]] std::string lowerTrimUtf8(const std::string_view value)
{
    auto wide = take(
        Infrastructure::Windows::Detail::strictUtf8ToUtf16(value));
    std::size_t first{};
    while (first < wide.size() && isFoundationWhitespace(wide[first])) {
        ++first;
    }
    std::size_t last = wide.size();
    while (last > first && isFoundationWhitespace(wide[last - 1U])) {
        --last;
    }
    if (first == last) {
        return {};
    }
    const auto count = last - first;
    if (count > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Project memory hash input exceeds the Windows text mapping range."));
    }
    const int required = ::LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        wide.data() + static_cast<std::ptrdiff_t>(first),
        static_cast<int>(count),
        nullptr,
        0,
        nullptr,
        nullptr,
        0);
    if (required <= 0) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project memory hash input could not be lowercased."));
    }
    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    const int written = ::LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        wide.data() + static_cast<std::ptrdiff_t>(first),
        static_cast<int>(count),
        lowered.data(),
        required,
        nullptr,
        nullptr,
        0);
    if (written != required) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Project memory hash input could not be lowercased deterministically."));
    }
    return take(
        Infrastructure::Windows::Detail::strictUtf16ToUtf8(lowered));
}

[[nodiscard]] Domain::Sha256Digest contentHash(
    const Domain::ProjectMemoryWrite& write,
    Contracts::IHasher& hasher)
{
    std::string tags;
    for (std::size_t index = 0U; index < write.tags.size(); ++index) {
        if (index != 0U) {
            tags.push_back('\x1f');
        }
        tags += write.tags[index];
    }
    const std::array<std::string, 5U> components{
        lowerTrimUtf8(write.kind),
        lowerTrimUtf8(write.title),
        lowerTrimUtf8(write.summary),
        lowerTrimUtf8(write.body.value_or("")),
        lowerTrimUtf8(tags)};
    std::string canonical;
    for (std::size_t index = 0U; index < components.size(); ++index) {
        if (index != 0U) {
            canonical.push_back('\x1e');
        }
        canonical += components[index];
    }
    const std::span<const char> characters{canonical.data(), canonical.size()};
    return take(hasher.sha256(std::as_bytes(characters)));
}

[[nodiscard]] Domain::Sha256Digest tombstoneContentHash(
    const Domain::ProjectMemoryWrite& write,
    const Domain::MemoryRecordId& recordId,
    Contracts::IHasher& hasher)
{
    const auto semanticHash = contentHash(write, hasher);
    const std::string canonical =
        "forge-project-memory-tombstone-v1\x1e" + recordId.value() +
        "\x1e" + semanticHash.value();
    const std::span<const char> characters{canonical.data(), canonical.size()};
    return take(hasher.sha256(std::as_bytes(characters)));
}

struct PreparedWrite final {
    Domain::ProjectMemoryWrite write;
    Domain::Sha256Digest hash;
};

[[nodiscard]] PreparedWrite prepareWrite(
    Domain::ProjectMemoryWrite write,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IRedactor& redactor,
    Contracts::IHasher& hasher)
{
    write = take(Domain::validateProjectMemoryWrite(std::move(write), limits));
    write.kind = take(redactor.redact(write.kind));
    write.title = take(redactor.redact(write.title));
    write.summary = take(redactor.redact(write.summary));
    if (write.body) {
        write.body = take(redactor.redact(*write.body));
    }
    if (write.sourceReference) {
        write.sourceReference = take(redactor.redact(*write.sourceReference));
    }
    for (auto& tag : write.tags) {
        tag = take(redactor.redact(tag));
    }
    write.sourceKind = take(redactor.redact(write.sourceKind));
    if (write.idempotencyKey) {
        write.idempotencyKey = take(Domain::IdempotencyKey::create(
            take(redactor.redact(write.idempotencyKey->value()))));
    }
    write = take(Domain::validateProjectMemoryWrite(std::move(write), limits));
    if (write.sourceKind.empty() || write.sourceKind.size() > MaximumSourceKindBytes) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Project memory source_kind is empty or exceeds its storage bound."));
    }
    auto hash = contentHash(write, hasher);
    return PreparedWrite{std::move(write), std::move(hash)};
}

[[nodiscard]] Json recordJson(
    const Domain::ProjectMemoryRecord& record,
    const bool includeBody,
    const std::optional<double> score)
{
    Json value{
        {"id", record.id.value()},
        {"project_id", record.projectId.value()},
        {"version", record.version},
        {"kind", record.kind},
        {"title", record.title},
        {"summary", record.summary},
        {"tags", record.tags},
        {"importance", record.importance},
        {"confidence", record.confidence},
        {"source_kind", record.sourceKind},
        {"source_reference", record.sourceReference ? Json(*record.sourceReference) : Json(nullptr)},
        {"session_id", record.sessionId ? Json(record.sessionId->value()) : Json(nullptr)},
        {"created_at", timestampText(record.createdAt)},
        {"updated_at", timestampText(record.updatedAt)},
        {"last_accessed_at", timestampText(record.lastAccessedAt)},
        {"expires_at", record.expiresAt ? Json(timestampText(*record.expiresAt)) : Json(nullptr)},
        {"content_hash", record.contentHash.value()},
        {"is_tombstone", record.isTombstone},
        {"schema_version", record.schemaVersion}};
    if (includeBody) {
        value["body"] = record.body ? Json(*record.body) : Json(nullptr);
    }
    if (score) {
        value["score"] = *score;
    }
    return value;
}

// Foundation JSONSerialization with sortedKeys emits compact, lexically sorted
// objects and escapes every solidus. nlohmann/json already provides the same
// compact key ordering; retain Foundation's solidus spelling so checksums remain
// compatible with Forge Conductor 0.9.0 macOS exports.
[[nodiscard]] std::string foundationCompatibleJson(const Json& value)
{
    auto encoded = value.dump(
        -1, ' ', false, Json::error_handler_t::strict);
    const auto solidusCount = static_cast<std::size_t>(
        std::count(encoded.begin(), encoded.end(), '/'));
    if (solidusCount == 0U) {
        return encoded;
    }
    if (solidusCount > (std::numeric_limits<std::size_t>::max)() - encoded.size()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Foundation-compatible JSON exceeds the supported byte range."));
    }
    const auto originalBytes = encoded.size();
    encoded.resize(originalBytes + solidusCount);
    std::size_t read = originalBytes;
    std::size_t write = encoded.size();
    while (read != 0U) {
        const char character = encoded[--read];
        encoded[--write] = character;
        if (character == '/') {
            encoded[--write] = '\\';
        }
    }
    return encoded;
}

[[nodiscard]] std::size_t stabilizedEnvelopeBytes(Json& envelope)
{
    std::size_t encodedBytes{};
    for (std::size_t attempt = 0U; attempt < 8U; ++attempt) {
        envelope["encoded_bytes"] = encodedBytes;
        const auto measured = envelope.dump(
            -1, ' ', false, Json::error_handler_t::strict).size();
        if (measured == encodedBytes) {
            return measured;
        }
        encodedBytes = measured;
    }
    envelope["encoded_bytes"] = encodedBytes;
    return envelope.dump(-1, ' ', false, Json::error_handler_t::strict).size();
}

[[nodiscard]] std::size_t encodedPageBytes(
    const Domain::MemoryPage& page,
    const bool includeBody,
    const std::optional<std::string_view> query)
{
    Json records = Json::array();
    for (const auto& hit : page.records) {
        records.push_back(recordJson(
            hit.record,
            includeBody,
            query ? std::optional<double>{hit.score} : std::nullopt));
    }
    Json envelope{
        {"ok", true},
        {"project_id", page.projectId.value()},
        {"count", records.size()},
        {"records", std::move(records)},
        {"next_cursor", page.nextCursor ? Json(*page.nextCursor) : Json(nullptr)},
        {"truncated", page.truncated},
        {"encoded_bytes", 0U},
        {"maximum_response_bytes", page.maximumResponseBytes},
        {"schema_version", page.schemaVersion},
        {"capability_version", page.capabilityVersion}};
    if (query) {
        envelope["query"] = *query;
        envelope["ranking"] = Json::array({
            "exact_id",
            "exact_title",
            "lexical_title",
            "summary",
            "body",
            "importance",
            "confidence"});
    }
    return stabilizedEnvelopeBytes(envelope);
}

[[nodiscard]] std::size_t encodedRecordsBytes(
    const Domain::MemoryRecords& records,
    const bool includeBody)
{
    Json encodedRecords = Json::array();
    for (const auto& record : records.records) {
        encodedRecords.push_back(recordJson(record, includeBody, std::nullopt));
    }
    Json envelope{
        {"ok", true},
        {"project_id", records.projectId.value()},
        {"count", encodedRecords.size()},
        {"records", std::move(encodedRecords)},
        {"encoded_bytes", 0U},
        {"maximum_response_bytes", records.maximumResponseBytes},
        {"schema_version", records.schemaVersion},
        {"capability_version", records.capabilityVersion}};
    return stabilizedEnvelopeBytes(envelope);
}

[[nodiscard]] std::string encodeCursor(const std::size_t offset)
{
    if (static_cast<std::uint64_t>(offset) >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project memory cursor offset exceeds the supported range."));
    }
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string input = "v1:" + std::to_string(offset);
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    for (std::size_t index = 0U; index < input.size(); index += 3U) {
        const auto first = static_cast<unsigned char>(input[index]);
        const bool hasSecond = index + 1U < input.size();
        const bool hasThird = index + 2U < input.size();
        const auto second = hasSecond
            ? static_cast<unsigned char>(input[index + 1U])
            : static_cast<unsigned char>(0U);
        const auto third = hasThird
            ? static_cast<unsigned char>(input[index + 2U])
            : static_cast<unsigned char>(0U);
        output.push_back(alphabet[first >> 2U]);
        output.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
        output.push_back(hasSecond
            ? alphabet[((second & 0x0FU) << 2U) | (third >> 6U)]
            : '=');
        output.push_back(hasThird ? alphabet[third & 0x3FU] : '=');
    }
    return output;
}

[[nodiscard]] std::size_t checkedCursorOffset(
    const std::size_t offset,
    const std::size_t delta)
{
    if (delta > (std::numeric_limits<std::size_t>::max)() - offset) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project memory cursor offset overflowed."));
    }
    const auto value = offset + delta;
    if (static_cast<std::uint64_t>(value) >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project memory cursor offset exceeds the supported range."));
    }
    return value;
}

[[nodiscard]] int base64Value(const unsigned char value) noexcept
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    return value == '/' ? 63 : -1;
}

[[nodiscard]] std::size_t decodeCursor(const std::optional<std::string>& cursor)
{
    if (!cursor || cursor->empty()) {
        return 0U;
    }
    std::string decoded;
    decoded.reserve((cursor->size() / 4U) * 3U);
    for (std::size_t offset = 0U; offset < cursor->size(); offset += 4U) {
        const int first = base64Value(static_cast<unsigned char>((*cursor)[offset]));
        const int second = base64Value(static_cast<unsigned char>((*cursor)[offset + 1U]));
        const bool thirdPadding = (*cursor)[offset + 2U] == '=';
        const bool fourthPadding = (*cursor)[offset + 3U] == '=';
        const int third = thirdPadding
            ? 0
            : base64Value(static_cast<unsigned char>((*cursor)[offset + 2U]));
        const int fourth = fourthPadding
            ? 0
            : base64Value(static_cast<unsigned char>((*cursor)[offset + 3U]));
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project memory cursor is invalid."));
        }
        decoded.push_back(static_cast<char>((first << 2) | (second >> 4)));
        if (!thirdPadding) {
            decoded.push_back(static_cast<char>(((second & 0x0F) << 4) | (third >> 2)));
        }
        if (!fourthPadding) {
            decoded.push_back(static_cast<char>(((third & 0x03) << 6) | fourth));
        }
    }
    if (!decoded.starts_with("v1:")) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project memory cursor is invalid."));
    }
    std::size_t value{};
    const auto digits = std::string_view{decoded}.substr(3U);
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), value);
    if (digits.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size() ||
        value > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project memory cursor is out of range."));
    }
    return value;
}

[[nodiscard]] std::string escapeLike(const std::string_view value)
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

} // namespace

struct WindowsProjectMemoryRepository::Impl final {
    Impl(
        std::unique_ptr<WindowsProjectDatabase> ownedDatabase,
        std::shared_ptr<Contracts::IProjectMemoryArtifactStore> ownedArtifactStore,
        std::shared_ptr<Contracts::IRedactor> ownedRedactor,
        std::shared_ptr<Contracts::IHasher> ownedHasher,
        std::shared_ptr<Contracts::IContinuityDocumentCodec> ownedContinuityCodec,
        std::shared_ptr<Contracts::IUuidGenerator> ownedUuidGenerator,
        std::shared_ptr<Contracts::IClock> ownedClock,
        WindowsProjectMemoryRepositoryOptions configuredOptions) noexcept
        : database{std::move(ownedDatabase)},
          artifactStore{std::move(ownedArtifactStore)},
          redactor{std::move(ownedRedactor)},
          hasher{std::move(ownedHasher)},
          continuityCodec{std::move(ownedContinuityCodec)},
          uuidGenerator{std::move(ownedUuidGenerator)},
          clock{std::move(ownedClock)},
          options{std::move(configuredOptions)}
    {
    }

    std::unique_ptr<WindowsProjectDatabase> database;
    std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore;
    std::shared_ptr<Contracts::IRedactor> redactor;
    std::shared_ptr<Contracts::IHasher> hasher;
    std::shared_ptr<Contracts::IContinuityDocumentCodec> continuityCodec;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator;
    std::shared_ptr<Contracts::IClock> clock;
    WindowsProjectMemoryRepositoryOptions options;
    Infrastructure::Windows::Detail::BoundedSerialExecutor artifactAdmission;
};

namespace {

[[nodiscard]] WinsqliteStatement prepareStatement(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    return take(connection.prepare(sql, context));
}

[[nodiscard]] WinsqliteStatement prepareStatement(
    WinsqliteTransaction& transaction,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    static_cast<void>(context);
    return take(transaction.prepare(sql));
}

constexpr std::size_t MaximumContinuityIdentifierBytes = 4U * 1024U;
constexpr std::size_t MaximumContinuityErrorBytes = 2U * 1024U;
constexpr std::size_t MaximumContinuityEvidenceBytes = 2U * 1024U;
constexpr std::uint32_t MaximumContinuityTransitionsPerOperation = 4'096U;
constexpr std::size_t MaximumContinuityOperationsPerProject = 4'096U;
constexpr std::string_view ContinuityOperationColumns =
    "operation_id,project_id,predecessor_session_id,successor_session_id,"
    "handoff_id,state,attempt,adapter_id,idempotency_key,"
    "acknowledged_session_id,acknowledged_handoff_id,created_at,updated_at,"
    "last_error,retry_at,state_checksum,retry_resume_state";

[[nodiscard]] Domain::Sha256Digest continuityChecksum(
    const Domain::ContinuityOperationId& operationId,
    const std::string_view persistedState,
    const std::optional<Domain::SessionId>& successorSessionId,
    const Domain::ContinuityHandoffId& handoffId,
    const std::uint32_t attempt,
    Contracts::IHasher& hasher)
{
    const std::string canonical =
        operationId.value() + "|" + std::string{persistedState} + "|" +
        (successorSessionId ? successorSessionId->value() : std::string{}) + "|" +
        handoffId.value() + "|" + std::to_string(attempt);
    const std::span<const char> characters{canonical.data(), canonical.size()};
    return take(hasher.sha256(std::as_bytes(characters)));
}

template <typename StrongId>
[[nodiscard]] std::optional<StrongId> optionalStrongId(
    const WinsqliteStatement& statement,
    const int column,
    const std::string_view field)
{
    const auto text = optionalText(
        statement, column, MaximumContinuityIdentifierBytes);
    if (!text) {
        return std::nullopt;
    }
    return parseStrongId<StrongId>(*text, field);
}

[[nodiscard]] Domain::ContinuityOperation readContinuityOperation(
    const WinsqliteStatement& statement,
    Contracts::IHasher& hasher)
{
    const auto operationId = parseStrongId<Domain::ContinuityOperationId>(
        requiredText(
            statement, 0, MaximumContinuityIdentifierBytes, "continuity operation id"),
        "continuity operation id");
    const auto projectId = parseStrongId<Domain::ProjectId>(
        requiredText(
            statement, 1, MaximumContinuityIdentifierBytes, "continuity project id"),
        "continuity project id");
    const auto predecessor = parseStrongId<Domain::SessionId>(
        requiredText(
            statement, 2, MaximumContinuityIdentifierBytes, "predecessor session id"),
        "predecessor session id");
    auto successor = optionalStrongId<Domain::SessionId>(
        statement, 3, "successor session id");
    const auto handoffId = parseStrongId<Domain::ContinuityHandoffId>(
        requiredText(
            statement, 4, MaximumContinuityIdentifierBytes, "continuity handoff id"),
        "continuity handoff id");
    const auto stateText = requiredText(
        statement, 5, 64U, "continuity state");
    const auto state = take(Domain::parseContinuityStateWireName(stateText));
    const auto attemptValue = take(statement.columnInt64(6));
    if (attemptValue < 0 ||
        static_cast<std::uint64_t>(attemptValue) >
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        fail(integrityError("A persisted continuity attempt is outside range."));
    }
    const auto attempt = static_cast<std::uint32_t>(attemptValue);
    const auto adapterId = parseStrongId<Domain::AdapterId>(
        requiredText(statement, 7, MaximumContinuityIdentifierBytes, "adapter id"),
        "adapter id");
    auto idempotencyResult = Domain::IdempotencyKey::create(requiredText(
        statement, 8, Domain::IdempotencyKey::MaximumBytes, "idempotency key"));
    if (!idempotencyResult) {
        fail(integrityError("A persisted continuity idempotency key is invalid."));
    }
    const auto idempotencyKey = std::move(idempotencyResult).value();
    auto acknowledgedSession = optionalStrongId<Domain::SessionId>(
        statement, 9, "acknowledged session id");
    auto acknowledgedHandoff = optionalStrongId<Domain::ContinuityHandoffId>(
        statement, 10, "acknowledged handoff id");
    const auto createdAt = parseTimestamp(requiredText(
        statement, 11, MaximumTimestampBytes, "continuity created timestamp"));
    const auto updatedAt = parseTimestamp(requiredText(
        statement, 12, MaximumTimestampBytes, "continuity updated timestamp"));
    auto lastError = optionalText(statement, 13, MaximumContinuityErrorBytes);
    std::optional<Domain::UtcTimePoint> retryAt;
    if (const auto value = optionalText(statement, 14, MaximumTimestampBytes)) {
        retryAt = parseTimestamp(*value);
    }
    const auto persistedChecksum = parseDigest(requiredText(
        statement, 15, 64U, "continuity state checksum"));
    std::optional<Domain::ContinuityState> retryResumeState;
    if (const auto value = optionalText(statement, 16, 64U)) {
        retryResumeState = take(Domain::parseContinuityStateWireName(*value));
    }
    const auto calculatedChecksum = continuityChecksum(
        operationId,
        stateText,
        successor,
        handoffId,
        attempt,
        hasher);
    if (calculatedChecksum != persistedChecksum) {
        fail(integrityError("A persisted continuity operation checksum is invalid."));
    }
    Domain::ContinuityOperation operation{
        operationId,
        projectId,
        predecessor,
        std::move(successor),
        handoffId,
        state,
        attempt,
        adapterId,
        idempotencyKey,
        std::move(acknowledgedSession),
        std::move(acknowledgedHandoff),
        createdAt,
        updatedAt,
        std::move(lastError),
        retryAt,
        persistedChecksum,
        retryResumeState};
    take(Domain::validateContinuityOperationRetryState(operation));
    if (operation.updatedAt < operation.createdAt) {
        fail(integrityError("A persisted continuity operation has decreasing timestamps."));
    }
    return operation;
}

template <typename SqlOwner>
void validateContinuityTransitionLedger(
    SqlOwner& owner,
    const Domain::ContinuityOperation& operation,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context);

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ContinuityOperation> continuityOperationById(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    const Domain::ContinuityOperationId& operationId,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    std::optional<Domain::ContinuityOperation> operation;
    {
        std::string sql{"SELECT "};
        sql += ContinuityOperationColumns;
        sql += " FROM rollover_operations WHERE project_id=? AND operation_id=? LIMIT 1";
        auto statement = prepareStatement(owner, sql, context);
        take(statement.bindText(1, projectId.value()));
        take(statement.bindText(2, operationId.value()));
        if (take(statement.step()) == WinsqliteStepResult::Done) {
            return std::nullopt;
        }
        operation = readContinuityOperation(statement, hasher);
    }
    requireScope(projectId, operation->projectId);
    validateContinuityTransitionLedger(owner, *operation, hasher, context);
    return operation;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ContinuityOperation>
continuityOperationByIdempotencyKey(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    const Domain::IdempotencyKey& idempotencyKey,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    std::optional<Domain::ContinuityOperation> operation;
    {
        std::string sql{"SELECT "};
        sql += ContinuityOperationColumns;
        sql += " FROM rollover_operations WHERE project_id=? AND idempotency_key=? LIMIT 1";
        auto statement = prepareStatement(owner, sql, context);
        take(statement.bindText(1, projectId.value()));
        take(statement.bindText(2, idempotencyKey.value()));
        if (take(statement.step()) == WinsqliteStepResult::Done) {
            return std::nullopt;
        }
        operation = readContinuityOperation(statement, hasher);
    }
    requireScope(projectId, operation->projectId);
    validateContinuityTransitionLedger(owner, *operation, hasher, context);
    return operation;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ContinuityOperation> activeContinuityOperation(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    std::optional<Domain::ContinuityOperation> operation;
    {
        std::string sql{"SELECT "};
        sql += ContinuityOperationColumns;
        sql +=
            " FROM rollover_operations WHERE project_id=? AND "
            "state NOT IN ('predecessorSealed','completed','cancelled') "
            "ORDER BY updated_at DESC,operation_id DESC LIMIT 1";
        auto statement = prepareStatement(owner, sql, context);
        take(statement.bindText(1, projectId.value()));
        if (take(statement.step()) == WinsqliteStepResult::Done) {
            return std::nullopt;
        }
        operation = readContinuityOperation(statement, hasher);
    }
    requireScope(projectId, operation->projectId);
    validateContinuityTransitionLedger(owner, *operation, hasher, context);
    return operation;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ContinuityHandoff> continuityHandoffById(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    const Domain::ContinuityHandoffId& handoffId,
    Contracts::IContinuityDocumentCodec& codec,
    const Domain::OperationContext& context)
{
    auto statement = prepareStatement(
        owner,
        "SELECT payload_json,content_sha256 FROM continuity_handoffs "
        "WHERE project_id=? AND handoff_id=? LIMIT 1",
        context);
    take(statement.bindText(1, projectId.value()));
    take(statement.bindText(2, handoffId.value()));
    if (take(statement.step()) == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    const auto payload = requiredText(
        statement,
        0,
        Domain::MaximumContinuityHandoffEncodedBytes,
        "continuity handoff payload");
    const auto storedDigest = parseDigest(requiredText(
        statement, 1, 64U, "continuity handoff checksum"));
    auto document = take(codec.decode(payload, context));
    if (document.canonicalUtf8 != payload ||
        document.handoff.contentSha256 != storedDigest ||
        document.handoff.project.projectId != projectId ||
        document.handoff.handoffId != handoffId) {
        fail(integrityError(
            "A persisted continuity handoff is not canonical or binding-consistent."));
    }
    return std::move(document.handoff);
}

void appendContinuityTransition(
    WinsqliteTransaction& transaction,
    const Domain::ContinuityOperation& operation,
    const std::optional<Domain::ContinuityState> from,
    const Domain::ContinuityState to,
    const std::uint32_t attempt,
    const std::optional<std::string>& evidence,
    const Domain::Sha256Digest& checksum,
    const std::string_view timestamp)
{
    auto statement = take(transaction.prepare(
        "INSERT INTO rollover_transitions("
        "operation_id,project_id,from_state,to_state,attempt,created_at,"
        "adapter_id,evidence,state_checksum) VALUES(?,?,?,?,?,?,?,?,?)"));
    take(statement.bindText(1, operation.operationId.value()));
    take(statement.bindText(2, operation.projectId.value()));
    if (from) {
        take(statement.bindText(3, Domain::wireName(*from)));
    } else {
        take(statement.bindNull(3));
    }
    take(statement.bindText(4, Domain::wireName(to)));
    take(statement.bindInt64(5, static_cast<std::int64_t>(attempt)));
    take(statement.bindText(6, timestamp));
    take(statement.bindText(7, operation.adapterId.value()));
    if (evidence) {
        take(statement.bindText(
            8,
            evidence->substr(0U, MaximumContinuityEvidenceBytes)));
    } else {
        take(statement.bindNull(8));
    }
    take(statement.bindText(9, checksum.value()));
    stepDone(statement);
}

[[nodiscard]] std::int64_t changedRows(WinsqliteTransaction& transaction)
{
    auto statement = take(transaction.prepare("SELECT changes()"));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        fail(integrityError("A continuity compare-and-set returned no change count."));
    }
    return take(statement.columnInt64(0));
}

[[nodiscard]] bool stateHasDurableSuccessor(
    const Domain::ContinuityState state) noexcept
{
    return state == Domain::ContinuityState::SuccessorCreated ||
           state == Domain::ContinuityState::BootstrapSending ||
           state == Domain::ContinuityState::Acknowledged ||
           state == Domain::ContinuityState::PredecessorSealing ||
           state == Domain::ContinuityState::Completed;
}

template <typename SqlOwner>
void validateContinuityTransitionLedger(
    SqlOwner& owner,
    const Domain::ContinuityOperation& operation,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    auto statement = prepareStatement(
        owner,
        "SELECT from_state,to_state,attempt,adapter_id,state_checksum "
        "FROM rollover_transitions WHERE project_id=? AND operation_id=? "
        "ORDER BY id ASC LIMIT ?",
        context);
    take(statement.bindText(1, operation.projectId.value()));
    take(statement.bindText(2, operation.operationId.value()));
    take(statement.bindInt64(
        3,
        static_cast<std::int64_t>(
            MaximumContinuityTransitionsPerOperation) + 1));
    std::size_t count{};
    std::optional<Domain::ContinuityState> previousTo;
    std::uint32_t previousAttempt{};
    bool hasPrevious{};
    bool successorEstablished{};
    Domain::Sha256Digest lastChecksum = operation.stateChecksum;
    while (take(statement.step()) == WinsqliteStepResult::Row) {
        ++count;
        if (count > MaximumContinuityTransitionsPerOperation) {
            fail(integrityError(
                "A continuity transition ledger exceeds its durable bound."));
        }
        std::optional<Domain::ContinuityState> from;
        if (const auto fromText = optionalText(statement, 0, 64U)) {
            from = take(Domain::parseContinuityStateWireName(*fromText));
        }
        const auto toText = requiredText(
            statement, 1, 64U, "continuity transition target state");
        const auto to = take(Domain::parseContinuityStateWireName(toText));
        const auto attemptValue = take(statement.columnInt64(2));
        if (attemptValue < 0 ||
            static_cast<std::uint64_t>(attemptValue) >
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            fail(integrityError(
                "A continuity transition attempt is outside range."));
        }
        const auto attempt = static_cast<std::uint32_t>(attemptValue);
        const auto adapter = parseStrongId<Domain::AdapterId>(
            requiredText(
                statement, 3, MaximumContinuityIdentifierBytes,
                "continuity transition adapter"),
            "continuity transition adapter");
        const auto persistedChecksum = parseDigest(requiredText(
            statement, 4, 64U, "continuity transition checksum"));
        if (adapter != operation.adapterId) {
            fail(integrityError(
                "A continuity transition adapter binding is invalid."));
        }
        if (!hasPrevious) {
            if (!from) {
                if (attempt != 0U || to != Domain::ContinuityState::Idle) {
                    fail(integrityError(
                        "A continuity transition ledger has an invalid origin."));
                }
            } else if (attempt == 0U ||
                       !Domain::isAllowedContinuityTransition(*from, to)) {
                // Imported macOS ledgers may retain only a verified suffix,
                // but that suffix must still begin with a legal transition.
                fail(integrityError(
                    "An imported continuity transition suffix has an invalid origin."));
            }
            successorEstablished =
                (from && stateHasDurableSuccessor(*from)) ||
                stateHasDurableSuccessor(to);
        } else {
            if (!from || from != previousTo ||
                attempt != previousAttempt + 1U ||
                !Domain::isAllowedContinuityTransition(*from, to)) {
                fail(integrityError(
                    "A continuity transition ledger is discontinuous."));
            }
            successorEstablished = successorEstablished ||
                stateHasDurableSuccessor(to);
        }
        const std::optional<Domain::SessionId> checksumSuccessor =
            successorEstablished ? operation.successorSessionId : std::nullopt;
        const auto calculated = continuityChecksum(
            operation.operationId,
            toText,
            checksumSuccessor,
            operation.handoffId,
            attempt,
            hasher);
        if (calculated != persistedChecksum) {
            fail(integrityError(
                "A continuity transition checksum is invalid."));
        }
        previousTo = to;
        previousAttempt = attempt;
        lastChecksum = persistedChecksum;
        hasPrevious = true;
    }
    if (!hasPrevious || previousTo != operation.state ||
        previousAttempt != operation.attempt ||
        lastChecksum != operation.stateChecksum) {
        fail(integrityError(
            "A continuity operation does not match its transition ledger tail."));
    }
}

template <typename SqlOwner>
void validateAllContinuityOperations(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    std::vector<Domain::ContinuityOperationId> operationIds;
    operationIds.reserve(64U);
    {
        auto statement = prepareStatement(
            owner,
            "SELECT operation_id FROM rollover_operations WHERE project_id=? "
            "ORDER BY created_at,operation_id LIMIT ?",
            context);
        take(statement.bindText(1, projectId.value()));
        take(statement.bindInt64(
            2,
            static_cast<std::int64_t>(MaximumContinuityOperationsPerProject) + 1));
        while (take(statement.step()) == WinsqliteStepResult::Row) {
            if (operationIds.size() >= MaximumContinuityOperationsPerProject) {
                fail(integrityError(
                    "A project continuity operation ledger exceeds its durable bound."));
            }
            operationIds.push_back(parseStrongId<Domain::ContinuityOperationId>(
                requiredText(
                    statement, 0, MaximumContinuityIdentifierBytes,
                    "continuity operation id"),
                "continuity operation id"));
        }
    }
    for (const auto& operationId : operationIds) {
        const auto operation = continuityOperationById(
            owner, projectId, operationId, hasher, context);
        if (!operation) {
            fail(integrityError(
                "A continuity operation disappeared during ledger validation."));
        }
    }
}

[[nodiscard]] std::string recordSelectPrefix()
{
    std::string sql{"SELECT "};
    sql += RecordColumns;
    sql += " FROM memory_records r";
    return sql;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ProjectMemoryRecord> recordById(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    const Domain::MemoryRecordId& recordId,
    const bool includeTombstone,
    const bool includeBody,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    auto sql = recordSelectPrefix();
    sql += " WHERE r.id=? AND r.project_id=?";
    if (!includeTombstone) {
        sql += " AND r.is_tombstone=0";
    }
    sql += " LIMIT 1";
    auto statement = prepareStatement(owner, sql, context);
    take(statement.bindText(1, recordId.value()));
    take(statement.bindText(2, projectId.value()));
    const auto stepped = take(statement.step());
    if (stepped == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    auto record = readRecord(statement, includeBody, limits, hasher);
    requireScope(projectId, record.projectId);
    return record;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ProjectMemoryRecord> recordByHash(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    const std::string_view kind,
    const Domain::Sha256Digest& hash,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    auto sql = recordSelectPrefix();
    sql += " WHERE r.project_id=? AND r.kind=? AND r.content_hash=?"
           " AND r.is_tombstone=0 LIMIT 1";
    auto statement = prepareStatement(owner, sql, context);
    take(statement.bindText(1, projectId.value()));
    take(statement.bindText(2, kind));
    take(statement.bindText(3, hash.value()));
    const auto stepped = take(statement.step());
    if (stepped == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    auto record = readRecord(statement, true, limits, hasher);
    requireScope(projectId, record.projectId);
    return record;
}

template <typename SqlOwner>
[[nodiscard]] std::optional<Domain::ProjectMemoryRecord> recordByIdempotencyKey(
    SqlOwner& owner,
    const Domain::ProjectId& projectId,
    const Domain::IdempotencyKey& key,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context)
{
    auto sql = recordSelectPrefix();
    sql += " WHERE r.project_id=? AND r.idempotency_key=? LIMIT 1";
    auto statement = prepareStatement(owner, sql, context);
    take(statement.bindText(1, projectId.value()));
    take(statement.bindText(2, key.value()));
    const auto stepped = take(statement.step());
    if (stepped == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    auto record = readRecord(statement, true, limits, hasher);
    requireScope(projectId, record.projectId);
    return record;
}

[[nodiscard]] std::int64_t scalarInteger(
    WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    take(statement.bindText(1, projectId.value()));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        fail(integrityError("A project memory count query returned no row."));
    }
    const auto value = take(statement.columnInt64(0));
    if (value < 0) {
        fail(integrityError("A project memory count query returned a negative value."));
    }
    return value;
}

[[nodiscard]] std::int64_t scalarInteger(
    WinsqliteTransaction& transaction,
    const std::string_view sql,
    const Domain::ProjectId& projectId)
{
    auto statement = take(transaction.prepare(sql));
    take(statement.bindText(1, projectId.value()));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        fail(integrityError("A project memory transaction count returned no row."));
    }
    const auto value = take(statement.columnInt64(0));
    if (value < 0) {
        fail(integrityError("A project memory transaction count was negative."));
    }
    return value;
}

void appendEvent(
    WinsqliteTransaction& transaction,
    const Domain::ProjectId& projectId,
    const std::optional<Domain::MemoryRecordId>& recordId,
    const std::string_view action,
    const std::optional<std::string>& detail,
    const std::string_view timestamp)
{
    auto statement = take(transaction.prepare(
        "INSERT INTO event_journal(project_id,record_id,action,detail,created_at) "
        "VALUES(?,?,?,?,?)"));
    take(statement.bindText(1, projectId.value()));
    if (recordId) {
        take(statement.bindText(2, recordId->value()));
    } else {
        take(statement.bindNull(2));
    }
    take(statement.bindText(3, action));
    bindOptionalText(statement, 4, detail);
    take(statement.bindText(5, timestamp));
    stepDone(statement);
}

void enforceEventJournalRetention(WinsqliteTransaction& transaction)
{
    // MAX(id) is a right-edge primary-key lookup. Any retained id must lie in
    // an integer window containing at most MaximumRetainedEventRows values,
    // including when older rows contain gaps or predate this retention rule.
    auto prune = take(transaction.prepare(
        "DELETE FROM event_journal WHERE id <= "
        "(SELECT MAX(id) - ? FROM event_journal)"));
    take(prune.bindInt64(1, MaximumRetainedEventRows));
    stepDone(prune);
}

void replaceTags(
    WinsqliteTransaction& transaction,
    const Domain::MemoryRecordId& recordId,
    const std::vector<std::string>& tags)
{
    {
        auto statement = take(transaction.prepare(
            "DELETE FROM memory_record_tags WHERE record_id=?"));
        take(statement.bindText(1, recordId.value()));
        stepDone(statement);
    }
    for (const auto& tag : tags) {
        {
            auto statement = take(transaction.prepare(
                "INSERT OR IGNORE INTO memory_tags(name) VALUES(?)"));
            take(statement.bindText(1, tag));
            stepDone(statement);
        }
        {
            auto statement = take(transaction.prepare(
                "INSERT OR IGNORE INTO memory_record_tags(record_id,tag_id) "
                "SELECT ?,id FROM memory_tags WHERE name=?"));
            take(statement.bindText(1, recordId.value()));
            take(statement.bindText(2, tag));
            stepDone(statement);
        }
    }
}

[[nodiscard]] bool linkExists(
    WinsqliteTransaction& transaction,
    const Domain::ProjectId& projectId,
    const Domain::MemoryRecordId& sourceId,
    const Domain::MemoryRecordId& targetId,
    const std::string_view relation)
{
    auto statement = take(transaction.prepare(
        "SELECT 1 FROM memory_links WHERE project_id=? AND source_id=? "
        "AND target_id=? AND relation=? LIMIT 1"));
    take(statement.bindText(1, projectId.value()));
    take(statement.bindText(2, sourceId.value()));
    take(statement.bindText(3, targetId.value()));
    take(statement.bindText(4, relation));
    return take(statement.step()) == WinsqliteStepResult::Row;
}

void insertLink(
    WinsqliteTransaction& transaction,
    const Domain::ProjectId& projectId,
    const Domain::MemoryRecordId& sourceId,
    const Domain::MemoryRecordId& targetId,
    const std::string_view relation,
    const std::string_view timestamp)
{
    auto statement = take(transaction.prepare(
        "INSERT OR IGNORE INTO memory_links"
        "(project_id,source_id,target_id,relation,created_at) VALUES(?,?,?,?,?)"));
    take(statement.bindText(1, projectId.value()));
    take(statement.bindText(2, sourceId.value()));
    take(statement.bindText(3, targetId.value()));
    take(statement.bindText(4, relation));
    take(statement.bindText(5, timestamp));
    stepDone(statement);
}

[[nodiscard]] Domain::MemoryWriteOutcome rememberPrepared(
    WinsqliteTransaction& transaction,
    const Domain::ProjectId& projectId,
    PreparedWrite prepared,
    Contracts::IHasher& hasher,
    Contracts::IUuidGenerator& uuidGenerator,
    const Contracts::IClock& clock,
    const Domain::ProjectMemoryLimits& limits,
    const Domain::OperationContext& context)
{
    if (prepared.write.idempotencyKey) {
        const auto existing = recordByIdempotencyKey(
            transaction,
            projectId,
            *prepared.write.idempotencyKey,
            limits,
            hasher,
            context);
        if (existing) {
            return Domain::MemoryWriteOutcome{
                projectId,
                existing->id,
                existing->version,
                Domain::MemoryWriteDisposition::Deduplicated,
                existing->contentHash};
        }
    }
    const auto duplicate = recordByHash(
        transaction,
        projectId,
        prepared.write.kind,
        prepared.hash,
        limits,
        hasher,
        context);
    if (duplicate) {
        return Domain::MemoryWriteOutcome{
            projectId,
            duplicate->id,
            duplicate->version,
            Domain::MemoryWriteDisposition::Deduplicated,
            duplicate->contentHash};
    }

    Domain::MemoryRecordId recordId{take(uuidGenerator.next())};
    const std::string timestamp = timestampText(clock.utcNow());
    {
        auto statement = take(transaction.prepare(R"sql(
INSERT INTO memory_records(
 id,project_id,version,kind,title,summary,body,importance,confidence,source_kind,
 source_reference,session_id,created_at,updated_at,last_accessed_at,expires_at,
 content_hash,is_tombstone,schema_version,idempotency_key)
VALUES(?,?,1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?)
)sql"));
        take(statement.bindText(1, recordId.value()));
        take(statement.bindText(2, projectId.value()));
        take(statement.bindText(3, prepared.write.kind));
        take(statement.bindText(4, prepared.write.title));
        take(statement.bindText(5, prepared.write.summary));
        bindOptionalText(statement, 6, prepared.write.body);
        take(statement.bindDouble(7, prepared.write.importance));
        take(statement.bindDouble(8, prepared.write.confidence));
        take(statement.bindText(9, prepared.write.sourceKind));
        bindOptionalText(statement, 10, prepared.write.sourceReference);
        if (prepared.write.sessionId) {
            take(statement.bindText(11, prepared.write.sessionId->value()));
        } else {
            take(statement.bindNull(11));
        }
        take(statement.bindText(12, timestamp));
        take(statement.bindText(13, timestamp));
        take(statement.bindText(14, timestamp));
        if (prepared.write.expiresAt) {
            take(statement.bindText(15, timestampText(*prepared.write.expiresAt)));
        } else {
            take(statement.bindNull(15));
        }
        take(statement.bindText(16, prepared.hash.value()));
        take(statement.bindInt64(17, Domain::ProjectMemorySchemaVersion));
        if (prepared.write.idempotencyKey) {
            take(statement.bindText(18, prepared.write.idempotencyKey->value()));
        } else {
            take(statement.bindNull(18));
        }
        stepDone(statement);
    }
    replaceTags(transaction, recordId, prepared.write.tags);
    for (const auto& relatedId : prepared.write.relatedIds) {
        if (recordById(
                transaction,
                projectId,
                relatedId,
                false,
                false,
                limits,
                hasher,
                context)) {
            insertLink(
                transaction,
                projectId,
                recordId,
                relatedId,
                "related",
                timestamp);
        }
    }
    appendEvent(
        transaction,
        projectId,
        recordId,
        "inserted",
        prepared.hash.value(),
        timestamp);
    return Domain::MemoryWriteOutcome{
        projectId,
        std::move(recordId),
        1U,
        Domain::MemoryWriteDisposition::Inserted,
        std::move(prepared.hash)};
}

[[nodiscard]] Domain::Result<Domain::MemoryPage> queryPage(
    WinsqliteConnection& connection,
    const Domain::ProjectId& projectId,
    const std::vector<std::string>& kinds,
    const std::optional<Domain::SessionId>& sessionId,
    const std::optional<std::string>& query,
    const std::vector<std::string>& tags,
    const std::size_t pageLimit,
    const std::size_t offset,
    const bool includeBody,
    const std::size_t maximumResponseBytes,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IHasher& hasher,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::MemoryPage>([&]() {
        const bool searching = query.has_value();
        std::string sql{"SELECT "};
        sql += RecordColumns;
        if (searching) {
            sql += R"sql(,
(CASE WHEN lower(r.id)=lower(?) THEN 1000 ELSE 0 END +
 CASE WHEN lower(r.title)=lower(?) THEN 400
      WHEN r.title LIKE ? ESCAPE '\' THEN 200 ELSE 0 END +
 CASE WHEN r.summary LIKE ? ESCAPE '\' THEN 80 ELSE 0 END +
 CASE WHEN r.body LIKE ? ESCAPE '\' THEN 30 ELSE 0 END +
 r.importance * 10 + r.confidence * 5) AS rank_score
)sql";
        }
        sql += " FROM memory_records r WHERE r.project_id=? AND r.is_tombstone=0";
        std::string pattern;
        if (searching) {
            pattern = "%" + escapeLike(*query) + "%";
            sql += " AND (r.id=? OR r.title LIKE ? ESCAPE '\\'"
                   " OR r.summary LIKE ? ESCAPE '\\'"
                   " OR r.body LIKE ? ESCAPE '\\')";
        }
        if (!kinds.empty()) {
            sql += " AND r.kind IN (";
            for (std::size_t index = 0U; index < kinds.size(); ++index) {
                if (index != 0U) {
                    sql.push_back(',');
                }
                sql.push_back('?');
            }
            sql.push_back(')');
        }
        if (sessionId) {
            sql += " AND r.session_id=?";
        }
        for (std::size_t index = 0U; index < tags.size(); ++index) {
            static_cast<void>(index);
            sql += " AND EXISTS(SELECT 1 FROM memory_record_tags rt"
                   " JOIN memory_tags t ON t.id=rt.tag_id"
                   " WHERE rt.record_id=r.id AND t.name=?)";
        }
        sql += searching
            ? " ORDER BY rank_score DESC,r.updated_at DESC,r.id ASC LIMIT ? OFFSET ?"
            : " ORDER BY r.updated_at DESC,r.id ASC LIMIT ? OFFSET ?";

        auto statement = take(connection.prepare(sql, context));
        int parameter = 1;
        if (searching) {
            take(statement.bindText(parameter++, *query));
            take(statement.bindText(parameter++, *query));
            take(statement.bindText(parameter++, pattern));
            take(statement.bindText(parameter++, pattern));
            take(statement.bindText(parameter++, pattern));
        }
        take(statement.bindText(parameter++, projectId.value()));
        if (searching) {
            take(statement.bindText(parameter++, *query));
            take(statement.bindText(parameter++, pattern));
            take(statement.bindText(parameter++, pattern));
            take(statement.bindText(parameter++, pattern));
        }
        for (const auto& kind : kinds) {
            take(statement.bindText(parameter++, kind));
        }
        if (sessionId) {
            take(statement.bindText(parameter++, sessionId->value()));
        }
        for (const auto& tag : tags) {
            take(statement.bindText(parameter++, tag));
        }
        const auto sqlLimit = pageLimit + 1U;
        take(statement.bindInt64(parameter++, static_cast<std::int64_t>(sqlLimit)));
        take(statement.bindInt64(parameter, static_cast<std::int64_t>(offset)));

        Domain::MemoryPage page{
            projectId,
            {},
            std::nullopt,
            false,
            0U,
            maximumResponseBytes};
        const auto encodedQuery = query
            ? std::optional<std::string_view>{*query}
            : std::nullopt;
        std::size_t rowsSeen{};
        while (true) {
            const auto stepped = take(statement.step());
            if (stepped == WinsqliteStepResult::Done) {
                page.truncated = false;
                break;
            }
            ++rowsSeen;
            if (rowsSeen > pageLimit) {
                page.truncated = true;
                break;
            }
            auto record = readRecord(statement, includeBody, limits, hasher);
            requireScope(projectId, record.projectId);
            const double score = searching ? take(statement.columnDouble(20)) : 0.0;
            page.records.push_back(Domain::MemorySearchHit{std::move(record), score});
            page.truncated = true;
            page.nextCursor = encodeCursor(checkedCursorOffset(
                offset, page.records.size()));
            page.encodedBytes = encodedPageBytes(
                page, includeBody, encodedQuery);
            if (page.encodedBytes > maximumResponseBytes) {
                if (page.records.size() == 1U) {
                    page.truncated = false;
                    page.nextCursor.reset();
                    page.encodedBytes = encodedPageBytes(
                        page, includeBody, encodedQuery);
                    if (page.encodedBytes <= maximumResponseBytes) {
                        const auto following = take(statement.step());
                        if (following == WinsqliteStepResult::Done) {
                            return page;
                        }
                    }
                    fail(Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "The first project-memory page record exceeds the response byte limit."));
                }
                page.records.pop_back();
                page.truncated = true;
                page.nextCursor = encodeCursor(checkedCursorOffset(
                    offset, page.records.size()));
                page.encodedBytes = encodedPageBytes(
                    page, includeBody, encodedQuery);
                break;
            }
        }
        if (page.truncated && !page.records.empty()) {
            page.nextCursor = encodeCursor(checkedCursorOffset(
                offset, page.records.size()));
        } else {
            page.nextCursor.reset();
        }
        page.encodedBytes = encodedPageBytes(page, includeBody, encodedQuery);
        if (page.encodedBytes > maximumResponseBytes) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The project-memory page envelope exceeds the response byte limit."));
        }
        return page;
    });
}

constexpr std::size_t MaximumArtifactJsonDepth = 32U;

struct EncodedArtifact final {
    std::vector<std::byte> content;
    Domain::Sha256Digest checksum;
    std::size_t recordCount{};
};

struct ParsedArtifactSummary final {
    Domain::ProjectId sourceProjectId;
    Domain::Sha256Digest checksum;
    std::size_t recordCount{};
};

// Quarantine is a destructive response and therefore follows explicit failure
// provenance carried by the validation path. Domain error codes remain the
// public result vocabulary; they are not used to infer whether retained bytes
// or an injected dependency caused a failure.
enum class ArtifactFailureProvenance {
    ArtifactValidation,
    Policy,
    Dependency
};

constexpr ULONG Sha256DigestBytes = 32U;
constexpr ULONG MaximumSha256ObjectBytes = 1024U * 1024U;
constexpr std::size_t IncrementalHashChunkBytes = 1024U * 1024U;

[[nodiscard]] Domain::Result<ULONG> bcryptSha256Property(
    const BCRYPT_ALG_HANDLE algorithm,
    const wchar_t* const property) noexcept
{
    try {
        ULONG value{};
        ULONG returned{};
        const NTSTATUS status = ::BCryptGetProperty(
            algorithm,
            property,
            reinterpret_cast<PUCHAR>(&value),
            static_cast<ULONG>(sizeof(value)),
            &returned,
            0U);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<ULONG>::failure(
                Infrastructure::Windows::Detail::makeNtStatusError(
                    "read an incremental BCrypt SHA-256 property", status));
        }
        if (returned != sizeof(value)) {
            return Domain::Result<ULONG>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "BCrypt returned an unexpected incremental SHA-256 property size."));
        }
        return Domain::Result<ULONG>::success(value);
    } catch (...) {
        return Domain::Result<ULONG>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "An incremental BCrypt SHA-256 property could not be read."));
    }
}

class IncrementalSha256 final {
public:
    [[nodiscard]] static Domain::Result<IncrementalSha256> create() noexcept
    {
        try {
            BCRYPT_ALG_HANDLE rawAlgorithm{};
            NTSTATUS status = ::BCryptOpenAlgorithmProvider(
                &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
            if (!BCRYPT_SUCCESS(status)) {
                return Domain::Result<IncrementalSha256>::failure(
                    Infrastructure::Windows::Detail::makeNtStatusError(
                        "open an incremental BCrypt SHA-256 provider", status));
            }
            Infrastructure::Windows::Detail::UniqueBCryptAlgorithmHandle algorithm{
                rawAlgorithm};
            auto objectBytes = bcryptSha256Property(
                algorithm.get(), BCRYPT_OBJECT_LENGTH);
            if (!objectBytes) {
                return Domain::Result<IncrementalSha256>::failure(
                    std::move(objectBytes).error());
            }
            if (objectBytes.value() == 0U ||
                objectBytes.value() > MaximumSha256ObjectBytes) {
                return Domain::Result<IncrementalSha256>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "BCrypt reported an unsafe incremental SHA-256 object length."));
            }
            auto digestBytes = bcryptSha256Property(
                algorithm.get(), BCRYPT_HASH_LENGTH);
            if (!digestBytes) {
                return Domain::Result<IncrementalSha256>::failure(
                    std::move(digestBytes).error());
            }
            if (digestBytes.value() != Sha256DigestBytes) {
                return Domain::Result<IncrementalSha256>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "BCrypt did not expose a 32-byte incremental SHA-256 digest."));
            }

            Infrastructure::Windows::Detail::SecureBuffer hashObject{
                objectBytes.value()};
            BCRYPT_HASH_HANDLE rawHash{};
            status = ::BCryptCreateHash(
                algorithm.get(),
                &rawHash,
                reinterpret_cast<PUCHAR>(hashObject.data()),
                objectBytes.value(),
                nullptr,
                0U,
                0U);
            if (!BCRYPT_SUCCESS(status)) {
                return Domain::Result<IncrementalSha256>::failure(
                    Infrastructure::Windows::Detail::makeNtStatusError(
                        "create an incremental BCrypt SHA-256 hash", status));
            }
            Infrastructure::Windows::Detail::UniqueBCryptHashHandle hash{rawHash};
            return Domain::Result<IncrementalSha256>::success(IncrementalSha256{
                std::move(algorithm), std::move(hashObject), std::move(hash)});
        } catch (...) {
            return Domain::Result<IncrementalSha256>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The incremental BCrypt SHA-256 state could not be allocated."));
        }
    }

    IncrementalSha256(const IncrementalSha256&) = delete;
    IncrementalSha256& operator=(const IncrementalSha256&) = delete;
    IncrementalSha256(IncrementalSha256&&) noexcept = default;
    IncrementalSha256& operator=(IncrementalSha256&&) noexcept = default;

    [[nodiscard]] Domain::Result<void> update(
        const std::span<const std::byte> bytes,
        const Domain::OperationContext& context,
        ArtifactFailureProvenance& failureProvenance) noexcept
    {
        try {
            if (finished_) {
                failureProvenance = ArtifactFailureProvenance::Dependency;
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "An incremental SHA-256 hash cannot be updated after completion."));
            }
            auto active = Infrastructure::Windows::Detail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "hash a project-memory artifact");
            if (!active) {
                failureProvenance = ArtifactFailureProvenance::Policy;
                return active;
            }
            std::size_t offset{};
            while (offset < bytes.size()) {
                const auto chunkBytes = (std::min)(
                    IncrementalHashChunkBytes, bytes.size() - offset);
                const NTSTATUS status = ::BCryptHashData(
                    hash_.get(),
                    reinterpret_cast<PUCHAR>(
                        const_cast<std::byte*>(bytes.data() + offset)),
                    static_cast<ULONG>(chunkBytes),
                    0U);
                if (!BCRYPT_SUCCESS(status)) {
                    failureProvenance = ArtifactFailureProvenance::Dependency;
                    return Domain::Result<void>::failure(
                        Infrastructure::Windows::Detail::makeNtStatusError(
                            "update an incremental BCrypt SHA-256 hash", status));
                }
                offset += chunkBytes;
                active = Infrastructure::Windows::Detail::validateOperationContext(
                    context,
                    std::chrono::steady_clock::now(),
                    "hash a project-memory artifact");
                if (!active) {
                    failureProvenance = ArtifactFailureProvenance::Policy;
                    return active;
                }
            }
            return Domain::Result<void>::success();
        } catch (...) {
            failureProvenance = ArtifactFailureProvenance::Dependency;
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The incremental BCrypt SHA-256 update failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> finish(
        const Domain::OperationContext& context,
        ArtifactFailureProvenance& failureProvenance) noexcept
    {
        try {
            if (finished_) {
                failureProvenance = ArtifactFailureProvenance::Dependency;
                return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "An incremental SHA-256 hash cannot be completed twice."));
            }
            auto active = Infrastructure::Windows::Detail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "finish a project-memory artifact hash");
            if (!active) {
                failureProvenance = ArtifactFailureProvenance::Policy;
                return Domain::Result<Domain::Sha256Digest>::failure(
                    std::move(active).error());
            }
            finished_ = true;
            Infrastructure::Windows::Detail::SecureBuffer digest{
                Sha256DigestBytes};
            const NTSTATUS status = ::BCryptFinishHash(
                hash_.get(),
                reinterpret_cast<PUCHAR>(digest.data()),
                Sha256DigestBytes,
                0U);
            if (!BCRYPT_SUCCESS(status)) {
                failureProvenance = ArtifactFailureProvenance::Dependency;
                return Domain::Result<Domain::Sha256Digest>::failure(
                    Infrastructure::Windows::Detail::makeNtStatusError(
                        "finish an incremental BCrypt SHA-256 hash", status));
            }
            active = Infrastructure::Windows::Detail::validateOperationContext(
                context,
                std::chrono::steady_clock::now(),
                "finish a project-memory artifact hash");
            if (!active) {
                failureProvenance = ArtifactFailureProvenance::Policy;
                return Domain::Result<Domain::Sha256Digest>::failure(
                    std::move(active).error());
            }
            static constexpr char Hex[] = "0123456789abcdef";
            std::string encoded(Sha256DigestBytes * 2U, '0');
            for (std::size_t index = 0U; index < digest.size(); ++index) {
                const auto value = static_cast<unsigned char>(digest.data()[index]);
                encoded[index * 2U] = Hex[(value >> 4U) & 0x0fU];
                encoded[index * 2U + 1U] = Hex[value & 0x0fU];
            }
            return Domain::Sha256Digest::parse(encoded);
        } catch (...) {
            failureProvenance = ArtifactFailureProvenance::Dependency;
            return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The incremental BCrypt SHA-256 completion failed safely."));
        }
    }

private:
    IncrementalSha256(
        Infrastructure::Windows::Detail::UniqueBCryptAlgorithmHandle algorithm,
        Infrastructure::Windows::Detail::SecureBuffer hashObject,
        Infrastructure::Windows::Detail::UniqueBCryptHashHandle hash) noexcept
        : algorithm_{std::move(algorithm)},
          hashObject_{std::move(hashObject)},
          hash_{std::move(hash)}
    {
    }

    Infrastructure::Windows::Detail::UniqueBCryptAlgorithmHandle algorithm_;
    Infrastructure::Windows::Detail::SecureBuffer hashObject_;
    Infrastructure::Windows::Detail::UniqueBCryptHashHandle hash_;
    bool finished_{};
};

using PreparedWriteConsumer = std::function<void(PreparedWrite)>;

class ArtifactValidationHasher final : public Contracts::IHasher {
public:
    ArtifactValidationHasher(
        Contracts::IHasher& hasher,
        ArtifactFailureProvenance& failureProvenance) noexcept
        : hasher_{hasher}, failureProvenance_{failureProvenance}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        const std::span<const std::byte> bytes) noexcept override
    {
        auto result = hasher_.sha256(bytes);
        if (!result) {
            failureProvenance_ = ArtifactFailureProvenance::Dependency;
        }
        return result;
    }

private:
    Contracts::IHasher& hasher_;
    ArtifactFailureProvenance& failureProvenance_;
};

class ArtifactValidationRedactor final : public Contracts::IRedactor {
public:
    ArtifactValidationRedactor(
        Contracts::IRedactor& redactor,
        ArtifactFailureProvenance& failureProvenance) noexcept
        : redactor_{redactor}, failureProvenance_{failureProvenance}
    {
    }

    [[nodiscard]] Domain::Result<std::string> redact(
        const std::string_view value) noexcept override
    {
        auto result = redactor_.redact(value);
        if (!result) {
            // RedactionRejected is the redactor contract's explicit content
            // verdict. Any other redactor failure belongs to the dependency.
            failureProvenance_ =
                result.error().code == Domain::ErrorCodes::RedactionRejected
                    ? ArtifactFailureProvenance::ArtifactValidation
                    : ArtifactFailureProvenance::Dependency;
        }
        return result;
    }

private:
    Contracts::IRedactor& redactor_;
    ArtifactFailureProvenance& failureProvenance_;
};

void requireArtifactOperationActive(
    const Domain::OperationContext& context,
    ArtifactFailureProvenance& failureProvenance,
    const std::string_view action)
{
    auto active = Infrastructure::Windows::Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
    if (!active) {
        failureProvenance = ArtifactFailureProvenance::Policy;
        fail(std::move(active).error());
    }
}

template <std::size_t Count>
void requireExactKeys(
    const Json& value,
    const std::array<std::string_view, Count>& expected,
    const std::string_view label)
{
    if (!value.is_object() || value.size() != expected.size()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " must contain exactly the supported fields."));
    }
    for (auto item = value.begin(); item != value.end(); ++item) {
        const auto found = std::find(
            expected.begin(), expected.end(), std::string_view{item.key()});
        if (found == expected.end()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{label} + " contains an unsupported field."));
        }
    }
}

[[nodiscard]] const Json& requiredField(
    const Json& value,
    const std::string_view name,
    const std::string_view label)
{
    const auto found = value.find(std::string{name});
    if (found == value.end()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " is missing a required field."));
    }
    return *found;
}

[[nodiscard]] std::string requiredArtifactText(
    const Json& value,
    const std::string_view name,
    const std::size_t maximumBytes,
    const std::string_view label)
{
    const auto& field = requiredField(value, name, label);
    if (!field.is_string()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " must be a string."));
    }
    const auto& text = field.get_ref<const std::string&>();
    if (text.size() > maximumBytes || text.find('\0') != std::string::npos) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            std::string{label} + " exceeds its UTF-8 byte bound."));
    }
    return text;
}

[[nodiscard]] std::optional<std::string> optionalArtifactText(
    const Json& value,
    const std::string_view name,
    const std::size_t maximumBytes,
    const std::string_view label)
{
    const auto& field = requiredField(value, name, label);
    if (field.is_null()) {
        return std::nullopt;
    }
    if (!field.is_string()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " must be a string or null."));
    }
    const auto& text = field.get_ref<const std::string&>();
    if (text.size() > maximumBytes || text.find('\0') != std::string::npos) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            std::string{label} + " exceeds its UTF-8 byte bound."));
    }
    return text;
}

[[nodiscard]] std::uint32_t requiredArtifactUnsigned(
    const Json& value,
    const std::string_view name,
    const std::string_view label)
{
    const auto& field = requiredField(value, name, label);
    if (!field.is_number_unsigned()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " must be an unsigned integer."));
    }
    const auto number = field.get<std::uint64_t>();
    if (number > (std::numeric_limits<std::uint32_t>::max)()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            std::string{label} + " exceeds its integer bound."));
    }
    return static_cast<std::uint32_t>(number);
}

[[nodiscard]] double requiredArtifactNumber(
    const Json& value,
    const std::string_view name,
    const std::string_view label)
{
    const auto& field = requiredField(value, name, label);
    if (!field.is_number()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " must be numeric."));
    }
    return field.get<double>();
}

[[nodiscard]] std::vector<std::string> artifactTags(
    const Json& value,
    const Domain::ProjectMemoryLimits& limits)
{
    const auto& field = requiredField(value, "tags", "Project-memory artifact tags");
    if (!field.is_array()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact tags must be an array."));
    }
    if (field.size() > limits.maximumTagCount) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Project-memory artifact tags exceed the per-record limit."));
    }
    std::vector<std::string> tags;
    tags.reserve(field.size());
    for (const auto& tag : field) {
        if (!tag.is_string()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Every project-memory artifact tag must be a string."));
        }
        const auto& text = tag.get_ref<const std::string&>();
        if (text.size() > limits.maximumTagBytes ||
            text.find('\0') != std::string::npos) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact tag exceeds its byte limit."));
        }
        tags.push_back(text);
    }
    return tags;
}

constexpr std::array<std::string_view, 5U> ArtifactRootKeys{
    "schema_version", "project_id", "created_at", "checksum", "records"};

constexpr std::array<std::string_view, 20U> ArtifactRecordKeys{
    "id",
    "project_id",
    "version",
    "kind",
    "title",
    "summary",
    "body",
    "tags",
    "importance",
    "confidence",
    "source_kind",
    "source_reference",
    "session_id",
    "created_at",
    "updated_at",
    "last_accessed_at",
    "expires_at",
    "content_hash",
    "is_tombstone",
    "schema_version"};

template <std::size_t Count>
[[nodiscard]] std::optional<std::size_t> artifactKeyIndex(
    const std::array<std::string_view, Count>& keys,
    const std::string_view key) noexcept
{
    const auto found = std::find(keys.begin(), keys.end(), key);
    if (found == keys.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(keys.begin(), found));
}

[[nodiscard]] PreparedWrite prepareArtifactRecord(
    const Json& value,
    const Domain::ProjectId& sourceProjectId,
    std::unordered_set<std::string>& sourceRecordIds,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IRedactor& redactor,
    Contracts::IHasher& hasher,
    ArtifactFailureProvenance& failureProvenance)
{
    requireExactKeys(
        value, ArtifactRecordKeys, "A project-memory artifact record");

    const auto sourceRecordIdText = requiredArtifactText(
        value, "id", 64U, "Project-memory artifact record id");
    static_cast<void>(take(Domain::MemoryRecordId::parse(sourceRecordIdText)));
    if (!sourceRecordIds.insert(sourceRecordIdText).second) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact contains duplicate record ids."));
    }
    const auto recordProjectId = take(Domain::ProjectId::parse(
        requiredArtifactText(
            value, "project_id", 64U, "Project-memory artifact record project id")));
    if (recordProjectId != sourceProjectId) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact record does not match its envelope project."));
    }
    if (requiredArtifactUnsigned(
            value, "version", "Project-memory artifact record version") == 0U) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact record version must be positive."));
    }
    if (requiredArtifactUnsigned(
            value, "schema_version", "Project-memory artifact record schema") !=
        Domain::ProjectMemorySchemaVersion) {
        failureProvenance = ArtifactFailureProvenance::Policy;
        fail(Domain::makeError(
            Domain::ErrorCodes::UnsupportedVersion,
            "A project-memory artifact record schema is not supported."));
    }
    const auto& tombstone = requiredField(
        value, "is_tombstone", "Project-memory artifact record tombstone");
    if (!tombstone.is_boolean() || tombstone.get<bool>()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory snapshot may contain only live records."));
    }

    Domain::ProjectMemoryWrite write{};
    write.kind = requiredArtifactText(
        value, "kind", 64U, "Project-memory artifact record kind");
    write.title = requiredArtifactText(
        value,
        "title",
        limits.maximumTitleBytes,
        "Project-memory artifact record title");
    write.summary = requiredArtifactText(
        value,
        "summary",
        limits.maximumSummaryBytes,
        "Project-memory artifact record summary");
    write.body = optionalArtifactText(
        value,
        "body",
        limits.maximumBodyBytes,
        "Project-memory artifact record body");
    write.tags = artifactTags(value, limits);
    write.importance = requiredArtifactNumber(
        value, "importance", "Project-memory artifact record importance");
    write.confidence = requiredArtifactNumber(
        value, "confidence", "Project-memory artifact record confidence");
    write.sourceKind = requiredArtifactText(
        value,
        "source_kind",
        MaximumSourceKindBytes,
        "Project-memory artifact record source kind");
    write.sourceReference = optionalArtifactText(
        value,
        "source_reference",
        limits.maximumSourceReferenceBytes,
        "Project-memory artifact record source reference");
    const auto sessionId = optionalArtifactText(
        value, "session_id", 64U, "Project-memory artifact record session id");
    if (sessionId) {
        write.sessionId = take(Domain::SessionId::parse(*sessionId));
    }
    static_cast<void>(parseTimestamp(requiredArtifactText(
        value, "created_at", MaximumTimestampBytes, "Project-memory artifact created_at")));
    static_cast<void>(parseTimestamp(requiredArtifactText(
        value, "updated_at", MaximumTimestampBytes, "Project-memory artifact updated_at")));
    static_cast<void>(parseTimestamp(requiredArtifactText(
        value,
        "last_accessed_at",
        MaximumTimestampBytes,
        "Project-memory artifact last_accessed_at")));
    const auto expiresAt = optionalArtifactText(
        value,
        "expires_at",
        MaximumTimestampBytes,
        "Project-memory artifact expires_at");
    if (expiresAt) {
        write.expiresAt = parseTimestamp(*expiresAt);
    }

    auto suppliedHash = Domain::Sha256Digest::parse(requiredArtifactText(
        value, "content_hash", 64U, "Project-memory artifact record content hash"));
    if (!suppliedHash) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact record content hash is invalid."));
    }
    auto normalized = take(Domain::validateProjectMemoryWrite(write, limits));
    if (normalized.kind != write.kind || normalized.title != write.title ||
        normalized.summary != write.summary || normalized.tags != write.tags) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact record is not canonically normalized."));
    }
    // Canonicalization and hashing are dependency work. Reset provenance after
    // success so a subsequent digest mismatch remains an artifact verdict.
    failureProvenance = ArtifactFailureProvenance::Dependency;
    const auto semanticHash = contentHash(normalized, hasher);
    failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
    if (semanticHash != suppliedHash.value()) {
        fail(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A project-memory artifact record content hash does not match its fields."));
    }
    failureProvenance = ArtifactFailureProvenance::Dependency;
    auto prepared = prepareWrite(
        std::move(normalized), limits, redactor, hasher);
    failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
    prepared.write.idempotencyKey = take(Domain::IdempotencyKey::create(
        "import:" + prepared.hash.value()));
    return prepared;
}

constexpr std::size_t MaximumSchemaProbeObjectFields = 10'000U;
constexpr std::size_t MaximumSchemaProbeArrayElements = 10'000U;
constexpr std::size_t MaximumSchemaProbeKeyBytes = 4U * 1024U;
constexpr std::size_t MaximumSchemaProbeNumericTokenBytes = 128U;

// Performs a dependency-free, full-syntax pass so records-first artifacts can
// reveal a future envelope schema before current-schema record semantics,
// redaction, or hashing run. The stack retains only keys for the at-most-32
// currently open containers and is bounded by both the artifact byte ceiling
// and explicit per-container limits.
class ArtifactSchemaProbe final : public Json::json_sax_t {
public:
    ArtifactSchemaProbe(
        const Domain::ProjectMemoryLimits& limits,
        const Domain::OperationContext& context,
        ArtifactFailureProvenance& failureProvenance)
        : limits_{limits},
          context_{context},
          failureProvenance_{failureProvenance}
    {
        containers_.reserve(MaximumArtifactJsonDepth);
    }

    [[nodiscard]] bool null() override
    {
        event();
        return scalarValue(false, 0U);
    }

    [[nodiscard]] bool boolean(const bool value) override
    {
        static_cast<void>(value);
        event();
        return scalarValue(false, 0U);
    }

    [[nodiscard]] bool number_integer(const number_integer_t value) override
    {
        static_cast<void>(value);
        event();
        return scalarValue(false, 0U);
    }

    [[nodiscard]] bool number_unsigned(const number_unsigned_t value) override
    {
        event();
        return scalarValue(true, value);
    }

    [[nodiscard]] bool number_float(
        const number_float_t value,
        const string_t& token) override
    {
        static_cast<void>(value);
        event();
        if (token.size() > MaximumSchemaProbeNumericTokenBytes) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact numeric token exceeds its byte bound."));
        }
        return scalarValue(false, 0U);
    }

    [[nodiscard]] bool string(string_t& value) override
    {
        static_cast<void>(value);
        event();
        return scalarValue(false, 0U);
    }

    [[nodiscard]] bool binary(binary_t& value) override
    {
        static_cast<void>(value);
        event();
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifacts cannot contain binary JSON values."));
    }

    [[nodiscard]] bool start_object(const std::size_t elements) override
    {
        event();
        if (containers_.empty()) {
            if (rootStarted_ || complete_) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A project-memory artifact must contain one root object."));
            }
            rootStarted_ = true;
            pushContainer(ContainerKind::Object, false, false, elements);
            return true;
        }
        const bool rootRecord =
            containers_.back().kind == ContainerKind::Array &&
            containers_.back().rootRecords;
        const auto rootValue = beginContainerValue(ContainerKind::Object);
        static_cast<void>(rootValue);
        pushContainer(ContainerKind::Object, false, rootRecord, elements);
        return true;
    }

    [[nodiscard]] bool key(string_t& value) override
    {
        event();
        if (containers_.empty() ||
            containers_.back().kind != ContainerKind::Object) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON contains a misplaced object key."));
        }
        if (value.size() > MaximumSchemaProbeKeyBytes ||
            value.find('\0') != std::string::npos) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact object key exceeds its byte bound."));
        }
        auto& object = containers_.back();
        if (!object.keys.insert(value).second) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON contains a duplicate object key."));
        }
        if (object.keys.size() > MaximumSchemaProbeObjectFields) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact object exceeds its field-count bound."));
        }
        if (containers_.size() == 1U) {
            if (rootValue_ != RootValue::None) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON is missing a root field value."));
            }
            if (value == "schema_version") {
                rootValue_ = RootValue::SchemaVersion;
            } else if (value == "records") {
                rootValue_ = RootValue::Records;
            } else {
                rootValue_ = RootValue::Other;
            }
        } else if (object.rootRecord) {
            if (object.recordValue != RecordValue::None) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON is missing a record field value."));
            }
            object.recordValue = value == "schema_version"
                ? RecordValue::SchemaVersion
                : RecordValue::Other;
        }
        return true;
    }

    [[nodiscard]] bool end_object() override
    {
        event();
        if (containers_.empty() ||
            containers_.back().kind != ContainerKind::Object) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON object nesting is malformed."));
        }
        const bool root = containers_.size() == 1U;
        if (root && rootValue_ != RootValue::None) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON is missing a root field value."));
        }
        if (containers_.back().rootRecord &&
            containers_.back().recordValue != RecordValue::None) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON is missing a record field value."));
        }
        containers_.pop_back();
        if (root) {
            complete_ = true;
        }
        return true;
    }

    [[nodiscard]] bool start_array(const std::size_t elements) override
    {
        event();
        if (containers_.empty()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A project-memory artifact root must be an object."));
        }
        const auto rootValue = beginContainerValue(ContainerKind::Array);
        pushContainer(
            ContainerKind::Array,
            rootValue == RootValue::Records,
            false,
            elements);
        return true;
    }

    [[nodiscard]] bool end_array() override
    {
        event();
        if (containers_.empty() ||
            containers_.back().kind != ContainerKind::Array) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON array nesting is malformed."));
        }
        containers_.pop_back();
        return true;
    }

    [[nodiscard]] bool parse_error(
        const std::size_t position,
        const std::string& lastToken,
        const Json::exception& exception) override
    {
        static_cast<void>(position);
        static_cast<void>(lastToken);
        static_cast<void>(exception);
        parserError_ = true;
        return false;
    }

    [[nodiscard]] std::uint32_t schemaVersion() const
    {
        if (!complete_ || !containers_.empty() || !schemaVersion_) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A project-memory artifact is missing one unsigned schema_version."));
        }
        return *schemaVersion_;
    }

    [[nodiscard]] bool recordSchemaMismatch() const noexcept
    {
        return recordSchemaMismatch_;
    }

    [[nodiscard]] bool parserError() const noexcept { return parserError_; }

private:
    enum class ContainerKind { Object, Array };
    enum class RootValue { None, Other, SchemaVersion, Records };
    enum class RecordValue { None, Other, SchemaVersion };

    struct Container final {
        ContainerKind kind{ContainerKind::Object};
        std::unordered_set<std::string> keys;
        std::size_t elements{};
        bool rootRecords{};
        bool rootRecord{};
        RecordValue recordValue{RecordValue::None};
    };

    void event()
    {
        ++eventCount_;
        if ((eventCount_ & 0xFFU) == 0U) {
            requireArtifactOperationActive(
                context_, failureProvenance_, "probe a project-memory artifact schema");
        }
    }

    void accountArrayValue()
    {
        if (containers_.empty() ||
            containers_.back().kind != ContainerKind::Array) {
            return;
        }
        auto& array = containers_.back();
        ++array.elements;
        const auto maximum = array.rootRecords
            ? limits_.maximumArtifactRecords
            : MaximumSchemaProbeArrayElements;
        if (array.elements > maximum) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                array.rootRecords
                    ? "Project-memory artifact records exceed the 10,000-record limit."
                    : "A project-memory artifact array exceeds its element-count bound."));
        }
    }

    [[nodiscard]] RootValue beginContainerValue(const ContainerKind kind)
    {
        accountArrayValue();
        if (!containers_.empty() && containers_.back().rootRecord) {
            auto& record = containers_.back();
            const auto recordValue = record.recordValue;
            record.recordValue = RecordValue::None;
            if (recordValue == RecordValue::SchemaVersion) {
                static_cast<void>(kind);
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact record schema_version must be an unsigned integer."));
            }
        }
        if (containers_.size() != 1U) {
            return RootValue::None;
        }
        const auto value = rootValue_;
        rootValue_ = RootValue::None;
        if (value == RootValue::SchemaVersion) {
            static_cast<void>(kind);
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact schema_version must be an unsigned integer."));
        }
        return value;
    }

    void pushContainer(
        const ContainerKind kind,
        const bool rootRecords,
        const bool rootRecord,
        const std::size_t declaredElements)
    {
        if (containers_.size() >= MaximumArtifactJsonDepth) {
            fail(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Project-memory artifact JSON exceeds depth 32."));
        }
        const auto maximum = kind == ContainerKind::Object
            ? MaximumSchemaProbeObjectFields
            : (rootRecords
                   ? limits_.maximumArtifactRecords
                   : MaximumSchemaProbeArrayElements);
        if (declaredElements != (std::numeric_limits<std::size_t>::max)() &&
            declaredElements > maximum) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact container exceeds its element-count bound."));
        }
        Container container{};
        container.kind = kind;
        container.rootRecords = rootRecords;
        container.rootRecord = rootRecord;
        containers_.push_back(std::move(container));
    }

    [[nodiscard]] bool scalarValue(
        const bool unsignedInteger,
        const std::uint64_t unsignedValue)
    {
        if (containers_.empty()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A project-memory artifact root must be an object."));
        }
        accountArrayValue();
        if (!containers_.empty() && containers_.back().rootRecord) {
            auto& record = containers_.back();
            const auto recordValue = record.recordValue;
            record.recordValue = RecordValue::None;
            if (recordValue == RecordValue::SchemaVersion) {
                if (!unsignedInteger) {
                    fail(Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "Project-memory artifact record schema_version must be an unsigned integer."));
                }
                if (unsignedValue >
                    (std::numeric_limits<std::uint32_t>::max)()) {
                    fail(Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Project-memory artifact record schema_version exceeds its integer bound."));
                }
                if (unsignedValue != Domain::ProjectMemorySchemaVersion) {
                    recordSchemaMismatch_ = true;
                }
            }
        }
        if (containers_.size() == 1U) {
            const auto value = rootValue_;
            rootValue_ = RootValue::None;
            if (value == RootValue::SchemaVersion) {
                if (!unsignedInteger) {
                    fail(Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "Project-memory artifact schema_version must be an unsigned integer."));
                }
                if (unsignedValue >
                    (std::numeric_limits<std::uint32_t>::max)()) {
                    fail(Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Project-memory artifact schema_version exceeds its integer bound."));
                }
                schemaVersion_ = static_cast<std::uint32_t>(unsignedValue);
            }
        }
        return true;
    }

    const Domain::ProjectMemoryLimits& limits_;
    const Domain::OperationContext& context_;
    ArtifactFailureProvenance& failureProvenance_;
    std::vector<Container> containers_;
    std::optional<std::uint32_t> schemaVersion_;
    RootValue rootValue_{RootValue::None};
    std::size_t eventCount_{};
    bool rootStarted_{};
    bool complete_{};
    bool parserError_{};
    bool recordSchemaMismatch_{};
};

struct ArtifactSchemaProbeSummary final {
    std::uint32_t rootSchemaVersion{};
    bool recordSchemaMismatch{};
};

[[nodiscard]] ArtifactSchemaProbeSummary probeArtifactSchema(
    const std::span<const std::byte> bytes,
    const Domain::ProjectMemoryLimits& limits,
    const Domain::OperationContext& context,
    ArtifactFailureProvenance& failureProvenance)
{
    ArtifactSchemaProbe probe{limits, context, failureProvenance};
    const auto* const first = reinterpret_cast<const char*>(bytes.data());
    const bool parsed = Json::sax_parse(
        first,
        first + bytes.size(),
        &probe,
        Json::input_format_t::json,
        true,
        false);
    if (!parsed || probe.parserError()) {
        failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project-memory artifact is not valid strict JSON."));
    }
    requireArtifactOperationActive(
        context, failureProvenance, "complete a project-memory artifact schema probe");
    return ArtifactSchemaProbeSummary{
        probe.schemaVersion(), probe.recordSchemaMismatch()};
}

class ArtifactSaxHandler final : public Json::json_sax_t {
public:
    ArtifactSaxHandler(
        const Domain::ProjectId& targetProjectId,
        const bool allowCrossProjectMerge,
        const Domain::ProjectMemoryLimits& limits,
        Contracts::IRedactor& redactor,
        Contracts::IHasher& hasher,
        ArtifactFailureProvenance& failureProvenance,
        const Domain::OperationContext& context,
        PreparedWriteConsumer& consumer,
        IncrementalSha256 recordsHash)
        : targetProjectId_{targetProjectId},
          allowCrossProjectMerge_{allowCrossProjectMerge},
          limits_{limits},
          redactor_{redactor},
          hasher_{hasher},
          failureProvenance_{failureProvenance},
          context_{context},
          consumer_{consumer},
          recordsHash_{std::move(recordsHash)},
          rootMetadata_{Json::object()},
          currentRecord_{Json::object()}
    {
        sourceRecordIds_.reserve(limits_.maximumArtifactRecords);
    }

    [[nodiscard]] bool null() override
    {
        event();
        if (skipping_) {
            return true;
        }
        validateNullScalar();
        return scalar(Json(nullptr));
    }

    [[nodiscard]] bool boolean(const bool value) override
    {
        event();
        if (skipping_) {
            return true;
        }
        validateBooleanScalar();
        return scalar(Json(value));
    }

    [[nodiscard]] bool number_integer(const number_integer_t value) override
    {
        event();
        if (skipping_) {
            return true;
        }
        validateIntegerScalar(false);
        return scalar(Json(value));
    }

    [[nodiscard]] bool number_unsigned(const number_unsigned_t value) override
    {
        event();
        if (skipping_) {
            return true;
        }
        validateIntegerScalar(true);
        return scalar(Json(value));
    }

    [[nodiscard]] bool number_float(
        const number_float_t value,
        const string_t& token) override
    {
        static_cast<void>(token);
        event();
        if (skipping_) {
            return true;
        }
        if (token.size() > MaximumNumericTokenBytes) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact numeric token exceeds its byte bound."));
        }
        validateFloatingScalar();
        return scalar(Json(value));
    }

    [[nodiscard]] bool string(string_t& value) override
    {
        event();
        if (skipping_) {
            return true;
        }
        if (state_ == State::TagsArray) {
            if (currentRecord_.at("tags").size() >= limits_.maximumTagCount) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Project-memory artifact tags exceed the per-record limit."));
            }
            if (value.size() > limits_.maximumTagBytes ||
                value.find('\0') != std::string::npos) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "A project-memory artifact tag exceeds its byte limit."));
            }
            currentRecord_.at("tags").push_back(std::move(value));
            return true;
        }
        validateStringScalar(value);
        return scalar(Json(std::move(value)));
    }

    [[nodiscard]] bool binary(binary_t& value) override
    {
        static_cast<void>(value);
        event();
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifacts cannot contain binary JSON values."));
    }

    [[nodiscard]] bool start_object(const std::size_t elements) override
    {
        event();
        enterContainer();
        if (skipping_) {
            return true;
        }
        if (state_ == State::ExpectRoot) {
            if (elements != UnknownContainerSize &&
                elements != ArtifactRootKeys.size()) {
                return skipInvalidContainer(
                    "The project-memory artifact must contain exactly the supported fields.");
            }
            state_ = State::RootObject;
            return true;
        }
        if (state_ == State::RecordsArray) {
            requireRecordCapacity();
            if (elements != UnknownContainerSize &&
                elements != ArtifactRecordKeys.size()) {
                return skipInvalidContainer(
                    "A project-memory artifact record must contain exactly the supported fields.");
            }
            currentRecord_ = Json::object();
            recordFieldMask_ = 0U;
            currentRecordField_.reset();
            state_ = State::RecordObject;
            return true;
        }
        return skipInvalidContainer(
            "Project-memory artifact JSON contains an object in an unsupported position.");
    }

    [[nodiscard]] bool key(string_t& value) override
    {
        event();
        if (skipping_) {
            return true;
        }
        if (state_ == State::RootObject) {
            if (currentRootField_) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON is missing a root field value."));
            }
            const auto index = artifactKeyIndex(
                ArtifactRootKeys, std::string_view{value});
            if (!index) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The project-memory artifact contains an unsupported field."));
            }
            const std::uint64_t bit = std::uint64_t{1} << *index;
            if ((rootFieldMask_ & bit) != 0U) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON contains a duplicate object key."));
            }
            rootFieldMask_ |= bit;
            currentRootField_ = *index;
            return true;
        }
        if (state_ == State::RecordObject) {
            if (currentRecordField_) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON is missing a record field value."));
            }
            const auto index = artifactKeyIndex(
                ArtifactRecordKeys, std::string_view{value});
            if (!index) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A project-memory artifact record contains an unsupported field."));
            }
            const std::uint64_t bit = std::uint64_t{1} << *index;
            if ((recordFieldMask_ & bit) != 0U) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON contains a duplicate object key."));
            }
            recordFieldMask_ |= bit;
            currentRecordField_ = *index;
            return true;
        }
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact JSON contains an object key in an unsupported position."));
    }

    [[nodiscard]] bool end_object() override
    {
        event();
        if (skipping_) {
            return leaveSkippedContainer();
        }
        if (state_ == State::RecordObject) {
            if (currentRecordField_) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON is missing a record field value."));
            }
            finishRecord();
            state_ = State::RecordsArray;
            leaveContainer();
            return true;
        }
        if (state_ == State::RootObject) {
            if (currentRootField_) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact JSON is missing a root field value."));
            }
            finishRoot();
            state_ = State::Complete;
            leaveContainer();
            return true;
        }
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact JSON object nesting is malformed."));
    }

    [[nodiscard]] bool start_array(const std::size_t elements) override
    {
        event();
        enterContainer();
        if (skipping_) {
            return true;
        }
        if (state_ == State::RootObject && currentRootField_ &&
            ArtifactRootKeys[*currentRootField_] == "records") {
            if (elements != UnknownContainerSize &&
                elements > limits_.maximumArtifactRecords) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Project-memory artifact records exceed the 10,000-record limit."));
            }
            currentRootField_.reset();
            state_ = State::RecordsArray;
            updateRecordsHash("[");
            return true;
        }
        if (state_ == State::RecordObject && currentRecordField_ &&
            ArtifactRecordKeys[*currentRecordField_] == "tags") {
            if (elements != UnknownContainerSize &&
                elements > limits_.maximumTagCount) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Project-memory artifact tags exceed the per-record limit."));
            }
            currentRecord_["tags"] = Json::array();
            currentRecordField_.reset();
            state_ = State::TagsArray;
            return true;
        }
        if (state_ == State::RecordsArray) {
            requireRecordCapacity();
        }
        return skipInvalidContainer(
            "Project-memory artifact JSON contains an array in an unsupported position.");
    }

    [[nodiscard]] bool end_array() override
    {
        event();
        if (skipping_) {
            return leaveSkippedContainer();
        }
        if (state_ == State::TagsArray) {
            state_ = State::RecordObject;
            leaveContainer();
            return true;
        }
        if (state_ == State::RecordsArray) {
            updateRecordsHash("]");
            recordsClosed_ = true;
            state_ = State::RootObject;
            leaveContainer();
            return true;
        }
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact JSON array nesting is malformed."));
    }

    [[nodiscard]] bool parse_error(
        const std::size_t position,
        const std::string& lastToken,
        const Json::exception& exception) override
    {
        static_cast<void>(position);
        static_cast<void>(lastToken);
        static_cast<void>(exception);
        if (!parserError_) {
            parserError_.emplace(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project-memory artifact is not valid strict JSON."));
        }
        return false;
    }

    [[nodiscard]] std::optional<Domain::Error> takeParserError() noexcept
    {
        return std::move(parserError_);
    }

    [[nodiscard]] ParsedArtifactSummary takeSummary()
    {
        if (state_ != State::Complete || !summary_) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project-memory artifact did not contain one complete root object."));
        }
        return std::move(*summary_);
    }

private:
    enum class State {
        ExpectRoot,
        RootObject,
        RecordsArray,
        RecordObject,
        TagsArray,
        Complete
    };

    static constexpr std::size_t UnknownContainerSize =
        (std::numeric_limits<std::size_t>::max)();
    static constexpr std::size_t MaximumNumericTokenBytes = 128U;
    static constexpr std::uint64_t CompleteRootMask =
        (std::uint64_t{1} << ArtifactRootKeys.size()) - 1U;
    static constexpr std::uint64_t CompleteRecordMask =
        (std::uint64_t{1} << ArtifactRecordKeys.size()) - 1U;

    void event()
    {
        ++eventCount_;
        if ((eventCount_ & 0xFFU) == 0U) {
            requireArtifactOperationActive(
                context_, failureProvenance_, "parse a project-memory artifact");
        }
    }

    void enterContainer()
    {
        ++depth_;
        if (depth_ > MaximumArtifactJsonDepth) {
            fail(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Project-memory artifact JSON exceeds depth 32."));
        }
    }

    void leaveContainer()
    {
        if (depth_ == 0U) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Project-memory artifact JSON container nesting is malformed."));
        }
        --depth_;
    }

    [[nodiscard]] bool skipInvalidContainer(const std::string_view message)
    {
        skipping_ = true;
        skippedParentDepth_ = depth_ - 1U;
        deferredContainerError_.emplace(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, std::string{message}));
        return true;
    }

    [[nodiscard]] bool leaveSkippedContainer()
    {
        leaveContainer();
        if (depth_ == skippedParentDepth_) {
            auto error = std::move(*deferredContainerError_);
            deferredContainerError_.reset();
            skipping_ = false;
            fail(std::move(error));
        }
        return true;
    }

    void requireRecordCapacity() const
    {
        if (recordCount_ >= limits_.maximumArtifactRecords) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Project-memory artifact records exceed the 10,000-record limit."));
        }
    }

    [[nodiscard]] std::string_view activeScalarKey() const
    {
        if (state_ == State::RootObject && currentRootField_) {
            return ArtifactRootKeys[*currentRootField_];
        }
        if (state_ == State::RecordObject && currentRecordField_) {
            return ArtifactRecordKeys[*currentRecordField_];
        }
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact JSON contains a scalar in an unsupported position."));
    }

    [[nodiscard]] bool isRootScalar() const noexcept
    {
        return state_ == State::RootObject && currentRootField_.has_value();
    }

    void rejectWrongScalarType(const std::string_view key) const
    {
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact field '" + std::string{key} +
                "' has an unsupported JSON type."));
    }

    void validateNullScalar() const
    {
        const auto key = activeScalarKey();
        if (!isRootScalar() &&
            (key == "body" || key == "source_reference" ||
             key == "session_id" || key == "expires_at")) {
            return;
        }
        rejectWrongScalarType(key);
    }

    void validateBooleanScalar() const
    {
        const auto key = activeScalarKey();
        if (!isRootScalar() && key == "is_tombstone") {
            return;
        }
        rejectWrongScalarType(key);
    }

    void validateIntegerScalar(const bool isUnsigned) const
    {
        const auto key = activeScalarKey();
        if (isRootScalar()) {
            if (key == "schema_version" && isUnsigned) {
                return;
            }
            rejectWrongScalarType(key);
        }
        if ((key == "version" || key == "schema_version") && isUnsigned) {
            return;
        }
        if (key == "importance" || key == "confidence") {
            return;
        }
        rejectWrongScalarType(key);
    }

    void validateFloatingScalar() const
    {
        const auto key = activeScalarKey();
        if (!isRootScalar() && (key == "importance" || key == "confidence")) {
            return;
        }
        rejectWrongScalarType(key);
    }

    void validateStringScalar(const string_t& value) const
    {
        const auto key = activeScalarKey();
        std::size_t maximumBytes{};
        if (isRootScalar()) {
            if (key == "project_id" || key == "checksum") {
                maximumBytes = 64U;
            } else if (key == "created_at") {
                maximumBytes = MaximumTimestampBytes;
            } else {
                rejectWrongScalarType(key);
            }
        } else if (key == "id" || key == "project_id" || key == "kind" ||
                   key == "session_id" || key == "content_hash") {
            maximumBytes = 64U;
        } else if (key == "title") {
            maximumBytes = limits_.maximumTitleBytes;
        } else if (key == "summary") {
            maximumBytes = limits_.maximumSummaryBytes;
        } else if (key == "body") {
            maximumBytes = limits_.maximumBodyBytes;
        } else if (key == "source_kind") {
            maximumBytes = MaximumSourceKindBytes;
        } else if (key == "source_reference") {
            maximumBytes = limits_.maximumSourceReferenceBytes;
        } else if (key == "created_at" || key == "updated_at" ||
                   key == "last_accessed_at" || key == "expires_at") {
            maximumBytes = MaximumTimestampBytes;
        } else {
            rejectWrongScalarType(key);
        }
        if (value.size() > maximumBytes || value.find('\0') != std::string::npos) {
            fail(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A project-memory artifact string field exceeds its UTF-8 byte bound."));
        }
    }

    [[nodiscard]] bool scalar(Json value)
    {
        if (state_ == State::RootObject && currentRootField_) {
            const auto key = ArtifactRootKeys[*currentRootField_];
            if (key == "records") {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact records must be an array."));
            }
            rootMetadata_[std::string{key}] = std::move(value);
            currentRootField_.reset();
            return true;
        }
        if (state_ == State::RecordObject && currentRecordField_) {
            const auto key = ArtifactRecordKeys[*currentRecordField_];
            if (key == "tags") {
                fail(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory artifact tags must be an array."));
            }
            currentRecord_[std::string{key}] = std::move(value);
            currentRecordField_.reset();
            return true;
        }
        if (state_ == State::RecordsArray) {
            requireRecordCapacity();
        }
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Project-memory artifact JSON contains a scalar in an unsupported position."));
    }

    void updateRecordsHash(const std::string_view text)
    {
        const std::span<const char> characters{text.data(), text.size()};
        take(recordsHash_.update(
            std::as_bytes(characters), context_, failureProvenance_));
    }

    void finishRecord()
    {
        if (recordFieldMask_ != CompleteRecordMask ||
            currentRecord_.size() != ArtifactRecordKeys.size()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A project-memory artifact record must contain exactly the supported fields."));
        }
        requireArtifactOperationActive(
            context_,
            failureProvenance_,
            "validate a project-memory artifact record");
        auto recordProjectId = take(Domain::ProjectId::parse(requiredArtifactText(
            currentRecord_,
            "project_id",
            64U,
            "Project-memory artifact record project id")));
        if (!recordsProjectId_) {
            recordsProjectId_ = recordProjectId;
        } else if (*recordsProjectId_ != recordProjectId) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Project-memory artifact records do not share one project id."));
        }

        const auto canonicalRecord = foundationCompatibleJson(currentRecord_);
        if (recordCount_ != 0U) {
            updateRecordsHash(",");
        }
        updateRecordsHash(canonicalRecord);
        auto prepared = prepareArtifactRecord(
            currentRecord_,
            recordProjectId,
            sourceRecordIds_,
            limits_,
            redactor_,
            hasher_,
            failureProvenance_);
        requireArtifactOperationActive(
            context_,
            failureProvenance_,
            "consume a project-memory artifact record");
        consumer_(std::move(prepared));
        ++recordCount_;
        currentRecord_ = Json::object();
        requireArtifactOperationActive(
            context_,
            failureProvenance_,
            "complete a project-memory artifact record");
    }

    void finishRoot()
    {
        if (rootFieldMask_ != CompleteRootMask ||
            rootMetadata_.size() != ArtifactRootKeys.size() - 1U ||
            !recordsClosed_) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project-memory artifact must contain exactly the supported fields."));
        }
        if (requiredArtifactUnsigned(
                rootMetadata_,
                "schema_version",
                "Project-memory artifact schema") !=
            Domain::ProjectMemorySchemaVersion) {
            failureProvenance_ = ArtifactFailureProvenance::Policy;
            fail(Domain::makeError(
                Domain::ErrorCodes::UnsupportedVersion,
                "The project-memory artifact schema version is not supported."));
        }
        auto sourceProjectId = take(Domain::ProjectId::parse(requiredArtifactText(
            rootMetadata_,
            "project_id",
            64U,
            "Project-memory artifact project id")));
        static_cast<void>(parseTimestamp(requiredArtifactText(
            rootMetadata_,
            "created_at",
            MaximumTimestampBytes,
            "Project-memory artifact created_at")));
        auto suppliedChecksum = Domain::Sha256Digest::parse(requiredArtifactText(
            rootMetadata_,
            "checksum",
            64U,
            "Project-memory artifact checksum"));
        if (!suppliedChecksum) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The project-memory artifact checksum is invalid."));
        }
        requireArtifactOperationActive(
            context_, failureProvenance_, "verify a project-memory artifact");
        auto computedChecksum = take(recordsHash_.finish(
            context_, failureProvenance_));
        if (computedChecksum != suppliedChecksum.value()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The project-memory artifact checksum does not match its records."));
        }
        if (recordsProjectId_ && *recordsProjectId_ != sourceProjectId) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A project-memory artifact record does not match its envelope project."));
        }
        if (sourceProjectId != targetProjectId_ && !allowCrossProjectMerge_) {
            failureProvenance_ = ArtifactFailureProvenance::Policy;
            fail(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "Cross-project memory import requires explicit merge policy."));
        }
        requireArtifactOperationActive(
            context_,
            failureProvenance_,
            "complete a project-memory artifact parse");
        summary_.emplace(ParsedArtifactSummary{
            std::move(sourceProjectId),
            std::move(suppliedChecksum).value(),
            recordCount_});
    }

    const Domain::ProjectId& targetProjectId_;
    bool allowCrossProjectMerge_{};
    const Domain::ProjectMemoryLimits& limits_;
    Contracts::IRedactor& redactor_;
    Contracts::IHasher& hasher_;
    ArtifactFailureProvenance& failureProvenance_;
    const Domain::OperationContext& context_;
    PreparedWriteConsumer& consumer_;
    IncrementalSha256 recordsHash_;
    Json rootMetadata_;
    Json currentRecord_;
    std::unordered_set<std::string> sourceRecordIds_;
    std::optional<Domain::ProjectId> recordsProjectId_;
    std::optional<ParsedArtifactSummary> summary_;
    std::optional<Domain::Error> parserError_;
    std::optional<Domain::Error> deferredContainerError_;
    std::optional<std::size_t> currentRootField_;
    std::optional<std::size_t> currentRecordField_;
    std::uint64_t rootFieldMask_{};
    std::uint64_t recordFieldMask_{};
    std::size_t recordCount_{};
    std::size_t eventCount_{};
    std::size_t depth_{};
    std::size_t skippedParentDepth_{};
    State state_{State::ExpectRoot};
    bool recordsClosed_{};
    bool skipping_{};
};

[[nodiscard]] ParsedArtifactSummary parseArtifact(
    const std::span<const std::byte> bytes,
    const Domain::ProjectId& targetProjectId,
    const bool allowCrossProjectMerge,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IRedactor& redactor,
    Contracts::IHasher& hasher,
    PreparedWriteConsumer consumer,
    const bool probeSchemaFirst,
    ArtifactFailureProvenance& failureProvenance,
    const Domain::OperationContext& context)
{
    failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
    requireArtifactOperationActive(
        context, failureProvenance, "parse a project-memory artifact");
    if (bytes.empty() || bytes.size() > limits.maximumArtifactBytes) {
        fail(Domain::makeError(
            bytes.empty() ? Domain::ErrorCodes::InvalidRequest
                          : Domain::ErrorCodes::PayloadTooLarge,
            bytes.empty() ? "A project-memory artifact cannot be empty."
                          : "A project-memory artifact exceeds the 32 MiB limit."));
    }
    try {
        if (probeSchemaFirst) {
            const auto probedSchema = probeArtifactSchema(
                bytes, limits, context, failureProvenance);
            if (probedSchema.rootSchemaVersion !=
                    Domain::ProjectMemorySchemaVersion ||
                probedSchema.recordSchemaMismatch) {
                failureProvenance = ArtifactFailureProvenance::Policy;
                fail(Domain::makeError(
                    Domain::ErrorCodes::UnsupportedVersion,
                    "The project-memory artifact schema version is not supported."));
            }
        }
        ArtifactValidationRedactor validationRedactor{
            redactor, failureProvenance};
        ArtifactValidationHasher validationHasher{
            hasher, failureProvenance};
        auto recordsHashResult = IncrementalSha256::create();
        if (!recordsHashResult) {
            failureProvenance = ArtifactFailureProvenance::Dependency;
            fail(std::move(recordsHashResult).error());
        }
        ArtifactSaxHandler handler{
            targetProjectId,
            allowCrossProjectMerge,
            limits,
            validationRedactor,
            validationHasher,
            failureProvenance,
            context,
            consumer,
            take(std::move(recordsHashResult))};
        const auto* const first =
            reinterpret_cast<const char*>(bytes.data());
        const bool parsed = Json::sax_parse(
            first,
            first + bytes.size(),
            &handler,
            Json::input_format_t::json,
            true,
            false);
        if (!parsed) {
            auto parserError = handler.takeParserError();
            failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
            fail(parserError
                     ? std::move(*parserError)
                     : Domain::makeError(
                           Domain::ErrorCodes::InvalidRequest,
                           "The project-memory artifact is not valid strict JSON."));
        }
        requireArtifactOperationActive(
            context,
            failureProvenance,
            "complete a project-memory artifact parse");
        return handler.takeSummary();
    } catch (RepositoryFailure&) {
        throw;
    } catch (const Json::parse_error&) {
        failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project-memory artifact is not valid strict JSON."));
    } catch (const Json::exception&) {
        failureProvenance = ArtifactFailureProvenance::ArtifactValidation;
        fail(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project-memory artifact contains an invalid JSON value."));
    } catch (...) {
        failureProvenance = ArtifactFailureProvenance::Dependency;
        throw;
    }
}

[[nodiscard]] Domain::ProjectMemoryRecord redactExportRecord(
    Domain::ProjectMemoryRecord record,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IRedactor& redactor,
    Contracts::IHasher& hasher)
{
    Domain::ProjectMemoryWrite write{
        record.kind,
        record.title,
        record.summary,
        record.body,
        record.tags,
        record.importance,
        record.confidence,
        record.sourceKind,
        record.sourceReference,
        record.sessionId,
        record.expiresAt,
        {},
        std::nullopt};
    auto prepared = prepareWrite(
        std::move(write), limits, redactor, hasher);
    record.kind = std::move(prepared.write.kind);
    record.title = std::move(prepared.write.title);
    record.summary = std::move(prepared.write.summary);
    record.body = std::move(prepared.write.body);
    record.tags = std::move(prepared.write.tags);
    record.importance = prepared.write.importance;
    record.confidence = prepared.write.confidence;
    record.sourceKind = std::move(prepared.write.sourceKind);
    record.sourceReference = std::move(prepared.write.sourceReference);
    record.sessionId = std::move(prepared.write.sessionId);
    record.expiresAt = std::move(prepared.write.expiresAt);
    record.contentHash = std::move(prepared.hash);
    return record;
}

[[nodiscard]] Domain::Result<EncodedArtifact> encodeArtifact(
    WinsqliteConnection& connection,
    const Domain::ProjectId& projectId,
    const Domain::ProjectMemoryLimits& limits,
    Contracts::IRedactor& redactor,
    Contracts::IHasher& hasher,
    const Contracts::IClock& clock,
    const Domain::OperationContext& context) noexcept
{
    return guarded<EncodedArtifact>([&]() {
        requireOperationActive(context, "encode a project-memory artifact");
        auto sql = recordSelectPrefix();
        sql += " WHERE r.project_id=? AND r.is_tombstone=0"
               " ORDER BY r.updated_at DESC,r.id ASC LIMIT ?";
        auto statement = take(connection.prepare(sql, context));
        take(statement.bindText(1, projectId.value()));
        const auto sqlLimit = limits.maximumArtifactRecords + 1U;
        take(statement.bindInt64(2, static_cast<std::int64_t>(sqlLimit)));

        // The output vector is the only envelope-sized allocation. Each row is
        // encoded into one temporary string and immediately appended and hashed.
        std::vector<std::byte> content;
        content.reserve(limits.maximumArtifactBytes);
        const auto append = [&](const std::string_view text) {
            if (text.size() > limits.maximumArtifactBytes -
                                  (std::min)(content.size(), limits.maximumArtifactBytes)) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Project memory exceeds the 32 MiB export limit."));
            }
            for (const char character : text) {
                content.push_back(
                    static_cast<std::byte>(static_cast<unsigned char>(character)));
            }
        };

        append("{\"checksum\":\"");
        const auto checksumOffset = content.size();
        append(std::string(64U, '0'));
        append("\",\"created_at\":\"");
        const auto createdAtOffset = content.size();
        append(std::string(20U, '0'));
        append("\",\"project_id\":");
        const auto encodedProjectId = foundationCompatibleJson(Json(projectId.value()));
        append(encodedProjectId);
        append(",\"records\":[");

        auto recordsHash = take(IncrementalSha256::create());
        ArtifactFailureProvenance exportHashProvenance =
            ArtifactFailureProvenance::Dependency;
        const auto updateHash = [&](const std::string_view text) {
            const std::span<const char> characters{text.data(), text.size()};
            take(recordsHash.update(
                std::as_bytes(characters), context, exportHashProvenance));
        };
        updateHash("[");

        std::size_t count{};
        while (take(statement.step()) == WinsqliteStepResult::Row) {
            requireOperationActive(context, "encode a project-memory artifact record");
            ++count;
            if (count > limits.maximumArtifactRecords) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Project memory exceeds the 10,000-record export limit."));
            }
            auto record = redactExportRecord(
                readRecord(statement, true, limits, hasher), limits, redactor, hasher);
            requireScope(projectId, record.projectId);
            const auto encodedRecord = foundationCompatibleJson(
                recordJson(record, true, std::nullopt));
            if (count != 1U) {
                append(",");
                updateHash(",");
            }
            append(encodedRecord);
            updateHash(encodedRecord);
        }

        append("]");
        updateHash("]");
        auto checksum = take(recordsHash.finish(context, exportHashProvenance));
        const auto& checksumText = checksum.value();
        if (checksumText.size() != 64U) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A SHA-256 artifact checksum did not contain 64 hexadecimal bytes."));
        }
        for (std::size_t index = 0U; index < checksumText.size(); ++index) {
            content[checksumOffset + index] = static_cast<std::byte>(
                static_cast<unsigned char>(checksumText[index]));
        }
        const auto createdAt = timestampText(clock.utcNow());
        if (createdAt.size() != 20U) {
            fail(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "An artifact creation timestamp did not contain 20 UTF-8 bytes."));
        }
        for (std::size_t index = 0U; index < createdAt.size(); ++index) {
            content[createdAtOffset + index] = static_cast<std::byte>(
                static_cast<unsigned char>(createdAt[index]));
        }
        append(",\"schema_version\":");
        append(std::to_string(Domain::ProjectMemorySchemaVersion));
        append("}");

        requireOperationActive(context, "encode a project-memory artifact");
        take(Domain::validateExportProjectMemoryRequest(
            Domain::ExportProjectMemoryRequest{projectId},
            content.size(),
            limits));
        requireOperationActive(context, "complete a project-memory artifact export");
        return EncodedArtifact{
            std::move(content), std::move(checksum), count};
    });
}

} // namespace

WindowsProjectMemoryRepository::WindowsProjectMemoryRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsProjectMemoryRepository::~WindowsProjectMemoryRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>
WindowsProjectMemoryRepository::open(
    const Domain::ProjectId& projectId,
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IRedactor> redactor,
    std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Contracts::IClock> clock,
    WindowsProjectMemoryRepositoryOptions options,
    const Domain::OperationContext& context) noexcept
{
    if (!clock) {
        return Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository requires an injected clock."));
    }
    auto database = WindowsProjectDatabase::open(
        projectId,
        std::move(applicationPaths),
        std::move(runtimeDiagnostics),
        clock,
        options.database,
        context);
    if (!database) {
        return Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>::failure(
            std::move(database).error());
    }
    return create(
        std::move(database).value(),
        std::move(artifactStore),
        std::move(redactor),
        std::move(hasher),
        std::move(uuidGenerator),
        std::move(clock),
        std::move(options));
}

Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>
WindowsProjectMemoryRepository::open(
    const Domain::ProjectId& projectId,
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IRedactor> redactor,
    std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Contracts::IClock> clock,
    WindowsProjectMemoryRepositoryOptions options,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto artifactStore =
            std::make_shared<WindowsProjectMemoryArtifactStore>(
                applicationPaths, uuidGenerator);
        return open(
            projectId,
            std::move(applicationPaths),
            std::move(artifactStore),
            std::move(runtimeDiagnostics),
            std::move(redactor),
            std::move(hasher),
            std::move(uuidGenerator),
            std::move(clock),
            std::move(options),
            context);
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The project-memory artifact store could not be composed."));
    }
}

Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>
WindowsProjectMemoryRepository::create(
    std::unique_ptr<WindowsProjectDatabase> database,
    std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore,
    std::shared_ptr<Contracts::IRedactor> redactor,
    std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Contracts::IClock> clock,
    WindowsProjectMemoryRepositoryOptions options) noexcept
{
    try {
        if (!database || !artifactStore || !redactor || !hasher ||
            !uuidGenerator || !clock ||
            options.limits.maximumPageCount == 0U ||
            options.limits.maximumResponseBytes < 1024U ||
            options.limits.maximumArtifactRecords == 0U ||
            options.limits.maximumArtifactRecords > 10'000U ||
            options.limits.maximumArtifactBytes == 0U ||
            options.limits.maximumArtifactBytes > 32U * 1024U * 1024U) {
            return Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project memory repository dependencies and limits are required."));
        }
        auto continuityCodec = std::make_shared<
            Infrastructure::Windows::WindowsContinuityDocumentCodec>(
            hasher,
            clock);
        auto implementation = std::make_unique<Impl>(
            std::move(database),
            std::move(artifactStore),
            std::move(redactor),
            std::move(hasher),
            std::move(continuityCodec),
            std::move(uuidGenerator),
            std::move(clock),
            std::move(options));
        return Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>::success(
            std::shared_ptr<WindowsProjectMemoryRepository>{
                new WindowsProjectMemoryRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::shared_ptr<WindowsProjectMemoryRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The project memory repository could not be constructed."));
    }
}

const Domain::ProjectId& WindowsProjectMemoryRepository::projectId() const noexcept
{
    return implementation_->database->projectId();
}

Domain::Result<Domain::MemoryWriteOutcome>
WindowsProjectMemoryRepository::remember(
    const Domain::RememberProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::MemoryWriteOutcome>([&]() {
        requireScope(projectId(), request.projectId);
        auto prepared = prepareWrite(
            request.write,
            implementation_->options.limits,
            *implementation_->redactor,
            *implementation_->hasher);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        auto result = runOnStore<Domain::MemoryWriteOutcome>(
            *store,
            "Remember project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::MemoryWriteOutcome>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    auto outcome = rememberPrepared(
                        transaction,
                        request.projectId,
                        std::move(prepared),
                        *implementation_->hasher,
                        *implementation_->uuidGenerator,
                        *implementation_->clock,
                        implementation_->options.limits,
                        context);
                    enforceEventJournalRetention(transaction);
                    take(transaction.commit());
                    return outcome;
                });
            });
        return take(std::move(result));
    });
}

Domain::Result<Domain::MemoryBatchOutcome>
WindowsProjectMemoryRepository::rememberBatch(
    const Domain::RememberProjectMemoryBatchRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::MemoryBatchOutcome>([&]() {
        requireScope(projectId(), request.projectId);
        auto normalized = take(Domain::validateProjectMemoryBatch(
            request.writes, implementation_->options.limits));
        std::vector<PreparedWrite> prepared;
        prepared.reserve(normalized.size());
        std::size_t redactedBytes{};
        for (auto& write : normalized) {
            auto item = prepareWrite(
                std::move(write),
                implementation_->options.limits,
                *implementation_->redactor,
                *implementation_->hasher);
            const auto bytes = item.write.title.size() + item.write.summary.size() +
                               (item.write.body ? item.write.body->size() : 0U);
            if (bytes > implementation_->options.limits.maximumBatchBytes -
                            (std::min)(
                                redactedBytes,
                                implementation_->options.limits.maximumBatchBytes)) {
                fail(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Redacted project memory batch exceeds its byte limit."));
            }
            redactedBytes += bytes;
            prepared.push_back(std::move(item));
        }
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        auto result = runOnStore<Domain::MemoryBatchOutcome>(
            *store,
            "Remember a project memory batch",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::MemoryBatchOutcome>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    Domain::MemoryBatchOutcome outcome{request.projectId, {}};
                    outcome.results.reserve(prepared.size());
                    for (auto& item : prepared) {
                        outcome.results.push_back(rememberPrepared(
                            transaction,
                            request.projectId,
                            std::move(item),
                            *implementation_->hasher,
                            *implementation_->uuidGenerator,
                            *implementation_->clock,
                            implementation_->options.limits,
                            context));
                    }
                    enforceEventJournalRetention(transaction);
                    take(transaction.commit());
                    return outcome;
                });
            });
        return take(std::move(result));
    });
}

Domain::Result<Domain::MemoryPage> WindowsProjectMemoryRepository::search(
    const Domain::SearchProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::MemoryPage>([&]() {
        requireScope(projectId(), request.projectId);
        auto validated = take(Domain::validateSearchProjectMemoryRequest(
            request, implementation_->options.limits));
        const auto offset = decodeCursor(validated.cursor);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::MemoryPage>(
            *store,
            "Search project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return queryPage(
                    connection,
                    validated.projectId,
                    validated.kinds,
                    validated.sessionId,
                    validated.query,
                    validated.tags,
                    validated.limit,
                    offset,
                    validated.includeBody,
                    validated.maximumResponseBytes,
                    implementation_->options.limits,
                    *implementation_->hasher,
                    context);
            }));
    });
}

Domain::Result<Domain::MemoryRecords> WindowsProjectMemoryRepository::get(
    const Domain::GetProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::MemoryRecords>([&]() {
        requireScope(projectId(), request.projectId);
        take(Domain::validateGetProjectMemoryRequest(
            request, implementation_->options.limits));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::MemoryRecords>(
            *store,
            "Get project memory records",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::MemoryRecords>([&]() {
                    Domain::MemoryRecords output{
                        request.projectId,
                        {},
                        0U,
                        request.maximumResponseBytes};
                    output.records.reserve(request.ids.size());
                    for (const auto& id : request.ids) {
                        auto record = recordById(
                            connection,
                            request.projectId,
                            id,
                            false,
                            request.includeBody,
                            implementation_->options.limits,
                            *implementation_->hasher,
                            context);
                        if (!record) {
                            continue;
                        }
                        output.records.push_back(std::move(*record));
                    }
                    output.encodedBytes = encodedRecordsBytes(
                        output, request.includeBody);
                    if (output.encodedBytes > request.maximumResponseBytes) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::PayloadTooLarge,
                            "Project memory get response exceeds its byte limit."));
                    }
                    return output;
                });
            }));
    });
}

Domain::Result<Domain::ProjectMemoryRecord>
WindowsProjectMemoryRepository::update(
    const Domain::UpdateProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ProjectMemoryRecord>([&]() {
        requireScope(projectId(), request.projectId);
        auto validated = take(Domain::validateUpdateProjectMemoryRequest(
            request, implementation_->options.limits));
        if (validated.title) {
            validated.title = take(implementation_->redactor->redact(*validated.title));
        }
        if (validated.summary) {
            validated.summary = take(implementation_->redactor->redact(*validated.summary));
        }
        if (validated.body) {
            validated.body = take(implementation_->redactor->redact(*validated.body));
        }
        if (validated.tags) {
            for (auto& tag : *validated.tags) {
                tag = take(implementation_->redactor->redact(tag));
            }
        }
        validated = take(Domain::validateUpdateProjectMemoryRequest(
            std::move(validated), implementation_->options.limits));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::ProjectMemoryRecord>(
            *store,
            "Update project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ProjectMemoryRecord>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    auto current = recordById(
                        transaction,
                        validated.projectId,
                        validated.recordId,
                        false,
                        true,
                        implementation_->options.limits,
                        *implementation_->hasher,
                        context);
                    if (!current) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The project memory record was not found."));
                    }
                    if (current->version != validated.expectedVersion) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The project memory record version does not match expected_version."));
                    }
                    const std::string nextTitle =
                        validated.title.value_or(current->title);
                    const std::string nextSummary =
                        validated.summary.value_or(current->summary);
                    const std::optional<std::string> nextBody =
                        validated.body ? validated.body : current->body;
                    const std::vector<std::string> nextTags =
                        validated.tags.value_or(current->tags);
                    Domain::ProjectMemoryWrite hashInput{
                        current->kind,
                        nextTitle,
                        nextSummary,
                        nextBody,
                        nextTags,
                        current->importance,
                        current->confidence,
                        current->sourceKind,
                        current->sourceReference,
                        current->sessionId,
                        current->expiresAt,
                        {},
                        std::nullopt};
                    auto hash = contentHash(hashInput, *implementation_->hasher);
                    const auto timestamp = timestampText(implementation_->clock->utcNow());
                    {
                        auto statement = take(transaction.prepare(R"sql(
UPDATE memory_records SET version=version+1,title=?,summary=?,body=?,
 updated_at=?,last_accessed_at=?,content_hash=?
WHERE id=? AND project_id=? AND version=? AND is_tombstone=0
)sql"));
                        take(statement.bindText(1, nextTitle));
                        take(statement.bindText(2, nextSummary));
                        bindOptionalText(statement, 3, nextBody);
                        take(statement.bindText(4, timestamp));
                        take(statement.bindText(5, timestamp));
                        take(statement.bindText(6, hash.value()));
                        take(statement.bindText(7, validated.recordId.value()));
                        take(statement.bindText(8, validated.projectId.value()));
                        take(statement.bindInt64(9, validated.expectedVersion));
                        stepDone(statement);
                    }
                    replaceTags(transaction, validated.recordId, nextTags);
                    appendEvent(
                        transaction,
                        validated.projectId,
                        validated.recordId,
                        "updated",
                        hash.value(),
                        timestamp);
                    auto updated = recordById(
                        transaction,
                        validated.projectId,
                        validated.recordId,
                        false,
                        true,
                        implementation_->options.limits,
                        *implementation_->hasher,
                        context);
                    if (!updated) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The updated project memory record could not be read."));
                    }
                    enforceEventJournalRetention(transaction);
                    take(transaction.commit());
                    return std::move(*updated);
                });
            }));
    });
}

Domain::Result<Domain::ForgetOutcome> WindowsProjectMemoryRepository::forget(
    const Domain::ForgetProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ForgetOutcome>([&]() {
        requireScope(projectId(), request.projectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::ForgetOutcome>(
            *store,
            "Forget project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ForgetOutcome>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    const auto current = recordById(
                        transaction,
                        request.projectId,
                        request.recordId,
                        false,
                        false,
                        implementation_->options.limits,
                        *implementation_->hasher,
                        context);
                    if (!current) {
                        take(transaction.commit());
                        return Domain::ForgetOutcome{
                            request.projectId,
                            request.recordId,
                            Domain::ForgetDisposition::NotFound};
                    }
                    const Domain::ProjectMemoryWrite tombstoneHashInput{
                        current->kind,
                        current->title,
                        current->summary,
                        std::nullopt,
                        current->tags,
                        current->importance,
                        current->confidence,
                        current->sourceKind,
                        current->sourceReference,
                        current->sessionId,
                        current->expiresAt,
                        {},
                        std::nullopt};
                    const auto tombstoneHash = tombstoneContentHash(
                        tombstoneHashInput,
                        request.recordId,
                        *implementation_->hasher);
                    const auto timestamp = timestampText(implementation_->clock->utcNow());
                    {
                        auto statement = take(transaction.prepare(R"sql(
UPDATE memory_records SET is_tombstone=1,version=version+1,body=NULL,
 updated_at=?,last_accessed_at=?,content_hash=?
WHERE id=? AND project_id=? AND is_tombstone=0
)sql"));
                        take(statement.bindText(1, timestamp));
                        take(statement.bindText(2, timestamp));
                        take(statement.bindText(3, tombstoneHash.value()));
                        take(statement.bindText(4, request.recordId.value()));
                        take(statement.bindText(5, request.projectId.value()));
                        stepDone(statement);
                    }
                    appendEvent(
                        transaction,
                        request.projectId,
                        request.recordId,
                        "tombstoned",
                        std::nullopt,
                        timestamp);
                    enforceEventJournalRetention(transaction);
                    take(transaction.commit());
                    return Domain::ForgetOutcome{
                        request.projectId,
                        request.recordId,
                        Domain::ForgetDisposition::Tombstoned};
                });
            }));
    });
}

Domain::Result<Domain::MemoryPage> WindowsProjectMemoryRepository::listRecent(
    const Domain::ListRecentProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::MemoryPage>([&]() {
        requireScope(projectId(), request.projectId);
        auto validated = take(Domain::validateListRecentProjectMemoryRequest(
            request, implementation_->options.limits));
        const auto offset = decodeCursor(validated.cursor);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::MemoryPage>(
            *store,
            "List recent project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return queryPage(
                    connection,
                    validated.projectId,
                    validated.kinds,
                    validated.sessionId,
                    std::nullopt,
                    {},
                    validated.limit,
                    offset,
                    validated.includeBody,
                    validated.maximumResponseBytes,
                    implementation_->options.limits,
                    *implementation_->hasher,
                    context);
            }));
    });
}

Domain::Result<Domain::LinkOutcome> WindowsProjectMemoryRepository::link(
    const Domain::LinkProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LinkOutcome>([&]() {
        requireScope(projectId(), request.projectId);
        auto validated = take(Domain::validateLinkProjectMemoryRequest(request));
        validated.relation = take(
            implementation_->redactor->redact(validated.relation));
        validated = take(Domain::validateLinkProjectMemoryRequest(
            std::move(validated)));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::LinkOutcome>(
            *store,
            "Link project memory records",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LinkOutcome>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    if (!recordById(
                            transaction,
                            validated.projectId,
                            validated.sourceId,
                            false,
                            false,
                            implementation_->options.limits,
                            *implementation_->hasher,
                            context) ||
                        !recordById(
                            transaction,
                            validated.projectId,
                            validated.targetId,
                            false,
                            false,
                            implementation_->options.limits,
                            *implementation_->hasher,
                            context)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The source or target project memory record was not found."));
                    }
                    const bool duplicate = linkExists(
                        transaction,
                        validated.projectId,
                        validated.sourceId,
                        validated.targetId,
                        validated.relation);
                    if (!duplicate) {
                        insertLink(
                            transaction,
                            validated.projectId,
                            validated.sourceId,
                            validated.targetId,
                            validated.relation,
                            timestampText(implementation_->clock->utcNow()));
                    }
                    take(transaction.commit());
                    return Domain::LinkOutcome{
                        validated.projectId,
                        duplicate
                            ? Domain::LinkDisposition::Deduplicated
                            : Domain::LinkDisposition::Inserted};
                });
            }));
    });
}

Domain::Result<Domain::ProjectMemoryExport>
WindowsProjectMemoryRepository::exportMemory(
    const Domain::ExportProjectMemoryRequest& request,
    const Contracts::WorkspaceAuthority& writeAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ProjectMemoryExport>([&]() {
        // Capability validation deliberately precedes even the first database
        // read so a mismatched export call cannot use record count or size as
        // an authorization oracle.
        if (request.projectId != projectId() ||
            writeAuthority.projectId() != projectId() ||
            writeAuthority.intent() != Domain::FileAccess::Write ||
            !authorization.matches(writeAuthority, context) ||
            !authorization.matchesProject(projectId()) ||
            authorization.toolName() != "project_memory.export" ||
            authorization.effect() != Domain::ToolEffect::Write) {
            fail(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The project-memory export authority or tool capability is mismatched."));
        }
        auto artifactAdmission = take(implementation_->artifactAdmission.acquire(
            context, "Export project memory"));
        static_cast<void>(artifactAdmission);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        auto encoded = take(runOnStore<EncodedArtifact>(
            *store,
            "Export project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return encodeArtifact(
                    connection,
                    request.projectId,
                    implementation_->options.limits,
                    *implementation_->redactor,
                    *implementation_->hasher,
                    *implementation_->clock,
                    context);
            }));
        auto artifact = take(implementation_->artifactStore->publish(
            request.projectId,
            encoded.content,
            writeAuthority,
            authorization,
            context));
        return Domain::ProjectMemoryExport{
            request.projectId,
            std::move(artifact),
            std::move(encoded.checksum),
            encoded.recordCount};
    });
}

Domain::Result<Domain::ProjectMemoryImport>
WindowsProjectMemoryRepository::importMemory(
    const Domain::ImportProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ProjectMemoryImport>([&]() {
        requireScope(projectId(), request.projectId);
        auto artifactAdmission = take(implementation_->artifactAdmission.acquire(
            context, "Import project memory"));
        static_cast<void>(artifactAdmission);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        auto documentResult = implementation_->artifactStore->read(
            request.projectId,
            request.artifact,
            implementation_->options.limits.maximumArtifactBytes,
            context);
        if (!documentResult) {
            auto readError = std::move(documentResult).error();
            if (!request.preview &&
                readError.code == Domain::ErrorCodes::PayloadTooLarge) {
                static_cast<void>(take(
                    implementation_->artifactStore->quarantineOversized(
                        request.projectId,
                        request.artifact,
                        implementation_->options.limits.maximumArtifactBytes,
                        context)));
            }
            fail(std::move(readError));
        }
        auto document = take(std::move(documentResult));
        take(Domain::validateImportProjectMemoryRequest(
            request,
            document.content.size(),
            implementation_->options.limits));
        PreparedWriteConsumer discard = [](PreparedWrite) {};
        ArtifactFailureProvenance validationFailureProvenance =
            ArtifactFailureProvenance::ArtifactValidation;
        auto validationResult = guarded<ParsedArtifactSummary>([&]() {
            return parseArtifact(
                document.content,
                request.projectId,
                request.allowCrossProjectMerge,
                implementation_->options.limits,
                *implementation_->redactor,
                *implementation_->hasher,
                std::move(discard),
                true,
                validationFailureProvenance,
                context);
        });
        if (!validationResult) {
            auto validationError = std::move(validationResult).error();
            if (!request.preview &&
                validationFailureProvenance ==
                    ArtifactFailureProvenance::ArtifactValidation) {
                // Only failures proven to originate while validating the retained
                // artifact may move that artifact into quarantine.
                static_cast<void>(take(
                    implementation_->artifactStore->quarantineCorrupt(
                        request.projectId, document, context)));
            }
            fail(std::move(validationError));
        }
        auto validated = take(std::move(validationResult));
        if (request.preview) {
            return Domain::ProjectMemoryImport{
                request.projectId,
                Domain::ImportDisposition::Preview,
                validated.recordCount,
                validated.recordCount,
                std::move(validated.checksum),
                {}};
        }

        auto importResult = runOnStore<Domain::ProjectMemoryImport>(
            *store,
            "Import project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ProjectMemoryImport>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    std::vector<Domain::MemoryWriteOutcome> imported;
                    imported.reserve(
                        implementation_->options.limits.maximumArtifactRecords);
                    PreparedWriteConsumer consume = [&](PreparedWrite prepared) {
                        requireOperationActive(
                            context, "import a project-memory artifact record");
                        imported.push_back(rememberPrepared(
                            transaction,
                            request.projectId,
                            std::move(prepared),
                            *implementation_->hasher,
                            *implementation_->uuidGenerator,
                            *implementation_->clock,
                            implementation_->options.limits,
                            context));
                    };
                    // The retained bytes passed a complete non-mutating validation
                    // pass before the transaction began. Parse them again while
                    // streaming records into the transaction so database failures
                    // retain their provenance and cannot quarantine a valid artifact.
                    ArtifactFailureProvenance ingestionFailureProvenance =
                        ArtifactFailureProvenance::ArtifactValidation;
                    auto parsed = parseArtifact(
                        document.content,
                        request.projectId,
                        request.allowCrossProjectMerge,
                        implementation_->options.limits,
                        *implementation_->redactor,
                        *implementation_->hasher,
                        std::move(consume),
                        false,
                        ingestionFailureProvenance,
                        context);
                    if (parsed.sourceProjectId != validated.sourceProjectId ||
                        parsed.checksum != validated.checksum ||
                        parsed.recordCount != validated.recordCount) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The validated project-memory artifact changed during import."));
                    }
                    enforceEventJournalRetention(transaction);
                    take(transaction.commit());
                    return Domain::ProjectMemoryImport{
                        request.projectId,
                        Domain::ImportDisposition::Imported,
                        parsed.recordCount,
                        parsed.recordCount,
                        std::move(parsed.checksum),
                        std::move(imported)};
                });
            });
        if (!importResult) {
            auto importError = std::move(importResult).error();
            // The retained artifact passed the complete validation pass above.
            // Failures here originate from transactional ingestion or its
            // dependencies, so the valid artifact must remain available.
            fail(std::move(importError));
        }
        return take(std::move(importResult));
    });
}

Domain::Result<Domain::ProjectMemoryStatus> WindowsProjectMemoryRepository::status(
    const Domain::ProjectMemoryStatusRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ProjectMemoryStatus>([&]() {
        requireScope(projectId(), request.projectId);
        take(Domain::validateProjectMemoryStatusRequest(request));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        auto statusResult = runOnStore<Domain::ProjectMemoryStatus>(
            *store,
            "Inspect project memory status",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ProjectMemoryStatus>([&]() {
                    const auto active = scalarInteger(
                        connection,
                        "SELECT COUNT(*) FROM memory_records "
                        "WHERE project_id=? AND is_tombstone=0",
                        request.projectId,
                        context);
                    const auto tombstones = scalarInteger(
                        connection,
                        "SELECT COUNT(*) FROM memory_records "
                        "WHERE project_id=? AND is_tombstone=1",
                        request.projectId,
                        context);
                    const auto events = scalarInteger(
                        connection,
                        "SELECT COUNT(*) FROM event_journal WHERE project_id=?",
                        request.projectId,
                        context);
                    const auto maximumSize = static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)());
                    if (static_cast<std::uint64_t>(active) > maximumSize ||
                        static_cast<std::uint64_t>(tombstones) > maximumSize ||
                        static_cast<std::uint64_t>(events) > maximumSize) {
                        fail(integrityError("Project memory status counts exceed addressable range."));
                    }

                    bool ftsAvailable{};
                    {
                        auto statement = take(connection.prepare(
                            "SELECT COUNT(*) FROM sqlite_master WHERE "
                            "(type='table' AND name='memory_records_fts') OR "
                            "(type='trigger' AND name IN("
                            "'memory_fts_insert','memory_fts_update','memory_fts_delete'))",
                            context));
                        if (take(statement.step()) == WinsqliteStepResult::Row) {
                            ftsAvailable = take(statement.columnInt64(0)) == 4;
                        }
                    }
                    bool integrityOk{};
                    {
                        auto statement = take(connection.prepare(
                            "PRAGMA main.quick_check(1);", context));
                        if (take(statement.step()) == WinsqliteStepResult::Row) {
                            const auto result = optionalText(statement, 0, 64U);
                            integrityOk = result.has_value() && *result == "ok";
                        }
                    }
                    Domain::ProjectMemoryStatus value{request.projectId};
                    value.recordCount = static_cast<std::size_t>(active);
                    value.tombstoneCount = static_cast<std::size_t>(tombstones);
                    value.eventCount = static_cast<std::size_t>(events);
                    value.fullTextSearchAvailable = ftsAvailable;
                    value.integrityOk = integrityOk;
                    value.limits = implementation_->options.limits;
                    return value;
                });
            });
        auto value = take(std::move(statusResult));
        const auto sizes = take(store->databaseFileSizes(context));
        value.databaseBytes = sizes.first;
        value.writeAheadLogBytes = sizes.second;
        return value;
    });
}

Domain::Result<void> WindowsProjectMemoryRepository::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->database) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The project memory repository is closed."));
    }
    return implementation_->database->quickCheck(context);
}

Domain::Result<Domain::ResetReport> WindowsProjectMemoryRepository::resetMemory(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ResetReport>([&]() {
        const std::string expectedToken =
            "RESET PROJECT MEMORY " + projectId().value();
        take(Domain::validateDestructiveConfirmation(
            confirmation,
            "reset_project_memory",
            projectId().value(),
            expectedToken));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project memory repository is closed."));
        }
        return take(runOnStore<Domain::ResetReport>(
            *store,
            "Reset project memory",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ResetReport>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    const auto records = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM memory_records WHERE project_id=?",
                        projectId());
                    const auto links = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM memory_links WHERE project_id=?",
                        projectId());
                    const auto events = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM event_journal WHERE project_id=?",
                        projectId());
                    for (const std::string_view sql : {
                             "DELETE FROM memory_links WHERE project_id=?",
                             "DELETE FROM memory_records WHERE project_id=?",
                             "DELETE FROM event_journal WHERE project_id=?"}) {
                        auto statement = take(transaction.prepare(sql));
                        take(statement.bindText(1, projectId().value()));
                        stepDone(statement);
                    }
                    take(transaction.execute(
                        "DELETE FROM memory_tags WHERE NOT EXISTS("
                        "SELECT 1 FROM memory_record_tags rt WHERE rt.tag_id=memory_tags.id)"));
                    const auto timestamp = timestampText(implementation_->clock->utcNow());
                    appendEvent(
                        transaction,
                        projectId(),
                        std::nullopt,
                        "reset",
                        "records=" + std::to_string(records) +
                            ";links=" + std::to_string(links) +
                            ";events=" + std::to_string(events),
                        timestamp);
                    enforceEventJournalRetention(transaction);
                    const bool verified =
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM memory_records WHERE project_id=?",
                            projectId()) == 0 &&
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM memory_links WHERE project_id=?",
                            projectId()) == 0 &&
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM event_journal WHERE project_id=?",
                            projectId()) == 1;
                    if (!verified) {
                        fail(integrityError(
                            "The transactional project memory reset could not be verified."));
                    }
                    const auto maximumSize = static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)());
                    if (static_cast<std::uint64_t>(records) > maximumSize ||
                        static_cast<std::uint64_t>(links) > maximumSize ||
                        static_cast<std::uint64_t>(events) > maximumSize) {
                        fail(integrityError("Project memory reset counts exceed addressable range."));
                    }
                    Domain::ResetReport report{
                        confirmation.action,
                        confirmation.scope,
                        1U,
                        static_cast<std::size_t>(records),
                        static_cast<std::size_t>(links),
                        static_cast<std::size_t>(events),
                        true};
                    take(transaction.commit());
                    return report;
                });
            }));
    });
}

Domain::Result<Domain::ContinuityOperation>
WindowsProjectMemoryRepository::createOperation(
    const Domain::ContinuityHandoff& handoffValue,
    const Domain::IdempotencyKey& idempotencyKey,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ContinuityOperation>([&]() {
        requireScope(projectId(), handoffValue.project.projectId);
        auto document = take(implementation_->continuityCodec->encode(
            handoffValue, context));
        const auto& handoff = document.handoff;
        requireScope(projectId(), handoff.project.projectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<Domain::ContinuityOperation>(
            *store,
            "Create continuity operation",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ContinuityOperation>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    if (auto existing = continuityOperationByIdempotencyKey(
                            transaction,
                            projectId(),
                            idempotencyKey,
                            *implementation_->hasher,
                            context)) {
                        if (existing->operationId != handoff.operationId ||
                            existing->handoffId != handoff.handoffId ||
                            existing->predecessorSessionId !=
                                handoff.predecessorSession.sessionId ||
                            existing->adapterId != handoff.hostState.adapterId) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "The continuity idempotency key is bound to another operation."));
                        }
                        take(transaction.commit());
                        return *existing;
                    }
                    if (const auto active = activeContinuityOperation(
                            transaction,
                            projectId(),
                            *implementation_->hasher,
                            context)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "Another continuity operation is active for this project: " +
                                active->operationId.value() + '.'));
                    }
                    const auto now = implementation_->clock->utcNow();
                    const auto timestamp = timestampText(now);
                    const auto checksum = continuityChecksum(
                        handoff.operationId,
                        Domain::wireName(Domain::ContinuityState::Idle),
                        std::nullopt,
                        handoff.handoffId,
                        0U,
                        *implementation_->hasher);
                    auto insert = take(transaction.prepare(
                        "INSERT INTO rollover_operations("
                        "operation_id,project_id,predecessor_session_id,"
                        "successor_session_id,handoff_id,state,attempt,adapter_id,"
                        "idempotency_key,acknowledged_session_id,"
                        "acknowledged_handoff_id,created_at,updated_at,last_error,"
                        "retry_at,state_checksum,retry_resume_state) "
                        "VALUES(?,?,?,NULL,?,?,0,?,?,NULL,NULL,?,?,NULL,NULL,?,NULL)"));
                    take(insert.bindText(1, handoff.operationId.value()));
                    take(insert.bindText(2, projectId().value()));
                    take(insert.bindText(
                        3, handoff.predecessorSession.sessionId.value()));
                    take(insert.bindText(4, handoff.handoffId.value()));
                    take(insert.bindText(
                        5, Domain::wireName(Domain::ContinuityState::Idle)));
                    take(insert.bindText(6, handoff.hostState.adapterId.value()));
                    take(insert.bindText(7, idempotencyKey.value()));
                    take(insert.bindText(8, timestamp));
                    take(insert.bindText(9, timestamp));
                    take(insert.bindText(10, checksum.value()));
                    stepDone(insert);
                    auto handoffInsert = take(transaction.prepare(
                        "INSERT INTO continuity_handoffs("
                        "handoff_id,project_id,operation_id,payload_json,"
                        "content_sha256,created_at,acknowledged_session_id,"
                        "acknowledged_at) VALUES(?,?,?,?,?,?,NULL,NULL)"));
                    take(handoffInsert.bindText(1, handoff.handoffId.value()));
                    take(handoffInsert.bindText(2, projectId().value()));
                    take(handoffInsert.bindText(3, handoff.operationId.value()));
                    take(handoffInsert.bindText(4, document.canonicalUtf8));
                    take(handoffInsert.bindText(
                        5, handoff.contentSha256.value()));
                    take(handoffInsert.bindText(
                        6, timestampText(handoff.createdAt)));
                    stepDone(handoffInsert);
                    Domain::ContinuityOperation created{
                        handoff.operationId,
                        projectId(),
                        handoff.predecessorSession.sessionId,
                        std::nullopt,
                        handoff.handoffId,
                        Domain::ContinuityState::Idle,
                        0U,
                        handoff.hostState.adapterId,
                        idempotencyKey,
                        std::nullopt,
                        std::nullopt,
                        now,
                        now,
                        std::nullopt,
                        std::nullopt,
                        checksum,
                        std::nullopt};
                    appendContinuityTransition(
                        transaction,
                        created,
                        std::nullopt,
                        Domain::ContinuityState::Idle,
                        0U,
                        std::optional<std::string>{"operation_created"},
                        checksum,
                        timestamp);
                    auto persisted = continuityOperationById(
                        transaction,
                        projectId(),
                        handoff.operationId,
                        *implementation_->hasher,
                        context);
                    if (!persisted) {
                        fail(integrityError(
                            "The created continuity operation is unreadable."));
                    }
                    const auto persistedHandoff = continuityHandoffById(
                        transaction,
                        projectId(),
                        handoff.handoffId,
                        *implementation_->continuityCodec,
                        context);
                    if (!persistedHandoff ||
                        persistedHandoff->contentSha256 !=
                            handoff.contentSha256) {
                        fail(integrityError(
                            "The operation's canonical handoff is unreadable."));
                    }
                    take(transaction.commit());
                    return *persisted;
                });
            }));
    });
}

Domain::Result<void> WindowsProjectMemoryRepository::storeHandoff(
    const Domain::ContinuityHandoff& handoffValue,
    const Domain::OperationContext& context) noexcept
{
    return guarded<void>([&]() {
        requireScope(projectId(), handoffValue.project.projectId);
        auto document = take(implementation_->continuityCodec->encode(
            handoffValue, context));
        const auto& handoff = document.handoff;
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        take(runOnStore<void>(
            *store,
            "Store continuity handoff",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<void>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    const auto operation = continuityOperationById(
                        transaction,
                        projectId(),
                        handoff.operationId,
                        *implementation_->hasher,
                        context);
                    if (!operation) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The handoff continuity operation was not found."));
                    }
                    if (operation->handoffId != handoff.handoffId ||
                        operation->predecessorSessionId !=
                            handoff.predecessorSession.sessionId ||
                        operation->adapterId != handoff.hostState.adapterId ||
                        (operation->state != Domain::ContinuityState::Idle &&
                         operation->state !=
                             Domain::ContinuityState::CheckpointPreparing)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The handoff does not match checkpoint preparation."));
                    }
                    if (const auto existing = continuityHandoffById(
                            transaction,
                            projectId(),
                            handoff.handoffId,
                            *implementation_->continuityCodec,
                            context)) {
                        if (existing->contentSha256 != handoff.contentSha256) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "A different canonical handoff is already bound to this operation."));
                        }
                        take(transaction.commit());
                        return;
                    }
                    auto write = take(transaction.prepare(
                        "INSERT INTO continuity_handoffs("
                        "handoff_id,project_id,operation_id,payload_json,"
                        "content_sha256,created_at,acknowledged_session_id,"
                        "acknowledged_at) VALUES(?,?,?,?,?,?,NULL,NULL) "
                        "ON CONFLICT(handoff_id) DO UPDATE SET "
                        "payload_json=excluded.payload_json,"
                        "content_sha256=excluded.content_sha256 "
                        "WHERE continuity_handoffs.project_id=excluded.project_id "
                        "AND continuity_handoffs.operation_id=excluded.operation_id "
                        "AND continuity_handoffs.acknowledged_session_id IS NULL"));
                    take(write.bindText(1, handoff.handoffId.value()));
                    take(write.bindText(2, projectId().value()));
                    take(write.bindText(3, handoff.operationId.value()));
                    take(write.bindText(4, document.canonicalUtf8));
                    take(write.bindText(5, handoff.contentSha256.value()));
                    take(write.bindText(6, timestampText(handoff.createdAt)));
                    stepDone(write);
                    if (changedRows(transaction) != 1) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The continuity handoff is already bound or acknowledged."));
                    }
                    const auto persisted = continuityHandoffById(
                        transaction,
                        projectId(),
                        handoff.handoffId,
                        *implementation_->continuityCodec,
                        context);
                    if (!persisted ||
                        persisted->contentSha256 != handoff.contentSha256) {
                        fail(integrityError(
                            "The stored continuity handoff could not be verified."));
                    }
                    take(transaction.commit());
                    return;
                });
            }));
        return;
    });
}

Domain::Result<std::optional<Domain::ContinuityHandoff>>
WindowsProjectMemoryRepository::handoff(
    const Domain::ProjectId& requestedProjectId,
    const Domain::ContinuityHandoffId& handoffId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::ContinuityHandoff>>([&]() {
        requireScope(projectId(), requestedProjectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<std::optional<Domain::ContinuityHandoff>>(
            *store,
            "Read continuity handoff",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::ContinuityHandoff>>([&]() {
                    return continuityHandoffById(
                        connection,
                        projectId(),
                        handoffId,
                        *implementation_->continuityCodec,
                        context);
                });
            }));
    });
}

Domain::Result<std::optional<Domain::ContinuityOperation>>
WindowsProjectMemoryRepository::operation(
    const Domain::ProjectId& requestedProjectId,
    const Domain::ContinuityOperationId& operationId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::ContinuityOperation>>([&]() {
        requireScope(projectId(), requestedProjectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<std::optional<Domain::ContinuityOperation>>(
            *store,
            "Read continuity operation",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::ContinuityOperation>>([&]() {
                    return continuityOperationById(
                        connection,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                });
            }));
    });
}

Domain::Result<std::optional<Domain::ContinuityOperation>>
WindowsProjectMemoryRepository::activeOperation(
    const Domain::ProjectId& requestedProjectId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::ContinuityOperation>>([&]() {
        requireScope(projectId(), requestedProjectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<std::optional<Domain::ContinuityOperation>>(
            *store,
            "Read active continuity operation",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::ContinuityOperation>>([&]() {
                    return activeContinuityOperation(
                        connection,
                        projectId(),
                        *implementation_->hasher,
                        context);
                });
            }));
    });
}

Domain::Result<Domain::ContinuityOperation>
WindowsProjectMemoryRepository::compareAndSet(
    const Domain::ContinuityOperationId& operationId,
    const Domain::ContinuityState expected,
    const Domain::ContinuityState next,
    std::optional<Domain::SessionId> successorSessionId,
    std::optional<std::string> evidence,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ContinuityOperation>([&]() {
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<Domain::ContinuityOperation>(
            *store,
            "Compare and set continuity state",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ContinuityOperation>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    auto current = continuityOperationById(
                        transaction,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                    if (!current) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The continuity operation was not found."));
                    }
                    if (current->state == next) {
                        if (successorSessionId && current->successorSessionId !=
                                successorSessionId) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "The completed continuity transition has another successor."));
                        }
                        take(transaction.commit());
                        return *current;
                    }
                    if (current->state != expected) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The continuity compare-and-set expected another state."));
                    }
                    if (!Domain::isAllowedContinuityTransition(expected, next)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::InvalidRequest,
                            "The requested continuity transition is invalid."));
                    }
                    if (expected == Domain::ContinuityState::RetryWait &&
                        current->retryResumeState != next) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The retry operation is not bound to the requested resume state."));
                    }
                    auto successor = successorSessionId
                        ? std::move(successorSessionId)
                        : current->successorSessionId;
                    if (next == Domain::ContinuityState::SuccessorCreated &&
                        !successor) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::InvalidRequest,
                            "A successor session is required for successor-created state."));
                    }
                    if (next == Domain::ContinuityState::CheckpointPersisted &&
                        !continuityHandoffById(
                            transaction,
                            projectId(),
                            current->handoffId,
                            *implementation_->continuityCodec,
                            context)) {
                        fail(integrityError(
                            "Checkpoint persistence requires a durable canonical handoff."));
                    }
                    if ((next == Domain::ContinuityState::PredecessorSealing ||
                         next == Domain::ContinuityState::Completed) &&
                        (!current->acknowledgedSessionId ||
                         !current->acknowledgedHandoffId ||
                         !successor ||
                         current->acknowledgedSessionId != successor ||
                         *current->acknowledgedHandoffId != current->handoffId)) {
                        fail(integrityError(
                            "Predecessor sealing requires an exact durable acknowledgement."));
                    }
                    if (current->attempt >=
                        MaximumContinuityTransitionsPerOperation - 1U) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The continuity transition attempt bound was reached."));
                    }
                    const auto attempt = current->attempt + 1U;
                    const auto timestamp = timestampText(
                        implementation_->clock->utcNow());
                    const auto checksum = continuityChecksum(
                        operationId,
                        Domain::wireName(next),
                        successor,
                        current->handoffId,
                        attempt,
                        *implementation_->hasher);
                    auto update = take(transaction.prepare(
                        "UPDATE rollover_operations SET state=?,attempt=?,"
                        "successor_session_id=?,updated_at=?,last_error=NULL,"
                        "retry_at=NULL,retry_resume_state=NULL,state_checksum=? "
                        "WHERE operation_id=? AND project_id=? AND state_checksum=?"));
                    take(update.bindText(1, Domain::wireName(next)));
                    take(update.bindInt64(2, static_cast<std::int64_t>(attempt)));
                    if (successor) {
                        take(update.bindText(3, successor->value()));
                    } else {
                        take(update.bindNull(3));
                    }
                    take(update.bindText(4, timestamp));
                    take(update.bindText(5, checksum.value()));
                    take(update.bindText(6, operationId.value()));
                    take(update.bindText(7, projectId().value()));
                    take(update.bindText(8, current->stateChecksum.value()));
                    stepDone(update);
                    if (changedRows(transaction) != 1) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The continuity compare-and-set lost its durable state race."));
                    }
                    if (next == Domain::ContinuityState::SuccessorCreated) {
                        auto durableHandoff = continuityHandoffById(
                            transaction,
                            projectId(),
                            current->handoffId,
                            *implementation_->continuityCodec,
                            context);
                        if (!durableHandoff || !successor) {
                            fail(integrityError(
                                "The successor transition has no durable handoff."));
                        }
                        durableHandoff->successorSession = Domain::ContinuitySession{
                            *successor,
                            std::nullopt,
                            std::nullopt,
                            std::nullopt};
                        auto document = take(implementation_->continuityCodec->encode(
                            *durableHandoff, context));
                        auto handoffUpdate = take(transaction.prepare(
                            "UPDATE continuity_handoffs SET payload_json=?,"
                            "content_sha256=? WHERE project_id=? AND handoff_id=? "
                            "AND acknowledged_session_id IS NULL"));
                        take(handoffUpdate.bindText(1, document.canonicalUtf8));
                        take(handoffUpdate.bindText(
                            2, document.handoff.contentSha256.value()));
                        take(handoffUpdate.bindText(3, projectId().value()));
                        take(handoffUpdate.bindText(4, current->handoffId.value()));
                        stepDone(handoffUpdate);
                        if (changedRows(transaction) != 1) {
                            fail(integrityError(
                                "The successor could not be bound to the canonical handoff."));
                        }
                    }
                    Domain::ContinuityOperation transition = *current;
                    transition.successorSessionId = successor;
                    appendContinuityTransition(
                        transaction,
                        transition,
                        expected,
                        next,
                        attempt,
                        evidence,
                        checksum,
                        timestamp);
                    if (next == Domain::ContinuityState::Completed && successor) {
                        auto pointer = take(transaction.prepare(
                            "INSERT INTO project_active_sessions("
                            "project_id,session_id,updated_at) VALUES(?,?,?) "
                            "ON CONFLICT(project_id) DO UPDATE SET "
                            "session_id=excluded.session_id,updated_at=excluded.updated_at"));
                        take(pointer.bindText(1, projectId().value()));
                        take(pointer.bindText(2, successor->value()));
                        take(pointer.bindText(3, timestamp));
                        stepDone(pointer);
                    }
                    auto persisted = continuityOperationById(
                        transaction,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                    if (!persisted || persisted->state != next) {
                        fail(integrityError(
                            "The continuity transition result is unreadable."));
                    }
                    take(transaction.commit());
                    return *persisted;
                });
            }));
    });
}

Domain::Result<Domain::ContinuityOperation>
WindowsProjectMemoryRepository::acknowledge(
    const Domain::ContinuityOperationId& operationId,
    const Domain::HandoffAcknowledgement& acknowledgement,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ContinuityOperation>([&]() {
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<Domain::ContinuityOperation>(
            *store,
            "Acknowledge continuity handoff",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ContinuityOperation>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    auto current = continuityOperationById(
                        transaction,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                    if (!current) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The continuity operation was not found."));
                    }
                    const auto durableHandoff = continuityHandoffById(
                        transaction,
                        projectId(),
                        current->handoffId,
                        *implementation_->continuityCodec,
                        context);
                    if (!durableHandoff) {
                        fail(integrityError(
                            "The continuity acknowledgement has no durable handoff."));
                    }
                    take(Domain::validateHandoffAcknowledgement(
                        *current, *durableHandoff, acknowledgement));
                    if (current->state == Domain::ContinuityState::Acknowledged ||
                        current->state == Domain::ContinuityState::PredecessorSealing ||
                        current->state == Domain::ContinuityState::Completed) {
                        if (current->acknowledgedSessionId !=
                                acknowledgement.successorSessionId ||
                            current->acknowledgedHandoffId !=
                                acknowledgement.handoffId) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "Another continuity acknowledgement is already durable."));
                        }
                        take(transaction.commit());
                        return *current;
                    }
                    if (current->state != Domain::ContinuityState::BootstrapSending) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The continuity operation is not awaiting acknowledgement."));
                    }
                    if (current->attempt >=
                        MaximumContinuityTransitionsPerOperation - 1U) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The continuity transition attempt bound was reached."));
                    }
                    const auto attempt = current->attempt + 1U;
                    const auto timestamp = timestampText(
                        implementation_->clock->utcNow());
                    const auto checksum = continuityChecksum(
                        operationId,
                        Domain::wireName(Domain::ContinuityState::Acknowledged),
                        current->successorSessionId,
                        current->handoffId,
                        attempt,
                        *implementation_->hasher);
                    auto update = take(transaction.prepare(
                        "UPDATE rollover_operations SET state=?,attempt=?,"
                        "acknowledged_session_id=?,acknowledged_handoff_id=?,"
                        "updated_at=?,last_error=NULL,retry_at=NULL,"
                        "retry_resume_state=NULL,state_checksum=? "
                        "WHERE operation_id=? AND project_id=? AND state_checksum=?"));
                    take(update.bindText(
                        1, Domain::wireName(Domain::ContinuityState::Acknowledged)));
                    take(update.bindInt64(2, static_cast<std::int64_t>(attempt)));
                    take(update.bindText(
                        3, acknowledgement.successorSessionId.value()));
                    take(update.bindText(4, acknowledgement.handoffId.value()));
                    take(update.bindText(5, timestamp));
                    take(update.bindText(6, checksum.value()));
                    take(update.bindText(7, operationId.value()));
                    take(update.bindText(8, projectId().value()));
                    take(update.bindText(9, current->stateChecksum.value()));
                    stepDone(update);
                    if (changedRows(transaction) != 1) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The acknowledgement lost its durable state race."));
                    }
                    auto handoffUpdate = take(transaction.prepare(
                        "UPDATE continuity_handoffs SET acknowledged_session_id=?,"
                        "acknowledged_at=? WHERE project_id=? AND handoff_id=? "
                        "AND (acknowledged_session_id IS NULL OR "
                        "acknowledged_session_id=?)"));
                    take(handoffUpdate.bindText(
                        1, acknowledgement.successorSessionId.value()));
                    take(handoffUpdate.bindText(2, timestamp));
                    take(handoffUpdate.bindText(3, projectId().value()));
                    take(handoffUpdate.bindText(4, acknowledgement.handoffId.value()));
                    take(handoffUpdate.bindText(
                        5, acknowledgement.successorSessionId.value()));
                    stepDone(handoffUpdate);
                    if (changedRows(transaction) != 1) {
                        fail(integrityError(
                            "The acknowledged handoff row could not be updated exactly."));
                    }
                    appendContinuityTransition(
                        transaction,
                        *current,
                        Domain::ContinuityState::BootstrapSending,
                        Domain::ContinuityState::Acknowledged,
                        attempt,
                        std::optional<std::string>{"exact_acknowledgement"},
                        checksum,
                        timestamp);
                    auto persisted = continuityOperationById(
                        transaction,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                    if (!persisted ||
                        persisted->state != Domain::ContinuityState::Acknowledged) {
                        fail(integrityError(
                            "The continuity acknowledgement result is unreadable."));
                    }
                    take(transaction.commit());
                    return *persisted;
                });
            }));
    });
}

Domain::Result<Domain::ContinuityOperation>
WindowsProjectMemoryRepository::recordRetry(
    const Domain::ContinuityOperationId& operationId,
    const Domain::ContinuityState resumeState,
    std::string error,
    std::optional<Domain::UtcTimePoint> retryAt,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ContinuityOperation>([&]() {
        if (!Domain::isRetryResumeState(resumeState) || error.empty()) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A continuity retry requires a resumable state and an error."));
        }
        error.resize((std::min)(error.size(), MaximumContinuityErrorBytes));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<Domain::ContinuityOperation>(
            *store,
            "Record continuity retry",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ContinuityOperation>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    auto current = continuityOperationById(
                        transaction,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                    if (!current) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The continuity operation was not found."));
                    }
                    const auto now = implementation_->clock->utcNow();
                    const auto effectiveRetryAt = retryAt.value_or(
                        now + std::chrono::seconds{1});
                    if (effectiveRetryAt < now) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::InvalidRequest,
                            "The continuity retry time must not precede the current time."));
                    }
                    const auto timestamp = timestampText(now);
                    const auto retryTimestamp = timestampText(effectiveRetryAt);
                    if (current->state == Domain::ContinuityState::RetryWait) {
                        if (current->retryResumeState != resumeState) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "The durable retry is bound to another resume state."));
                        }
                        auto refresh = take(transaction.prepare(
                            "UPDATE rollover_operations SET last_error=?,retry_at=?,"
                            "updated_at=? WHERE operation_id=? AND project_id=? "
                            "AND state_checksum=?"));
                        take(refresh.bindText(1, error));
                        take(refresh.bindText(2, retryTimestamp));
                        take(refresh.bindText(3, timestamp));
                        take(refresh.bindText(4, operationId.value()));
                        take(refresh.bindText(5, projectId().value()));
                        take(refresh.bindText(6, current->stateChecksum.value()));
                        stepDone(refresh);
                        if (changedRows(transaction) != 1) {
                            fail(Domain::makeError(
                                Domain::ErrorCodes::Conflict,
                                "The retry refresh lost its durable state race."));
                        }
                        auto persisted = continuityOperationById(
                            transaction,
                            projectId(),
                            operationId,
                            *implementation_->hasher,
                            context);
                        if (!persisted) {
                            fail(integrityError(
                                "The refreshed retry operation is unreadable."));
                        }
                        take(transaction.commit());
                        return *persisted;
                    }
                    if (current->state != resumeState ||
                        !Domain::isAllowedContinuityTransition(
                            current->state,
                            Domain::ContinuityState::FailedRecoverable) ||
                        current->attempt >
                            MaximumContinuityTransitionsPerOperation - 3U) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The continuity operation cannot enter retry wait from its current state."));
                    }
                    const auto failedAttempt = current->attempt + 1U;
                    const auto failedChecksum = continuityChecksum(
                        operationId,
                        Domain::wireName(
                            Domain::ContinuityState::FailedRecoverable),
                        current->successorSessionId,
                        current->handoffId,
                        failedAttempt,
                        *implementation_->hasher);
                    auto failUpdate = take(transaction.prepare(
                        "UPDATE rollover_operations SET state=?,attempt=?,"
                        "updated_at=?,last_error=?,retry_at=NULL,"
                        "retry_resume_state=NULL,state_checksum=? "
                        "WHERE operation_id=? AND project_id=? AND state_checksum=?"));
                    take(failUpdate.bindText(
                        1,
                        Domain::wireName(
                            Domain::ContinuityState::FailedRecoverable)));
                    take(failUpdate.bindInt64(
                        2, static_cast<std::int64_t>(failedAttempt)));
                    take(failUpdate.bindText(3, timestamp));
                    take(failUpdate.bindText(4, error));
                    take(failUpdate.bindText(5, failedChecksum.value()));
                    take(failUpdate.bindText(6, operationId.value()));
                    take(failUpdate.bindText(7, projectId().value()));
                    take(failUpdate.bindText(8, current->stateChecksum.value()));
                    stepDone(failUpdate);
                    if (changedRows(transaction) != 1) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The recoverable-failure transition lost its durable state race."));
                    }
                    appendContinuityTransition(
                        transaction,
                        *current,
                        current->state,
                        Domain::ContinuityState::FailedRecoverable,
                        failedAttempt,
                        std::optional<std::string>{error},
                        failedChecksum,
                        timestamp);
                    const auto retryAttempt = failedAttempt + 1U;
                    const auto retryChecksum = continuityChecksum(
                        operationId,
                        Domain::wireName(Domain::ContinuityState::RetryWait),
                        current->successorSessionId,
                        current->handoffId,
                        retryAttempt,
                        *implementation_->hasher);
                    auto retryUpdate = take(transaction.prepare(
                        "UPDATE rollover_operations SET state=?,attempt=?,"
                        "updated_at=?,last_error=?,retry_at=?,retry_resume_state=?,"
                        "state_checksum=? WHERE operation_id=? AND project_id=? "
                        "AND state_checksum=?"));
                    take(retryUpdate.bindText(
                        1, Domain::wireName(Domain::ContinuityState::RetryWait)));
                    take(retryUpdate.bindInt64(
                        2, static_cast<std::int64_t>(retryAttempt)));
                    take(retryUpdate.bindText(3, timestamp));
                    take(retryUpdate.bindText(4, error));
                    take(retryUpdate.bindText(5, retryTimestamp));
                    take(retryUpdate.bindText(6, Domain::wireName(resumeState)));
                    take(retryUpdate.bindText(7, retryChecksum.value()));
                    take(retryUpdate.bindText(8, operationId.value()));
                    take(retryUpdate.bindText(9, projectId().value()));
                    take(retryUpdate.bindText(10, failedChecksum.value()));
                    stepDone(retryUpdate);
                    if (changedRows(transaction) != 1) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The retry-wait transition lost its durable state race."));
                    }
                    Domain::ContinuityOperation failed = *current;
                    failed.state = Domain::ContinuityState::FailedRecoverable;
                    failed.attempt = failedAttempt;
                    failed.stateChecksum = failedChecksum;
                    appendContinuityTransition(
                        transaction,
                        failed,
                        Domain::ContinuityState::FailedRecoverable,
                        Domain::ContinuityState::RetryWait,
                        retryAttempt,
                        std::optional<std::string>{"retry_scheduled"},
                        retryChecksum,
                        timestamp);
                    auto persisted = continuityOperationById(
                        transaction,
                        projectId(),
                        operationId,
                        *implementation_->hasher,
                        context);
                    if (!persisted ||
                        persisted->state != Domain::ContinuityState::RetryWait ||
                        persisted->retryResumeState != resumeState) {
                        fail(integrityError(
                            "The retry-wait operation is unreadable."));
                    }
                    take(transaction.commit());
                    return *persisted;
                });
            }));
    });
}

Domain::Result<std::optional<Domain::SessionId>>
WindowsProjectMemoryRepository::activeSession(
    const Domain::ProjectId& requestedProjectId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::SessionId>>([&]() {
        requireScope(projectId(), requestedProjectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<std::optional<Domain::SessionId>>(
            *store,
            "Read active continuity session",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::SessionId>>([&]() {
                    auto statement = take(connection.prepare(
                        "SELECT session_id FROM project_active_sessions "
                        "WHERE project_id=? LIMIT 1",
                        context));
                    take(statement.bindText(1, projectId().value()));
                    if (take(statement.step()) == WinsqliteStepResult::Done) {
                        return std::optional<Domain::SessionId>{};
                    }
                    return std::optional<Domain::SessionId>{
                        parseStrongId<Domain::SessionId>(
                            requiredText(
                                statement,
                                0,
                                MaximumContinuityIdentifierBytes,
                                "active continuity session id"),
                            "active continuity session id")};
                });
            }));
    });
}

Domain::Result<std::size_t> WindowsProjectMemoryRepository::transitionCount(
    const Domain::ContinuityOperationId& operationId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::size_t>([&]() {
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<std::size_t>(
            *store,
            "Count continuity transitions",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::size_t>([&]() {
                    if (!continuityOperationById(
                            connection,
                            projectId(),
                            operationId,
                            *implementation_->hasher,
                            context)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The continuity transition operation was not found."));
                    }
                    auto statement = take(connection.prepare(
                        "SELECT COUNT(*) FROM rollover_transitions "
                        "WHERE project_id=? AND operation_id=?",
                        context));
                    take(statement.bindText(1, projectId().value()));
                    take(statement.bindText(2, operationId.value()));
                    if (take(statement.step()) != WinsqliteStepResult::Row) {
                        fail(integrityError(
                            "The continuity transition count returned no row."));
                    }
                    const auto count = take(statement.columnInt64(0));
                    if (count < 0 ||
                        static_cast<std::uint64_t>(count) >
                            static_cast<std::uint64_t>(
                                (std::numeric_limits<std::size_t>::max)())) {
                        fail(integrityError(
                            "The continuity transition count is outside range."));
                    }
                    return static_cast<std::size_t>(count);
                });
            }));
    });
}

Domain::Result<Domain::ContinuityStatus>
WindowsProjectMemoryRepository::status(
    const Domain::ProjectId& requestedProjectId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ContinuityStatus>([&]() {
        requireScope(projectId(), requestedProjectId);
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<Domain::ContinuityStatus>(
            *store,
            "Inspect continuity status",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ContinuityStatus>([&]() {
                    validateAllContinuityOperations(
                        connection,
                        projectId(),
                        *implementation_->hasher,
                        context);
                    const auto operationCount = scalarInteger(
                        connection,
                        "SELECT COUNT(*) FROM rollover_operations WHERE project_id=?",
                        projectId(),
                        context);
                    const auto handoffCount = scalarInteger(
                        connection,
                        "SELECT COUNT(*) FROM continuity_handoffs WHERE project_id=?",
                        projectId(),
                        context);
                    const auto maximum = static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)());
                    if (static_cast<std::uint64_t>(operationCount) > maximum ||
                        static_cast<std::uint64_t>(handoffCount) > maximum) {
                        fail(integrityError(
                            "Continuity status counts exceed addressable range."));
                    }
                    auto active = activeContinuityOperation(
                        connection,
                        projectId(),
                        *implementation_->hasher,
                        context);
                    return Domain::ContinuityStatus{
                        projectId(),
                        active,
                        static_cast<std::size_t>(operationCount),
                        static_cast<std::size_t>(handoffCount),
                        active.has_value()};
                });
            }));
    });
}

Domain::Result<Domain::ContinuityResetReport>
WindowsProjectMemoryRepository::resetContinuity(
    const Domain::ContinuityResetRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::ContinuityResetReport>([&]() {
        requireScope(projectId(), request.projectId);
        take(Domain::validateDestructiveConfirmation(
            request.confirmation,
            "reset_project_continuity",
            projectId().value(),
            "RESET PROJECT CONTINUITY " + projectId().value()));
        auto* store = implementation_->database->repositoryStore();
        if (store == nullptr) {
            fail(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project continuity repository is closed."));
        }
        return take(runOnStore<Domain::ContinuityResetReport>(
            *store,
            "Reset project continuity",
            context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::ContinuityResetReport>([&]() {
                    auto transaction = take(WinsqliteTransaction::beginImmediate(
                        connection, context));
                    const auto handoffs = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM continuity_handoffs WHERE project_id=?",
                        projectId());
                    const auto operations = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM rollover_operations WHERE project_id=?",
                        projectId());
                    const auto transitions = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM rollover_transitions WHERE project_id=?",
                        projectId());
                    const auto activeSessions = scalarInteger(
                        transaction,
                        "SELECT COUNT(*) FROM project_active_sessions WHERE project_id=?",
                        projectId());
                    for (const std::string_view sql : {
                             "DELETE FROM rollover_transitions WHERE project_id=?",
                             "DELETE FROM continuity_handoffs WHERE project_id=?",
                             "DELETE FROM rollover_operations WHERE project_id=?",
                             "DELETE FROM project_active_sessions WHERE project_id=?"}) {
                        auto statement = take(transaction.prepare(sql));
                        take(statement.bindText(1, projectId().value()));
                        stepDone(statement);
                    }
                    const bool verified =
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM continuity_handoffs WHERE project_id=?",
                            projectId()) == 0 &&
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM rollover_operations WHERE project_id=?",
                            projectId()) == 0 &&
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM rollover_transitions WHERE project_id=?",
                            projectId()) == 0 &&
                        scalarInteger(
                            transaction,
                            "SELECT COUNT(*) FROM project_active_sessions WHERE project_id=?",
                            projectId()) == 0;
                    if (!verified) {
                        fail(integrityError(
                            "The transactional continuity reset could not be verified."));
                    }
                    const auto maximum = static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)());
                    for (const auto count : {
                             handoffs, operations, transitions, activeSessions}) {
                        if (static_cast<std::uint64_t>(count) > maximum) {
                            fail(integrityError(
                                "A continuity reset count exceeds addressable range."));
                        }
                    }
                    Domain::ResetReport report{
                        request.confirmation.action,
                        request.confirmation.scope,
                        1U,
                        static_cast<std::size_t>(handoffs),
                        static_cast<std::size_t>(operations),
                        static_cast<std::size_t>(transitions + activeSessions),
                        true};
                    take(transaction.commit());
                    return Domain::ContinuityResetReport{projectId(), std::move(report)};
                });
            }));
    });
}

void WindowsProjectMemoryRepository::close() noexcept
{
    try {
        if (!implementation_ || !implementation_->database || !implementation_->clock) {
            return;
        }
        implementation_->artifactAdmission.beginShutdown();
        if (!implementation_->artifactAdmission.waitUntilIdle(
                std::chrono::seconds{60})) {
            return;
        }
        auto operationId = Domain::OperationId::parse(
            "ffffffff-ffff-4fff-bfff-fffffffffff0");
        auto correlationId = Domain::CorrelationId::parse(
            "project-memory-repository-close");
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
        // close is an idempotent best-effort noexcept boundary. The owned RAII
        // database still performs native release without checkpointing.
    }
}

} // namespace ForgeConductor::Persistence::Windows
