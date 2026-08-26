#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"

#include "Detail/BoundedSerialExecutor.h"
#include "Detail/OperationContextGuard.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

constexpr std::string_view ZeroSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";

class LedgerDocumentException final : public std::runtime_error {
public:
    explicit LedgerDocumentException(Domain::Error error)
        : std::runtime_error{error.message}, error_{std::move(error)}
    {
    }

    [[nodiscard]] const Domain::Error& error() const noexcept { return error_; }

private:
    Domain::Error error_;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw LedgerDocumentException{std::move(error)};
}

[[noreturn]] void reject(const std::string_view code, const std::string_view message)
{
    fail(Domain::makeError(code, std::string{message}));
}

[[noreturn]] void integrity(const std::string_view message)
{
    reject(Domain::ErrorCodes::IntegrityFailure, message);
}

void requireExactFields(
    const Json& object,
    const std::initializer_list<std::string_view> fields,
    const std::string_view label)
{
    if (!object.is_object()) {
        integrity(std::string{label} + " must be a JSON object.");
    }
    if (object.size() != fields.size()) {
        integrity(std::string{label} + " has missing or unsupported fields.");
    }
    for (const auto field : fields) {
        if (!object.contains(std::string{field})) {
            integrity(std::string{label} + " omits a required field.");
        }
    }
}

[[nodiscard]] const Json& requiredMember(
    const Json& object,
    const std::string_view name)
{
    const auto iterator = object.find(std::string{name});
    if (iterator == object.end()) {
        integrity("The native session ledger omits a required field.");
    }
    return *iterator;
}

[[nodiscard]] std::uint64_t requiredUnsigned(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredMember(object, name);
    if (!value.is_number_unsigned()) {
        integrity("A native session ledger unsigned integer has the wrong JSON type.");
    }
    try {
        return value.get<std::uint64_t>();
    } catch (...) {
        integrity("A native session ledger unsigned integer exceeds its supported range.");
    }
}

[[nodiscard]] std::string requiredString(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredMember(object, name);
    if (!value.is_string()) {
        integrity("A native session ledger string has the wrong JSON type.");
    }
    auto text = value.get<std::string>();
    if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text)) {
        integrity("A native session ledger string is not valid NUL-free UTF-8.");
    }
    return text;
}

[[nodiscard]] std::optional<std::string> nullableString(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredMember(object, name);
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_string()) {
        integrity("A nullable native session ledger string has the wrong JSON type.");
    }
    auto text = value.get<std::string>();
    if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text)) {
        integrity("A native session ledger string is not valid NUL-free UTF-8.");
    }
    return text;
}

