#include "ForgeConductor/Persistence/Windows/WindowsLegacyContinuityRepository.h"

#include "Detail/WindowsDatabaseStore.h"
#include "Detail/WinsqliteConnection.h"
#include "Detail/WinsqliteStatement.h"
#include "Detail/WinsqliteTransaction.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
constexpr std::size_t MaximumJsonDepth = 64U;
constexpr std::string_view ResetAction = "reset_legacy_continuity";
constexpr std::string_view ResetScope = "legacy-context-continuity";
constexpr std::string_view ResetToken = "RESET LEGACY CONTINUITY";
constexpr std::string_view ProjectionColumns =
    "id,created_at,updated_at,source,resume_ready,packet_json,client_id,"
    "write_sequence,payload_json,content_sha256";

struct RepositoryFailure final {
    Domain::Error error;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw RepositoryFailure{std::move(error)};
}

[[noreturn]] void integrity(std::string message)
{
    fail(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message)));
}

[[noreturn]] void invalid(std::string message)
{
    fail(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest, std::move(message)));
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
            "The legacy continuity repository operation failed safely."));
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
                "The legacy continuity database operation produced no result."));
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
                "The legacy continuity database callback failed safely.");
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

[[nodiscard]] Detail::WindowsDatabaseStore& requireStore(
    Detail::WindowsDatabaseStore* store)
{
    if (!store) {
        invalid("The legacy continuity repository is closed.");
    }
    return *store;
}

void stepDone(WinsqliteStatement& statement)
{
    if (take(statement.step()) != WinsqliteStepResult::Done) {
        integrity("A legacy continuity write unexpectedly returned a row.");
    }
}

[[nodiscard]] std::string requiredText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto value = take(statement.columnText(column, maximumBytes));
    if (!value) {
        integrity("A required legacy continuity column is null: " +
                  std::string{field} + '.');
    }
    if (value->find('\0') != std::string::npos ||
        !Domain::isValidUtf8(*value)) {
        integrity("A persisted legacy continuity column is invalid UTF-8: " +
                  std::string{field} + '.');
    }
    return std::move(*value);
}