template <typename Identifier>
[[nodiscard]] Identifier parseUuidIdentifier(
    const std::string_view value,
    const std::string_view label)
{
    auto parsed = Identifier::parse(value);
    if (!parsed) {
        integrity(std::string{"The native session ledger contains an invalid "} +
                  std::string{label} + '.');
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::IdempotencyKey parseIdempotencyKey(
    const std::string_view value)
{
    auto parsed = Domain::IdempotencyKey::create(value);
    if (!parsed) {
        integrity("The native session ledger contains an invalid idempotency key.");
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::ProviderSessionId parseProviderSessionId(
    const std::string_view value)
{
    auto parsed = Domain::ProviderSessionId::parse(value, 512U);
    if (!parsed) {
        integrity("The native session ledger contains an invalid provider session ID.");
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::Sha256Digest parseDigest(const std::string_view value)
{
    auto parsed = Domain::Sha256Digest::parse(value);
    if (!parsed) {
        integrity("The native session ledger contains an invalid SHA-256 digest.");
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
        integrity("A native session ledger timestamp is not canonical UTC.");
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
        integrity("A native session ledger timestamp is invalid.");
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
        integrity("A native session ledger timestamp is outside the supported UTC range.");
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
        std::chrono::milliseconds{*millisecond};
}

[[nodiscard]] Domain::UtcTimePoint normalizeTimestamp(
    const Domain::UtcTimePoint timestamp)
{
    const auto elapsed = timestamp.time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    if (milliseconds.count() < 0) {
        integrity("A native session ledger timestamp predates the supported UTC range.");
    }
    return Domain::UtcTimePoint{milliseconds};
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto normalized = normalizeTimestamp(timestamp);
    const auto totalMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        normalized.time_since_epoch());
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(totalMilliseconds);
    const auto milliseconds = static_cast<int>((totalMilliseconds - seconds).count());
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0) {
        integrity("A native session ledger timestamp is outside the supported UTC range.");
    }

    std::array<char, 25U> buffer{};
    const int written = milliseconds == 0
        ? std::snprintf(
              buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
              utc.tm_min, utc.tm_sec)
        : std::snprintf(
              buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
              utc.tm_min, utc.tm_sec, milliseconds);
    const int expected = milliseconds == 0 ? 20 : 24;
    if (written != expected) {
        integrity("A native session ledger timestamp could not be formatted.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] std::string_view statusName(
    const Domain::HostSessionStatus status)
{
    switch (status) {
    case Domain::HostSessionStatus::Creating:
        return "creating";
    case Domain::HostSessionStatus::Active:
        return "active";
    case Domain::HostSessionStatus::Bootstrapping:
        return "bootstrapping";
    case Domain::HostSessionStatus::Ready:
        return "ready";
    case Domain::HostSessionStatus::Sealed:
        return "sealed";
    case Domain::HostSessionStatus::Failed:
        return "failed";
    case Domain::HostSessionStatus::Cancelled:
        return "cancelled";
    }
    integrity("The native session ledger contains an unknown lifecycle status.");
}

[[nodiscard]] Domain::HostSessionStatus parseStatus(const std::string_view value)
{
    if (value == "creating")
        return Domain::HostSessionStatus::Creating;
    if (value == "active")
        return Domain::HostSessionStatus::Active;
    if (value == "bootstrapping")
        return Domain::HostSessionStatus::Bootstrapping;
    if (value == "ready")
        return Domain::HostSessionStatus::Ready;
    if (value == "sealed")
        return Domain::HostSessionStatus::Sealed;
    if (value == "failed")
        return Domain::HostSessionStatus::Failed;
    if (value == "cancelled")
        return Domain::HostSessionStatus::Cancelled;
    integrity("The native session ledger contains an unknown lifecycle status.");
}

[[nodiscard]] Json optionalTextJson(const std::optional<std::string>& value)
{
    return value ? Json(*value) : Json(nullptr);
}

template <typename Identifier>
[[nodiscard]] Json optionalIdentifierJson(const std::optional<Identifier>& value)
{
    return value ? Json(value->value()) : Json(nullptr);
}

[[nodiscard]] Json recordDocument(const Domain::NativeSessionRecord& record)
{
    Json document = Json::object();
    document["created_at"] = timestampText(record.createdAt);
    document["handoff_id"] = optionalIdentifierJson(record.handoffId);
    document["handoff_sha256"] = optionalIdentifierJson(record.handoffSha256);
    document["idempotency_key"] = record.session.idempotencyKey.value();
    document["input_tokens"] = record.inputTokens;
    document["model"] = optionalTextJson(record.session.model);
    document["operation_id"] = record.session.operationId.value();
    document["output_tokens"] = record.outputTokens;
    document["owner_operation_id"] = record.ownerOperationId.value();
    document["predecessor_session_id"] = record.session.predecessorSessionId.value();
    document["project_id"] = record.session.projectId.value();
    document["provider_session_id"] = optionalIdentifierJson(record.session.providerSessionId);
    document["session_id"] = record.session.id.value();
    document["status"] = statusName(record.session.status);
    document["updated_at"] = timestampText(record.updatedAt);
    return document;
}

[[nodiscard]] Json ledgerDocument(
    const Domain::NativeSessionLedger& ledger,
    const std::string_view checksum)
{
    Json records = Json::array();
    for (const auto& record : ledger.records) {
        records.push_back(recordDocument(record));
    }

    Json document = Json::object();
    document["content_sha256"] = checksum;
    document["records"] = std::move(records);
    document["revision"] = ledger.revision;
    document["schema_version"] = ledger.schemaVersion;
    return document;
}

[[nodiscard]] std::string canonicalJson(const Json& document)
{
    try {
        return document.dump(-1, ' ', false, Json::error_handler_t::strict);
    } catch (const nlohmann::json::exception&) {
        integrity("The native session ledger cannot be represented as strict UTF-8 JSON.");
    }
}

[[nodiscard]] Domain::Sha256Digest contentChecksum(
    const Domain::NativeSessionLedger& ledger,
    Contracts::IHasher& hasher)
{
    const auto canonical = canonicalJson(ledgerDocument(ledger, ZeroSha256));
    auto hashed = hasher.sha256(std::as_bytes(std::span<const char>{
        canonical.data(), canonical.size()}));
    if (!hashed) {
        fail(std::move(hashed).error());
    }
    return std::move(hashed).value();
}

[[nodiscard]] Domain::NativeSessionRecord decodeRecord(const Json& document)
{
    requireExactFields(
        document,
        {"created_at", "handoff_id", "handoff_sha256", "idempotency_key",
         "input_tokens", "model", "operation_id", "output_tokens",
         "owner_operation_id", "predecessor_session_id", "project_id",
         "provider_session_id", "session_id", "status", "updated_at"},
        "A native session ledger record");

    std::optional<Domain::ContinuityHandoffId> handoffId;
    if (const auto value = nullableString(document, "handoff_id")) {
        handoffId = parseUuidIdentifier<Domain::ContinuityHandoffId>(
            *value, "handoff ID");
    }
    std::optional<Domain::Sha256Digest> handoffSha256;
    if (const auto value = nullableString(document, "handoff_sha256")) {
        handoffSha256 = parseDigest(*value);
    }
    std::optional<Domain::ProviderSessionId> providerSessionId;
    if (const auto value = nullableString(document, "provider_session_id")) {
        providerSessionId = parseProviderSessionId(*value);
    }

    return Domain::NativeSessionRecord{
        Domain::HostSession{
            parseUuidIdentifier<Domain::SessionId>(
                requiredString(document, "session_id"), "logical session ID"),
            parseUuidIdentifier<Domain::ProjectId>(
                requiredString(document, "project_id"), "project ID"),
            parseUuidIdentifier<Domain::ContinuityOperationId>(
                requiredString(document, "operation_id"), "continuity operation ID"),
            parseUuidIdentifier<Domain::SessionId>(
                requiredString(document, "predecessor_session_id"),
                "predecessor session ID"),
            parseIdempotencyKey(requiredString(document, "idempotency_key")),
            std::move(providerSessionId),
            nullableString(document, "model"),
            parseStatus(requiredString(document, "status"))},
        parseUuidIdentifier<Domain::OperationId>(
            requiredString(document, "owner_operation_id"), "owner operation ID"),
        std::move(handoffId),
        std::move(handoffSha256),
        requiredUnsigned(document, "input_tokens"),
        requiredUnsigned(document, "output_tokens"),
        parseTimestamp(requiredString(document, "created_at")),
        parseTimestamp(requiredString(document, "updated_at"))};
}

[[nodiscard]] Domain::NativeSessionLedger decodeLedger(
    const Json& document,
    Contracts::IHasher& hasher)
{
    requireExactFields(
        document,
        {"content_sha256", "records", "revision", "schema_version"},
        "The native session ledger root");

    const auto schemaVersion = requiredUnsigned(document, "schema_version");
    if (schemaVersion != WindowsNativeSessionLedger::SchemaVersion) {
        reject(Domain::ErrorCodes::UnsupportedVersion,
               "The native session ledger schema version is unsupported.");
    }
    const auto revision = requiredUnsigned(document, "revision");
    if (revision == 0U) {
        integrity("A persisted native session ledger revision must be positive.");
    }
    const auto checksum = parseDigest(requiredString(document, "content_sha256"));

    const auto& recordsJson = requiredMember(document, "records");
    if (!recordsJson.is_array()) {
        integrity("The native session ledger records field must be a JSON array.");
    }
    if (recordsJson.size() > Domain::MaximumNativeSessionRecords) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "The native session ledger exceeds its record bound.");
    }

    std::vector<Domain::NativeSessionRecord> records;
    records.reserve(recordsJson.size());
    for (const auto& record : recordsJson) {
        records.push_back(decodeRecord(record));
    }

    Domain::NativeSessionLedger ledger{
        static_cast<std::uint32_t>(schemaVersion),
        revision,
        std::move(records),
        checksum};
    auto valid = Domain::validateNativeSessionLedger(ledger);
    if (!valid) {
        fail(std::move(valid).error());
    }
    const auto expected = contentChecksum(ledger, hasher);
    if (expected != checksum) {
        integrity("The native session ledger content checksum does not match its canonical data.");
    }
    return ledger;
}

[[nodiscard]] Domain::Result<Domain::NativeSessionLedger> parseDocument(
    const std::span<const std::byte> bytes,
    Contracts::IHasher& hasher) noexcept
{
    try {
        if (bytes.empty()) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "The native session ledger document is empty."));
        }
        if (bytes.size() > WindowsNativeSessionLedger::MaximumDocumentBytes) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "The native session ledger exceeds 1 MiB."));
        }
        const std::string text{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text)) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "The native session ledger is not valid NUL-free UTF-8."));
        }

        std::vector<std::unordered_set<std::string>> objectKeys;
        const auto callback = [&objectKeys](
                                  const int depth,
                                  const Json::parse_event_t event,
                                  Json& parsed) {
            if (depth < 0 || static_cast<std::size_t>(depth) >
                    WindowsNativeSessionLedger::MaximumJsonDepth) {
                reject(Domain::ErrorCodes::LimitExceeded,
                       "The native session ledger exceeds its JSON depth bound.");
            }
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (objectKeys.empty() ||
                    !objectKeys.back().insert(parsed.get<std::string>()).second) {
                    integrity("The native session ledger contains a duplicate JSON key.");
                }
            } else if (event == Json::parse_event_t::object_end) {
                if (objectKeys.empty()) {
                    integrity("The native session ledger JSON object nesting is malformed.");
                }
                objectKeys.pop_back();
            }
            return true;
        };

        auto document = Json::parse(text, callback, true, false);
        if (!objectKeys.empty()) {
            integrity("The native session ledger JSON object nesting is incomplete.");
        }
        return Domain::Result<Domain::NativeSessionLedger>::success(
            decodeLedger(document, hasher));
    } catch (const LedgerDocumentException& error) {
        return Domain::Result<Domain::NativeSessionLedger>::failure(error.error());
    } catch (const nlohmann::json::parse_error&) {
        return Domain::Result<Domain::NativeSessionLedger>::failure(
            Domain::makeError(Domain::ErrorCodes::MalformedMessage,
                              "The native session ledger is not valid strict JSON."));
    } catch (const nlohmann::json::exception&) {
        return Domain::Result<Domain::NativeSessionLedger>::failure(
            Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                              "The native session ledger contains invalid JSON values."));
    } catch (...) {
        return Domain::Result<Domain::NativeSessionLedger>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The native session ledger could not be decoded."));
    }
}