[[nodiscard]] std::optional<std::string> optionalText(
    const WinsqliteStatement& statement,
    const int column,
    const std::size_t maximumBytes,
    const std::string_view field)
{
    auto value = take(statement.columnText(column, maximumBytes));
    if (value && (value->find('\0') != std::string::npos ||
                  !Domain::isValidUtf8(*value))) {
        integrity("A persisted optional legacy continuity column is invalid UTF-8: " +
                  std::string{field} + '.');
    }
    return value;
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

[[nodiscard]] Domain::UtcTimePoint parseTimestamp(
    const std::string_view text)
{
    const bool seconds = text.size() == 20U && text[19] == 'Z';
    const bool milliseconds =
        text.size() == 24U && text[19] == '.' && text[23] == 'Z';
    if ((!seconds && !milliseconds) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        integrity("A persisted legacy continuity timestamp is not canonical UTC.");
    }
    const auto year = decimalComponent(text, 0U, 4U);
    const auto month = decimalComponent(text, 5U, 2U);
    const auto day = decimalComponent(text, 8U, 2U);
    const auto hour = decimalComponent(text, 11U, 2U);
    const auto minute = decimalComponent(text, 14U, 2U);
    const auto second = decimalComponent(text, 17U, 2U);
    const auto millis = milliseconds
        ? decimalComponent(text, 20U, 3U)
        : std::optional<int>{0};
    if (!year || !month || !day || !hour || !minute || !second || !millis ||
        *year < 1970 || *month < 1 || *month > 12 || *day < 1 || *day > 31 ||
        *hour > 23 || *minute > 59 || *second > 59) {
        integrity("A persisted legacy continuity timestamp is invalid.");
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
        integrity("A persisted legacy continuity timestamp is outside range.");
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
           std::chrono::milliseconds{*millis};
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto elapsed = timestamp.time_since_epoch();
    const auto totalMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    if (totalMilliseconds.count() < 0) {
        invalid("A legacy continuity timestamp predates the supported range.");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(totalMilliseconds);
    const int millis = static_cast<int>((totalMilliseconds - seconds).count());
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0) {
        invalid("A legacy continuity timestamp is outside the supported range.");
    }
    std::array<char, 25U> buffer{};
    const int written = millis == 0
        ? std::snprintf(
              buffer.data(), buffer.size(),
              "%04d-%02d-%02dT%02d:%02d:%02dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec)
        : std::snprintf(
              buffer.data(), buffer.size(),
              "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
              utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    if (written != (millis == 0 ? 20 : 24)) {
        invalid("A legacy continuity timestamp could not be formatted.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

void validateJsonTree(const Json& value, const std::size_t depth)
{
    if (depth > MaximumJsonDepth) {
        integrity("A persisted legacy continuity document exceeds the JSON depth limit.");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text)) {
            integrity("A persisted legacy continuity document contains invalid UTF-8.");
        }
    } else if (value.is_object()) {
        for (auto item = value.cbegin(); item != value.cend(); ++item) {
            if (item.key().find('\0') != std::string::npos ||
                !Domain::isValidUtf8(item.key())) {
                integrity("A persisted legacy continuity document contains an invalid key.");
            }
            validateJsonTree(*item, depth + 1U);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            validateJsonTree(item, depth + 1U);
        }
    }
}

[[nodiscard]] Json parseJson(const std::string_view encoded)
{
    bool invalidStructure{};
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&](const int depth,
                              const Json::parse_event_t event,
                              Json& parsed) {
        if (depth < 0 || depth > static_cast<int>(MaximumJsonDepth)) {
            invalidStructure = true;
        }
        if (event == Json::parse_event_t::object_start) {
            objectKeys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            if (objectKeys.empty() || !parsed.is_string() ||
                !objectKeys.back().insert(
                    parsed.get_ref<const std::string&>()).second) {
                invalidStructure = true;
            }
        } else if (event == Json::parse_event_t::object_end) {
            if (objectKeys.empty()) {
                invalidStructure = true;
            } else {
                objectKeys.pop_back();
            }
        }
        return true;
    };
    auto document = Json::parse(encoded, callback, false, false);
    if (invalidStructure || !objectKeys.empty() || document.is_discarded() ||
        !document.is_object()) {
        integrity("A persisted legacy continuity document is not one bounded JSON object.");
    }
    validateJsonTree(document, 0U);
    return document;
}

[[nodiscard]] const Json& objectOrEmpty(
    const Json& parent,
    const std::string_view field,
    Json& empty)
{
    const auto found = parent.find(std::string{field});
    if (found == parent.end()) {
        empty = Json::object();
        return empty;
    }
    if (!found->is_object()) {
        integrity("A persisted legacy continuity section has the wrong JSON type.");
    }
    return *found;
}

[[nodiscard]] std::optional<std::string> optionalString(
    const Json& object,
    const std::string_view field)
{
    const auto found = object.find(std::string{field});
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_string()) {
        integrity("A persisted legacy continuity text field has the wrong JSON type.");
    }
    return found->get<std::string>();
}

[[nodiscard]] std::string stringOr(
    const Json& object,
    const std::string_view field,
    std::string fallback)
{
    auto value = optionalString(object, field);
    return value ? std::move(*value) : std::move(fallback);
}

[[nodiscard]] bool booleanOr(
    const Json& object,
    const std::string_view field,
    const bool fallback)
{
    const auto found = object.find(std::string{field});
    if (found == object.end()) {
        return fallback;
    }
    if (!found->is_boolean()) {
        integrity("A persisted legacy continuity Boolean has the wrong JSON type.");
    }
    return found->get<bool>();
}

[[nodiscard]] std::uint32_t schemaVersion(const Json& object)
{
    const auto found = object.find("schema_version");
    if (found == object.end()) {
        return Domain::LegacyContinuityLimits::SchemaVersion;
    }
    std::uint64_t value{};
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else if (found->is_number_integer()) {
        const auto signedValue = found->get<std::int64_t>();
        if (signedValue < 0) {
            integrity("A persisted legacy continuity schema version is negative.");
        }
        value = static_cast<std::uint64_t>(signedValue);
    } else {
        integrity("A persisted legacy continuity schema version is not an integer.");
    }
    if (value != Domain::LegacyContinuityLimits::SchemaVersion) {
        fail(Domain::makeError(
            Domain::ErrorCodes::UnsupportedVersion,
            "The persisted legacy continuity schema version is unsupported."));
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::vector<std::string> stringArrayOrEmpty(
    const Json& object,
    const std::string_view field)
{
    const auto found = object.find(std::string{field});
    if (found == object.end()) {
        return {};
    }
    if (!found->is_array() ||
        found->size() > Domain::LegacyContinuityLimits::MaximumCollectionItems) {
        integrity("A persisted legacy continuity collection is not a bounded array.");
    }
    std::vector<std::string> values;
    values.reserve(found->size());
    for (const auto& item : *found) {
        if (!item.is_string()) {
            integrity("A persisted legacy continuity collection contains a non-string item.");
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

template <typename Identifier>
[[nodiscard]] Identifier identifier(
    const std::string_view value,
    const std::string_view field)
{
    auto parsed = Identifier::parse(value);
    if (!parsed) {
        integrity("A persisted legacy continuity document contains an invalid " +
                  std::string{field} + '.');
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::LegacyHandoffPacket decodePacket(
    const std::string_view encoded)
{
    const auto root = parseJson(encoded);
    Json emptyMeta;
    Json emptyTask;
    Json emptyWorking;
    Json emptyResume;
    const auto& meta = objectOrEmpty(root, "meta", emptyMeta);
    const auto& task = objectOrEmpty(root, "task", emptyTask);
    const auto& working = objectOrEmpty(root, "working_set", emptyWorking);
    const auto& resume = objectOrEmpty(root, "resume", emptyResume);

    const auto rootVersion = schemaVersion(root);
    const auto metaVersion = schemaVersion(meta);
    if (rootVersion != metaVersion) {
        integrity("Persisted legacy continuity schema versions disagree.");
    }
    auto idText = optionalString(meta, "id");
    if (!idText) {
        idText = optionalString(root, "id");
    }
    if (!idText) {
        integrity("A persisted legacy continuity document has no handoff id.");
    }
    const auto sourceText = stringOr(meta, "source", "model");
    auto source = Domain::legacyHandoffSourceFromWire(sourceText);
    if (!source) {
        integrity("A persisted legacy continuity document has an invalid source.");
    }

    const auto created = optionalString(meta, "created_at");
    const auto updated = optionalString(meta, "updated_at");
    if (!created || !updated) {
        integrity("A persisted legacy continuity document lacks timestamps.");
    }

    std::optional<Domain::ClientId> clientId;
    if (const auto value = optionalString(meta, "client_id")) {
        clientId = identifier<Domain::ClientId>(*value, "client id");
    }

    std::vector<Domain::LegacyAgentContinuitySnapshot> agents;
    const auto agentsValue = root.find("agents");
    if (agentsValue != root.end()) {
        if (!agentsValue->is_array() ||
            agentsValue->size() >
                Domain::LegacyContinuityLimits::MaximumAgentSnapshots) {
            integrity("Persisted legacy continuity agents are not a bounded array.");
        }
        agents.reserve(agentsValue->size());
        for (const auto& item : *agentsValue) {
            if (!item.is_object()) {
                integrity("A persisted legacy continuity agent is not an object.");
            }
            const auto session = optionalString(item, "session_id");
            const auto agent = optionalString(item, "agent_id");
            if (!session || !agent) {
                integrity("A persisted legacy continuity agent lacks an identifier.");
            }
            std::optional<Domain::UtcTimePoint> agentUpdatedAt;
            if (const auto value = optionalString(item, "updated_at")) {
                agentUpdatedAt = parseTimestamp(*value);
            }
            agents.push_back(Domain::LegacyAgentContinuitySnapshot{
                identifier<Domain::SessionId>(*session, "agent session id"),
                identifier<Domain::AgentId>(*agent, "agent id"),
                stringOr(item, "goal", {}),
                optionalString(item, "cwd"),
                stringOr(item, "status", "open"),
                agentUpdatedAt,
                stringOr(item, "resume_hint", {})});
        }
    }

    const auto resumeSeed = stringOr(resume, "seed", {});
    bool custom = booleanOr(resume, "custom", false);
    if (resume.find("custom") == resume.end() && !resumeSeed.empty()) {
        custom = !resumeSeed.starts_with(
            "Forge Continuity resume (handoff " + *idText + ").");
    }

    Domain::LegacyHandoffPacket packet{
        identifier<Domain::LegacyHandoffId>(*idText, "handoff id"),
        rootVersion,
        parseTimestamp(*created),
        parseTimestamp(*updated),
        std::move(source).value(),
        booleanOr(meta, "resume_ready", false),
        optionalString(meta, "chat_label"),
        std::move(clientId),
        stringOr(task, "goal", {}),
        stringOr(task, "status", "in_progress"),
        optionalString(task, "project_slug"),
        optionalString(task, "cwd"),
        stringArrayOrEmpty(task, "blockers"),
        stringArrayOrEmpty(task, "next_actions"),
        stringArrayOrEmpty(working, "key_files"),
        stringArrayOrEmpty(working, "decisions"),
        std::move(agents),
        stringOr(root, "narrative", {}),
        resumeSeed,
        custom};
    auto valid = Domain::validateLegacyHandoffPacket(packet);
    if (!valid) {
        integrity("A persisted legacy continuity packet violates semantic bounds.");
    }
    return packet;
}

[[nodiscard]] Json encodeAgent(
    const Domain::LegacyAgentContinuitySnapshot& agent)
{
    Json result = Json::object();
    result["agent_id"] = agent.agentId.value();
    if (agent.workingDirectory) {
        result["cwd"] = *agent.workingDirectory;
    }
    result["goal"] = agent.goal;
    result["resume_hint"] = agent.resumeHint;
    result["session_id"] = agent.sessionId.value();
    result["status"] = agent.status;
    if (agent.updatedAt) {
        result["updated_at"] = timestampText(*agent.updatedAt);
    }
    return result;
}

[[nodiscard]] std::string encodePacket(
    const Domain::LegacyHandoffPacket& packet)
{
    take(Domain::validateLegacyHandoffPacket(packet));
    Json meta = Json::object();
    if (packet.chatLabel) meta["chat_label"] = *packet.chatLabel;
    if (packet.clientId) meta["client_id"] = packet.clientId->value();
    meta["created_at"] = timestampText(packet.createdAt);
    meta["id"] = packet.id.value();
    meta["resume_ready"] = packet.resumeReady;
    meta["schema_version"] = packet.schemaVersion;
    meta["source"] = std::string{Domain::wireName(packet.source)};
    meta["updated_at"] = timestampText(packet.updatedAt);

    Json task = Json::object();
    task["blockers"] = packet.blockers;
    if (packet.workingDirectory) task["cwd"] = *packet.workingDirectory;
    task["goal"] = packet.goal;
    task["next_actions"] = packet.nextActions;
    if (packet.projectSlug) task["project_slug"] = *packet.projectSlug;
    task["status"] = packet.status;

    Json agents = Json::array();
    for (const auto& agent : packet.agents) {
        agents.push_back(encodeAgent(agent));
    }
    Json root = Json::object();
    root["agents"] = std::move(agents);
    root["meta"] = std::move(meta);
    root["narrative"] = packet.narrative;
    root["resume"] = Json{
        {"custom", packet.resumeSeedIsCustom},
        {"instructions", Json::array({
            "Call context_get if you need the full packet again",
            "Pass this handoff id to session_checkpoint/session_handoff when continuing it",
            "Reattach open agents with agent_run_status(session_id) or complete and restart",
            "Update memory/current-task.md via session_checkpoint as you progress"})},
        {"seed", packet.resumeSeed}};
    root["schema_version"] = packet.schemaVersion;
    root["task"] = std::move(task);
    root["working_set"] = Json{
        {"decisions", packet.decisions}, {"key_files", packet.keyFiles}};
    validateJsonTree(root, 0U);
    const auto encoded = root.dump(-1, ' ', false, Json::error_handler_t::strict);
    if (encoded.size() > Domain::LegacyContinuityLimits::MaximumPacketBytes) {
        fail(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The canonical legacy continuity document exceeds one MiB."));
    }
    return encoded;
}

[[nodiscard]] Domain::Sha256Digest digest(
    Contracts::IHasher& hasher,
    const std::string_view bytes)
{
    return take(hasher.sha256(std::as_bytes(std::span<const char>{
        bytes.data(), bytes.size()})));
}

[[nodiscard]] Domain::LegacyContinuityRecord readRecord(
    const WinsqliteStatement& statement,
    Contracts::IHasher& hasher)
{
    const auto rowId = requiredText(statement, 0, 128U, "id");
    const auto created = requiredText(
        statement, 1, MaximumTimestampBytes, "created_at");
    const auto updated = requiredText(
        statement, 2, MaximumTimestampBytes, "updated_at");
    const auto source = requiredText(statement, 3, 16U, "source");
    const auto resumeReady = take(statement.columnInt64(4));
    const auto packetJson = requiredText(
        statement, 5,
        Domain::LegacyContinuityLimits::MaximumPacketBytes, "packet_json");
    const auto rowClient = optionalText(statement, 6, 128U, "client_id");
    const auto sequence = take(statement.columnInt64(7));
    const auto payloadJson = optionalText(
        statement, 8,
        Domain::LegacyContinuityLimits::MaximumPacketBytes, "payload_json");
    const auto hashText = optionalText(statement, 9, 64U, "content_sha256");
    if ((resumeReady != 0 && resumeReady != 1) || sequence <= 0) {
        integrity("A persisted legacy continuity row has invalid scalar metadata.");
    }
    if (payloadJson.has_value() != hashText.has_value()) {
        // C006 migrated source rows by copying packet_json to payload_json but
        // deliberately had no digest. Treat that pair as legacy packet-only.
        if (!(payloadJson && !hashText && *payloadJson == packetJson)) {
            integrity("A persisted legacy continuity payload and digest are incomplete.");
        }
    }
    const bool canonicalPair = payloadJson && hashText;
    const auto& selected = canonicalPair ? *payloadJson : packetJson;
    if (canonicalPair) {
        auto parsedDigest = Domain::Sha256Digest::parse(*hashText);
        if (!parsedDigest || digest(hasher, selected) != parsedDigest.value()) {
            integrity("A persisted legacy continuity payload failed SHA-256 validation.");
        }
    }
    auto packet = decodePacket(selected);
    if (packet.id.value() != rowId ||
        timestampText(packet.createdAt) != created ||
        timestampText(packet.updatedAt) != updated ||
        Domain::wireName(packet.source) != source ||
        packet.resumeReady != (resumeReady != 0) ||
        (rowClient.has_value() != packet.clientId.has_value()) ||
        (rowClient && *rowClient != packet.clientId->value())) {
        integrity("A persisted legacy continuity row disagrees with its document metadata.");
    }
    Domain::LegacyContinuityRecord record{
        std::move(packet),
        static_cast<std::uint64_t>(sequence),
        Domain::LegacyContinuityDocuments{
            packetJson,
            canonicalPair ? payloadJson : std::nullopt,
            canonicalPair
                ? std::optional<Domain::Sha256Digest>{
                      take(Domain::Sha256Digest::parse(*hashText))}
                : std::nullopt}};
    auto valid = Domain::validateLegacyContinuityRecord(record);
    if (!valid) {
        integrity("A persisted legacy continuity record violates semantic bounds.");
    }
    return record;
}

[[nodiscard]] std::vector<Domain::LegacyContinuityRecord> readRows(
    WinsqliteStatement& statement,
    Contracts::IHasher& hasher,
    const std::size_t maximumCount)
{
    std::vector<Domain::LegacyContinuityRecord> records;
    records.reserve(maximumCount);
    for (;;) {
        const auto stepped = take(statement.step());
        if (stepped == WinsqliteStepResult::Done) break;
        if (records.size() >= maximumCount) {
            integrity("A legacy continuity query exceeded its row bound.");
        }
        records.push_back(readRecord(statement, hasher));
    }
    take(Domain::validateLegacyContinuityList(records, maximumCount));
    return records;
}

[[nodiscard]] std::optional<std::string> pointerId(
    WinsqliteTransaction& transaction,
    const bool resumeReady)
{
    auto statement = take(transaction.prepare(
        resumeReady
            ? "SELECT id FROM context_handoffs WHERE resume_ready=1 "
              "ORDER BY write_sequence DESC,id ASC LIMIT 1"
            : "SELECT id FROM context_handoffs "
              "ORDER BY write_sequence DESC,id ASC LIMIT 1"));
    if (take(statement.step()) == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    return requiredText(statement, 0, 128U, "continuity pointer id");
}

[[nodiscard]] std::optional<std::string> existingPointer(
    WinsqliteTransaction& transaction,
    const std::string_view key)
{
    auto statement = take(transaction.prepare(
        "SELECT body FROM memory_notes WHERE key=?"));
    take(statement.bindText(1, key));
    if (take(statement.step()) == WinsqliteStepResult::Done) {
        return std::nullopt;
    }
    return requiredText(statement, 0, 128U, "continuity pointer body");
}

[[nodiscard]] bool replacePointer(
    WinsqliteTransaction& transaction,
    const std::string_view key,
    const std::string_view tags,
    const std::optional<std::string>& desired,
    const std::string_view timestamp)
{
    const auto before = existingPointer(transaction, key);
    if (before == desired) return false;
    if (!desired) {
        auto deletion = take(transaction.prepare(
            "DELETE FROM memory_notes WHERE key=?"));
        take(deletion.bindText(1, key));
        stepDone(deletion);
        return true;
    }
    auto write = take(transaction.prepare(
        "INSERT INTO memory_notes(key,body,tags_json,created_at,updated_at) "
        "VALUES(?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET "
        "body=excluded.body,tags_json=excluded.tags_json,"
        "updated_at=excluded.updated_at"));
    take(write.bindText(1, key));
    take(write.bindText(2, *desired));
    take(write.bindText(3, tags));
    take(write.bindText(4, timestamp));
    take(write.bindText(5, timestamp));
    stepDone(write);
    return true;
}

[[nodiscard]] Domain::LegacyContinuityPointerRepairOutcome repairPointersIn(
    WinsqliteTransaction& transaction,
    const std::string_view timestamp)
{
    const auto latest = pointerId(transaction, false);
    const auto resume = pointerId(transaction, true);
    std::size_t changed{};
    if (replacePointer(
            transaction, "continuity/latest", "[\"continuity\",\"latest\"]",
            latest, timestamp)) {
        ++changed;
    }
    if (replacePointer(
            transaction, "continuity/resume_ready",
            "[\"continuity\",\"resume\"]", resume, timestamp)) {
        ++changed;
    }
    std::optional<Domain::LegacyHandoffId> latestId;
    std::optional<Domain::LegacyHandoffId> resumeId;
    if (latest) latestId = identifier<Domain::LegacyHandoffId>(*latest, "latest pointer");
    if (resume) resumeId = identifier<Domain::LegacyHandoffId>(*resume, "resume pointer");
    return Domain::LegacyContinuityPointerRepairOutcome{
        std::move(latestId), std::move(resumeId), changed};
}

[[nodiscard]] std::size_t checkedCount(const std::int64_t value)
{
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(
                             (std::numeric_limits<std::size_t>::max)())) {
        integrity("A legacy continuity row count is outside range.");
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::int64_t scalar(
    WinsqliteTransaction& transaction,
    const std::string_view sql)
{
    auto statement = take(transaction.prepare(sql));
    if (take(statement.step()) != WinsqliteStepResult::Row) {
        integrity("A transactional legacy continuity scalar returned no row.");
    }
    return take(statement.columnInt64(0));
}

} // namespace

struct WindowsLegacyContinuityRepository::Impl final {
    Impl(
        std::shared_ptr<WindowsCentralDatabase> ownedDatabase,
        const bool closeOwnedDatabase,
        std::shared_ptr<Contracts::IClock> ownedClock,
        std::shared_ptr<Contracts::IHasher> ownedHasher) noexcept
        : database{std::move(ownedDatabase)},
          closeDatabaseOnClose{closeOwnedDatabase},
          clock{std::move(ownedClock)},
          hasher{std::move(ownedHasher)}
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
    std::shared_ptr<Contracts::IHasher> hasher;
};

WindowsLegacyContinuityRepository::WindowsLegacyContinuityRepository(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsLegacyContinuityRepository::~WindowsLegacyContinuityRepository() noexcept
{
    close();
}

Domain::Result<std::shared_ptr<WindowsLegacyContinuityRepository>>
WindowsLegacyContinuityRepository::open(
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IHasher> hasher,
    const Domain::OperationContext& context) noexcept
{
    if (!applicationPaths || !runtimeDiagnostics || !clock || !hasher) {
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The legacy continuity repository requires paths, diagnostics, clock, and hasher."));
    }
    auto database = WindowsCentralDatabase::open(
        std::move(applicationPaths), std::move(runtimeDiagnostics), clock, context);
    if (!database) {
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityRepository>>::failure(
            std::move(database).error());
    }
    return create(std::move(database).value(), std::move(clock), std::move(hasher));
}

Domain::Result<std::shared_ptr<WindowsLegacyContinuityRepository>>
WindowsLegacyContinuityRepository::create(
    std::unique_ptr<WindowsCentralDatabase> database,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IHasher> hasher) noexcept
{
    try {
        if (!database || !clock || !hasher) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyContinuityRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The owned legacy continuity repository has missing dependencies."));
        }
        auto implementation = std::make_unique<Impl>(
            std::shared_ptr<WindowsCentralDatabase>{std::move(database)},
            true, std::move(clock), std::move(hasher));
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityRepository>>::success(
            std::shared_ptr<WindowsLegacyContinuityRepository>{
                new WindowsLegacyContinuityRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The owned legacy continuity repository could not be constructed."));
    }
}

Domain::Result<std::shared_ptr<WindowsLegacyContinuityRepository>>
WindowsLegacyContinuityRepository::attach(
    std::shared_ptr<WindowsCentralDatabase> database,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IHasher> hasher) noexcept
{
    try {
        if (!database || !clock || !hasher) {
            return Domain::Result<
                std::shared_ptr<WindowsLegacyContinuityRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The shared legacy continuity repository has missing dependencies."));
        }
        auto implementation = std::make_unique<Impl>(
            std::move(database), false, std::move(clock), std::move(hasher));
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityRepository>>::success(
            std::shared_ptr<WindowsLegacyContinuityRepository>{
                new WindowsLegacyContinuityRepository{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<WindowsLegacyContinuityRepository>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The shared legacy continuity repository could not be constructed."));
    }
}

Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
WindowsLegacyContinuityRepository::get(
    const Domain::LegacyHandoffId& handoffId,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::LegacyContinuityRecord>>([&]() {
        if (!implementation_ || !implementation_->hasher) {
            invalid("The legacy continuity repository is closed.");
        }
        auto& store = requireStore(implementation_->repositoryStore());
        return take(runOnStore<std::optional<Domain::LegacyContinuityRecord>>(
            store, "Get legacy continuity handoff", context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::LegacyContinuityRecord>>([&]() {
                    auto statement = take(connection.prepare(
                        std::string{"SELECT "} + std::string{ProjectionColumns} +
                            " FROM context_handoffs WHERE id=?",
                        context));
                    take(statement.bindText(1, handoffId.value()));
                    if (take(statement.step()) == WinsqliteStepResult::Done) {
                        return std::optional<Domain::LegacyContinuityRecord>{};
                    }
                    return std::optional<Domain::LegacyContinuityRecord>{
                        readRecord(statement, *implementation_->hasher)};
                });
            }));
    });
}

Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
WindowsLegacyContinuityRepository::latest(
    const std::optional<Domain::ClientId>& clientId,
    const bool resumeReadyOnly,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::optional<Domain::LegacyContinuityRecord>>([&]() {
        if (!implementation_ || !implementation_->hasher) {
            invalid("The legacy continuity repository is closed.");
        }
        auto& store = requireStore(implementation_->repositoryStore());
        return take(runOnStore<std::optional<Domain::LegacyContinuityRecord>>(
            store, "Select latest legacy continuity handoff", context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::optional<Domain::LegacyContinuityRecord>>([&]() {
                    std::string sql = "SELECT ";
                    sql += ProjectionColumns;
                    sql += " FROM context_handoffs WHERE 1=1";
                    if (resumeReadyOnly) sql += " AND resume_ready=1";
                    if (clientId) sql += " AND client_id=?";
                    sql += " ORDER BY write_sequence DESC,id ASC LIMIT 1";
                    auto statement = take(connection.prepare(sql, context));
                    if (clientId) take(statement.bindText(1, clientId->value()));
                    if (take(statement.step()) == WinsqliteStepResult::Done) {
                        return std::optional<Domain::LegacyContinuityRecord>{};
                    }
                    return std::optional<Domain::LegacyContinuityRecord>{
                        readRecord(statement, *implementation_->hasher)};
                });
            }));
    });
}

Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
WindowsLegacyContinuityRepository::list(
    const std::size_t maximumCount,
    const Domain::OperationContext& context) noexcept
{
    return listAll(maximumCount, context);
}

Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
WindowsLegacyContinuityRepository::listAll(
    const std::size_t maximumCount,
    const Domain::OperationContext& context) noexcept
{
    return guarded<std::vector<Domain::LegacyContinuityRecord>>([&]() {
        if (maximumCount == 0U ||
            maximumCount > Domain::LegacyContinuityLimits::MaximumRepairRows) {
            invalid("The legacy continuity row limit is outside its bounded range.");
        }
        if (!implementation_ || !implementation_->hasher) {
            invalid("The legacy continuity repository is closed.");
        }
        auto& store = requireStore(implementation_->repositoryStore());
        return take(runOnStore<std::vector<Domain::LegacyContinuityRecord>>(
            store, "List legacy continuity handoffs", context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<std::vector<Domain::LegacyContinuityRecord>>([&]() {
                    auto statement = take(connection.prepare(
                        std::string{"SELECT "} + std::string{ProjectionColumns} +
                            " FROM context_handoffs "
                            "ORDER BY write_sequence DESC,id ASC LIMIT ?",
                        context));
                    take(statement.bindInt64(
                        1, static_cast<std::int64_t>(maximumCount)));
                    return readRows(
                        statement, *implementation_->hasher, maximumCount);
                });
            }));
    });
}

Domain::Result<Domain::LegacyContinuityRecord>
WindowsLegacyContinuityRepository::compareExchange(
    const Domain::LegacyContinuityCompareExchange& mutation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyContinuityRecord>([&]() {
        take(Domain::validateLegacyHandoffPacket(mutation.packet));
        if (!implementation_ || !implementation_->clock ||
            !implementation_->hasher) {
            invalid("The legacy continuity repository is closed.");
        }
        const auto payload = encodePacket(mutation.packet);
        const auto contentDigest = digest(*implementation_->hasher, payload);
        const auto noteTimestamp = timestampText(implementation_->clock->utcNow());
        auto& store = requireStore(implementation_->repositoryStore());
        return take(runOnStore<Domain::LegacyContinuityRecord>(
            store, "Compare-exchange legacy continuity handoff", context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyContinuityRecord>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto existing = take(transaction.prepare(
                        "SELECT write_sequence,packet_json FROM context_handoffs WHERE id=?"));
                    take(existing.bindText(1, mutation.packet.id.value()));
                    const auto stepped = take(existing.step());
                    const bool exists = stepped == WinsqliteStepResult::Row;
                    std::optional<std::int64_t> priorSequence;
                    std::optional<std::string> priorPacket;
                    if (exists) {
                        priorSequence = take(existing.columnInt64(0));
                        priorPacket = requiredText(
                            existing, 1,
                            Domain::LegacyContinuityLimits::MaximumPacketBytes,
                            "packet_json");
                    }
                    if ((mutation.expectedWriteSequence &&
                         (!priorSequence || *priorSequence <= 0 ||
                          static_cast<std::uint64_t>(*priorSequence) !=
                              *mutation.expectedWriteSequence)) ||
                        (!mutation.expectedWriteSequence && exists)) {
                        fail(Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "The legacy continuity compare-exchange observed a stale sequence.",
                            true));
                    }
                    const auto maximum = scalar(
                        transaction,
                        "SELECT COALESCE(MAX(write_sequence),0) FROM context_handoffs");
                    if (maximum < 0 || maximum ==
                            (std::numeric_limits<std::int64_t>::max)()) {
                        integrity("The legacy continuity write sequence cannot advance.");
                    }
                    const auto nextSequence = maximum + 1;
                    auto write = take(transaction.prepare(
                        exists
                            ? "UPDATE context_handoffs SET created_at=?,updated_at=?,"
                              "source=?,resume_ready=?,packet_json=?,client_id=?,"
                              "write_sequence=?,project_id=NULL,session_id=NULL,"
                              "payload_json=?,content_sha256=? "
                              "WHERE id=? AND write_sequence=?"
                            : "INSERT INTO context_handoffs("
                              "created_at,updated_at,source,resume_ready,packet_json,"
                              "client_id,write_sequence,project_id,session_id,"
                              "payload_json,content_sha256,id) "
                              "VALUES(?,?,?,?,?,?,?,NULL,NULL,?,?,?)"));
                    take(write.bindText(1, timestampText(mutation.packet.createdAt)));
                    take(write.bindText(2, timestampText(mutation.packet.updatedAt)));
                    take(write.bindText(
                        3, Domain::wireName(mutation.packet.source)));
                    take(write.bindInt64(4, mutation.packet.resumeReady ? 1 : 0));
                    take(write.bindText(5, priorPacket ? *priorPacket : payload));
                    if (mutation.packet.clientId) {
                        take(write.bindText(6, mutation.packet.clientId->value()));
                    } else {
                        take(write.bindNull(6));
                    }
                    take(write.bindInt64(7, nextSequence));
                    take(write.bindText(8, payload));
                    take(write.bindText(9, contentDigest.value()));
                    take(write.bindText(10, mutation.packet.id.value()));
                    if (exists) take(write.bindInt64(11, *priorSequence));
                    stepDone(write);

                    static_cast<void>(repairPointersIn(transaction, noteTimestamp));
                    auto read = take(transaction.prepare(
                        std::string{"SELECT "} + std::string{ProjectionColumns} +
                            " FROM context_handoffs WHERE id=?"));
                    take(read.bindText(1, mutation.packet.id.value()));
                    if (take(read.step()) != WinsqliteStepResult::Row) {
                        integrity("The legacy continuity CAS result could not be reloaded.");
                    }
                    auto record = readRecord(read, *implementation_->hasher);
                    if (record.packet != mutation.packet ||
                        record.writeSequence !=
                            static_cast<std::uint64_t>(nextSequence)) {
                        integrity("The legacy continuity CAS result is inconsistent.");
                    }
                    take(transaction.commit());
                    return record;
                });
            }));
    });
}

Domain::Result<Domain::LegacyContinuityPointerRepairOutcome>
WindowsLegacyContinuityRepository::repairPointers(
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyContinuityPointerRepairOutcome>([&]() {
        if (!implementation_ || !implementation_->clock) {
            invalid("The legacy continuity repository is closed.");
        }
        const auto timestamp = timestampText(implementation_->clock->utcNow());
        auto& store = requireStore(implementation_->repositoryStore());
        return take(runOnStore<Domain::LegacyContinuityPointerRepairOutcome>(
            store, "Repair legacy continuity pointers", context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyContinuityPointerRepairOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    auto outcome = repairPointersIn(transaction, timestamp);
                    take(transaction.commit());
                    return outcome;
                });
            }));
    });
}

Domain::Result<Domain::LegacyContinuityResetOutcome>
WindowsLegacyContinuityRepository::reset(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return guarded<Domain::LegacyContinuityResetOutcome>([&]() {
        take(Domain::validateDestructiveConfirmation(
            confirmation, ResetAction, ResetScope, ResetToken));
        auto& store = requireStore(
            implementation_ ? implementation_->repositoryStore() : nullptr);
        return take(runOnStore<Domain::LegacyContinuityResetOutcome>(
            store, "Reset legacy continuity", context,
            [&](WinsqliteConnection& connection) noexcept {
                return guarded<Domain::LegacyContinuityResetOutcome>([&]() {
                    auto transaction = take(
                        WinsqliteTransaction::beginImmediate(connection, context));
                    const auto handoffs = checkedCount(scalar(
                        transaction, "SELECT COUNT(*) FROM context_handoffs"));
                    const auto pointers = checkedCount(scalar(
                        transaction,
                        "SELECT COUNT(*) FROM memory_notes WHERE "
                        "key IN ('continuity/latest','continuity/resume_ready')"));
                    take(transaction.execute("DELETE FROM context_handoffs"));
                    take(transaction.execute(
                        "DELETE FROM memory_notes WHERE "
                        "key IN ('continuity/latest','continuity/resume_ready')"));
                    const bool verified = scalar(
                        transaction,
                        "SELECT (SELECT COUNT(*) FROM context_handoffs)+"
                        "(SELECT COUNT(*) FROM memory_notes WHERE "
                        "key IN ('continuity/latest','continuity/resume_ready'))") == 0;
                    if (!verified) {
                        integrity("The transactional legacy continuity reset did not verify.");
                    }
                    take(transaction.commit());
                    return Domain::LegacyContinuityResetOutcome{
                        handoffs, pointers, 0U, true, false, true, std::nullopt};
                });
            }));
    });
}

Domain::Result<void> WindowsLegacyContinuityRepository::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_ || !implementation_->repositoryStore() ||
        !implementation_->database) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The legacy continuity repository is closed."));
    }
    return implementation_->database->quickCheck(context);
}

void WindowsLegacyContinuityRepository::close() noexcept
{
    try {
        if (!implementation_ || !implementation_->database ||
            !implementation_->clock ||
            implementation_->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (!implementation_->closeDatabaseOnClose) return;
        auto operationId = Domain::OperationId::parse(
            "ffffffff-ffff-4fff-bfff-fffffffffff3");
        auto correlationId = Domain::CorrelationId::parse(
            "legacy-continuity-repository-close");
        if (!operationId || !correlationId) return;
        const Domain::OperationContext context{
            std::move(operationId).value(),
            implementation_->clock->monotonicNow() + std::chrono::seconds{30},
            {},
            std::move(correlationId).value()};
        static_cast<void>(implementation_->database->close(context));
    } catch (...) {
        // The database and native handle wrappers retain their RAII fallback.
    }
}

} // namespace ForgeConductor::Persistence::Windows