[[nodiscard]] Domain::Result<std::vector<std::byte>> serializeDocument(
    const Domain::NativeSessionLedger& ledger) noexcept
{
    try {
        if (!ledger.contentSha256) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "The committed native session ledger omits its checksum."));
        }
        auto text = ledgerDocument(ledger, ledger.contentSha256->value())
                        .dump(2, ' ', false, Json::error_handler_t::strict);
        text.push_back('\n');
        if (text.size() > WindowsNativeSessionLedger::MaximumDocumentBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "The native session ledger exceeds 1 MiB."));
        }
        std::vector<std::byte> bytes(text.size());
        if (!text.empty()) {
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch (const LedgerDocumentException& error) {
        return Domain::Result<std::vector<std::byte>>::failure(error.error());
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The native session ledger could not be serialized."));
    }
}

[[nodiscard]] bool samePathBinding(
    const Contracts::AuthorizedPath& readPath,
    const Contracts::AuthorizedPath& writePath,
    const Contracts::AuthorizedPath& createPath) noexcept
{
    return readPath.access() == Domain::FileAccess::Read &&
        writePath.access() == Domain::FileAccess::Write &&
        createPath.access() == Domain::FileAccess::Create &&
        readPath.authorityId() == writePath.authorityId() &&
        readPath.authorityId() == createPath.authorityId() &&
        readPath.canonicalPath() == writePath.canonicalPath() &&
        readPath.canonicalPath() == createPath.canonicalPath() &&
        readPath.authorityRoot() == writePath.authorityRoot() &&
        readPath.authorityRoot() == createPath.authorityRoot();
}

[[nodiscard]] bool backupPathBinding(
    const Contracts::AuthorizedPath& primaryReadPath,
    const Contracts::AuthorizedPath& backupReadPath) noexcept
{
    const auto& primary = primaryReadPath.canonicalPath().value();
    const auto& backup = backupReadPath.canonicalPath().value();
    return backupReadPath.access() == Domain::FileAccess::Read &&
        primaryReadPath.authorityId() == backupReadPath.authorityId() &&
        primaryReadPath.authorityRoot() == backupReadPath.authorityRoot() &&
        backup.size() == primary.size() + 4U && backup.starts_with(primary) &&
        backup.ends_with(".bak");
}

[[nodiscard]] bool recoverableDocumentError(const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::IntegrityFailure ||
        error.code == Domain::ErrorCodes::MalformedMessage ||
        error.code == Domain::ErrorCodes::UnsupportedVersion ||
        error.code == Domain::ErrorCodes::InvalidRequest ||
        error.code == Domain::ErrorCodes::PayloadTooLarge ||
        error.code == Domain::ErrorCodes::LimitExceeded;
}

[[nodiscard]] Domain::NativeSessionLedger emptyLedger()
{
    return Domain::NativeSessionLedger{
        WindowsNativeSessionLedger::SchemaVersion, 0U, {}, std::nullopt};
}

void normalizeLedgerTimestamps(Domain::NativeSessionLedger& ledger)
{
    for (auto& record : ledger.records) {
        record.createdAt = normalizeTimestamp(record.createdAt);
        record.updatedAt = normalizeTimestamp(record.updatedAt);
    }
}

} // namespace

class WindowsNativeSessionLedger::Impl final {
public:
    Impl(
        Contracts::IAtomicFileStore& atomicFileStore,
        Contracts::IHasher& hasher,
        Contracts::AuthorizedPath readPath,
        Contracts::AuthorizedPath writePath,
        Contracts::AuthorizedPath createPath,
        Contracts::AuthorizedPath backupReadPath)
        : atomicFileStore_{atomicFileStore}, hasher_{hasher},
          readPath_{std::move(readPath)}, writePath_{std::move(writePath)},
          createPath_{std::move(createPath)},
          backupReadPath_{std::move(backupReadPath)}, snapshot_{emptyLedger()}
    {
        if (!samePathBinding(readPath_, writePath_, createPath_) ||
            !backupPathBinding(readPath_, backupReadPath_)) {
            throw std::invalid_argument(
                "Native session ledger capabilities must bind one primary file and its "
                "read-only sibling backup under the same authority.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeSessionLedger> load(
        const Domain::OperationContext& context)
    {
        auto lease = executor_.acquire(context, "Native session ledger load");
        if (!lease) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(lease).error());
        }
        if (loaded_) {
            return Domain::Result<Domain::NativeSessionLedger>::success(snapshot_);
        }

        auto durable = readDurable(context);
        if (!durable) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(durable).error());
        }
        snapshot_ = std::move(durable).value().ledger;
        loaded_ = true;
        return Domain::Result<Domain::NativeSessionLedger>::success(snapshot_);
    }

    [[nodiscard]] Domain::Result<Domain::NativeSessionLedger> commit(
        const Domain::NativeSessionLedger& ledger,
        const std::uint64_t expectedRevision,
        const Domain::OperationContext& context)
    {
        auto lease = executor_.acquire(context, "Native session ledger commit");
        if (!lease) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(lease).error());
        }
        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "commit the native session ledger");
        if (!validContext) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(validContext).error());
        }
        if (ledger.schemaVersion != SchemaVersion ||
            ledger.revision != expectedRevision) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The native session ledger candidate does not match the expected revision."));
        }
        auto validLedger = Domain::validateNativeSessionLedger(ledger);
        if (!validLedger) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(validLedger).error());
        }

        auto durable = readDurable(context);
        if (!durable) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(durable).error());
        }
        auto current = std::move(durable).value();
        if (current.ledger.revision != expectedRevision ||
            current.ledger.contentSha256 != ledger.contentSha256) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The durable native session ledger changed before commit."));
        }
        if (expectedRevision == std::numeric_limits<std::uint64_t>::max()) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The native session ledger revision cannot be incremented."));
        }

        auto candidate = ledger;
        candidate.revision = expectedRevision + 1U;
        normalizeLedgerTimestamps(candidate);
        try {
            candidate.contentSha256 = contentChecksum(candidate, hasher_);
        } catch (const LedgerDocumentException& error) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(error.error());
        }
        validLedger = Domain::validateNativeSessionLedger(candidate);
        if (!validLedger) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(validLedger).error());
        }
        auto serialized = serializeDocument(candidate);
        if (!serialized) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(serialized).error());
        }

        validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "publish the native session ledger");
        if (!validContext) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(validContext).error());
        }
        const auto& persistPath = current.primaryExists ? writePath_ : createPath_;
        auto committed = atomicFileStore_.replace(
            persistPath, serialized.value(), true, context);
        if (!committed) {
            return Domain::Result<Domain::NativeSessionLedger>::failure(
                std::move(committed).error());
        }

        // Atomic replacement is the linearization point. Late cancellation must
        // not make a successfully published revision appear to have failed.
        snapshot_ = candidate;
        loaded_ = true;
        return Domain::Result<Domain::NativeSessionLedger>::success(
            std::move(candidate));
    }

    void shutdown() noexcept { executor_.shutdown(); }

private:
    struct DurableSnapshot final {
        Domain::NativeSessionLedger ledger;
        bool primaryExists{};
    };

    [[nodiscard]] Domain::Result<DurableSnapshot> readDurable(
        const Domain::OperationContext& context)
    {
        auto content = atomicFileStore_.read(readPath_, MaximumDocumentBytes, context);
        if (!content) {
            if (content.error().code == Domain::ErrorCodes::RecordNotFound) {
                auto validContext = Detail::validateOperationContext(
                    context, std::chrono::steady_clock::now(),
                    "load the missing native session ledger");
                if (!validContext) {
                    return Domain::Result<DurableSnapshot>::failure(
                        std::move(validContext).error());
                }
                return Domain::Result<DurableSnapshot>::success(
                    DurableSnapshot{emptyLedger(), false});
            }
            if (content.error().code == Domain::ErrorCodes::PayloadTooLarge ||
                content.error().code == Domain::ErrorCodes::IntegrityFailure) {
                return recoverFromBackup(context);
            }
            return Domain::Result<DurableSnapshot>::failure(
                std::move(content).error());
        }

        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "decode the native session ledger");
        if (!validContext) {
            return Domain::Result<DurableSnapshot>::failure(
                std::move(validContext).error());
        }
        auto parsed = parseDocument(content.value(), hasher_);
        if (!parsed) {
            if (recoverableDocumentError(parsed.error())) {
                return recoverFromBackup(context);
            }
            return Domain::Result<DurableSnapshot>::failure(
                std::move(parsed).error());
        }
        validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "finish loading the native session ledger");
        if (!validContext) {
            return Domain::Result<DurableSnapshot>::failure(
                std::move(validContext).error());
        }
        return Domain::Result<DurableSnapshot>::success(
            DurableSnapshot{std::move(parsed).value(), true});
    }

    [[nodiscard]] Domain::Result<DurableSnapshot> recoverFromBackup(
        const Domain::OperationContext& context)
    {
        auto content = atomicFileStore_.read(
            backupReadPath_, MaximumDocumentBytes, context);
        if (!content) {
            if (content.error().code == Domain::ErrorCodes::RecordNotFound ||
                content.error().code == Domain::ErrorCodes::PayloadTooLarge ||
                content.error().code == Domain::ErrorCodes::IntegrityFailure) {
                return invalidRecoveryPair();
            }
            return Domain::Result<DurableSnapshot>::failure(
                std::move(content).error());
        }

        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "decode the native session ledger backup");
        if (!validContext) {
            return Domain::Result<DurableSnapshot>::failure(
                std::move(validContext).error());
        }
        auto parsed = parseDocument(content.value(), hasher_);
        if (!parsed) {
            if (recoverableDocumentError(parsed.error())) {
                return invalidRecoveryPair();
            }
            return Domain::Result<DurableSnapshot>::failure(
                std::move(parsed).error());
        }
        validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(),
            "finish recovering the native session ledger backup");
        if (!validContext) {
            return Domain::Result<DurableSnapshot>::failure(
                std::move(validContext).error());
        }
        return Domain::Result<DurableSnapshot>::success(
            DurableSnapshot{std::move(parsed).value(), true});
    }

    [[nodiscard]] static Domain::Result<DurableSnapshot> invalidRecoveryPair()
    {
        return Domain::Result<DurableSnapshot>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The native session ledger is invalid and no valid sibling backup is available."));
    }

    Contracts::IAtomicFileStore& atomicFileStore_;
    Contracts::IHasher& hasher_;
    const Contracts::AuthorizedPath readPath_;
    const Contracts::AuthorizedPath writePath_;
    const Contracts::AuthorizedPath createPath_;
    const Contracts::AuthorizedPath backupReadPath_;
    Detail::BoundedSerialExecutor executor_;
    Domain::NativeSessionLedger snapshot_;
    bool loaded_{};
};

WindowsNativeSessionLedger::WindowsNativeSessionLedger(
    Contracts::IAtomicFileStore& atomicFileStore,
    Contracts::IHasher& hasher,
    Contracts::AuthorizedPath readPath,
    Contracts::AuthorizedPath writePath,
    Contracts::AuthorizedPath createPath,
    Contracts::AuthorizedPath backupReadPath)
    : implementation_{std::make_unique<Impl>(
          atomicFileStore, hasher, std::move(readPath), std::move(writePath),
          std::move(createPath), std::move(backupReadPath))}
{
}

WindowsNativeSessionLedger::~WindowsNativeSessionLedger() noexcept
{
    shutdown();
}

Domain::Result<Domain::NativeSessionLedger> WindowsNativeSessionLedger::load(
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->load(context);
    } catch (...) {
        return Domain::Result<Domain::NativeSessionLedger>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Native session ledger load failed at the platform boundary."));
    }
}

Domain::Result<Domain::NativeSessionLedger> WindowsNativeSessionLedger::commit(
    const Domain::NativeSessionLedger& ledger,
    const std::uint64_t expectedRevision,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->commit(ledger, expectedRevision, context);
    } catch (...) {
        return Domain::Result<Domain::NativeSessionLedger>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Native session ledger commit failed at the platform boundary."));
    }
}

void WindowsNativeSessionLedger::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
