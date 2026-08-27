#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "Detail/OperationContextGuard.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

constexpr std::size_t MaximumJsonDepth = 64U;
constexpr std::string_view ZeroSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";

struct CodecFailure final {
    Domain::Error error;
};

[[noreturn]] void fail(Domain::Error error)
{
    throw CodecFailure{std::move(error)};
}

[[noreturn]] void invalid(const std::string_view message)
{
    fail(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        std::string{message}));
}

[[noreturn]] void integrity(const std::string_view message)
{
    fail(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        std::string{message}));
}

void requireObjectKeys(
    const Json& value,
    const std::initializer_list<std::string_view> required,
    const std::initializer_list<std::string_view> optional = {})
{
    if (!value.is_object()) {
        invalid("A continuity document section is not an object.");
    }
    const auto allowed = [required, optional](const std::string_view key) {
        return std::find(required.begin(), required.end(), key) != required.end() ||
               std::find(optional.begin(), optional.end(), key) != optional.end();
    };
    for (const auto key : required) {
        if (!value.contains(std::string{key})) {
            invalid("A continuity document is missing a required field.");
        }
    }
    for (auto item = value.cbegin(); item != value.cend(); ++item) {
        if (!allowed(item.key())) {
            invalid("A continuity document contains an unsupported field.");
        }
    }
}

[[nodiscard]] const Json& requiredField(
    const Json& object,
    const std::string_view name)
{
    const auto item = object.find(std::string{name});
    if (item == object.end()) {
        invalid("A continuity document is missing a required field.");
    }
    return *item;
}

[[nodiscard]] std::string requiredString(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredField(object, name);
    if (!value.is_string()) {
        invalid("A continuity document field has the wrong JSON type.");
    }
    return value.get<std::string>();
}

[[nodiscard]] std::optional<std::string> optionalString(
    const Json& object,
    const std::string_view name)
{
    const auto item = object.find(std::string{name});
    if (item == object.end()) {
        return std::nullopt;
    }
    if (!item->is_string()) {
        invalid("An optional continuity document field has the wrong JSON type.");
    }
    return item->get<std::string>();
}

[[nodiscard]] std::optional<std::string> requiredNullableString(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredField(object, name);
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_string()) {
        invalid("A nullable continuity document field has the wrong JSON type.");
    }
    return value.get<std::string>();
}

[[nodiscard]] std::uint32_t requiredUint32(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredField(object, name);
    std::uint64_t encoded{};
    if (value.is_number_unsigned()) {
        encoded = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue < 0) {
            invalid("A continuity document unsigned integer is negative.");
        }
        encoded = static_cast<std::uint64_t>(signedValue);
    } else {
        invalid("A continuity document integer field has the wrong JSON type.");
    }
    if (encoded > (std::numeric_limits<std::uint32_t>::max)()) {
        invalid("A continuity document integer exceeds its bounded range.");
    }
    return static_cast<std::uint32_t>(encoded);
}

[[nodiscard]] std::int32_t requiredInt32(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredField(object, name);
    std::int64_t encoded{};
    if (value.is_number_unsigned()) {
        const auto unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)())) {
            invalid("A continuity document signed integer exceeds its bounded range.");
        }
        encoded = static_cast<std::int64_t>(unsignedValue);
    } else if (value.is_number_integer()) {
        encoded = value.get<std::int64_t>();
    } else {
        invalid("A continuity document integer field has the wrong JSON type.");
    }
    if (encoded < (std::numeric_limits<std::int32_t>::min)() ||
        encoded > (std::numeric_limits<std::int32_t>::max)()) {
        invalid("A continuity document signed integer exceeds its bounded range.");
    }
    return static_cast<std::int32_t>(encoded);
}

[[nodiscard]] bool requiredBoolean(
    const Json& object,
    const std::string_view name)
{
    const auto& value = requiredField(object, name);
    if (!value.is_boolean()) {
        invalid("A continuity document Boolean field has the wrong JSON type.");
    }
    return value.get<bool>();
}

template <typename Identifier>
[[nodiscard]] Identifier parseIdentifier(
    const std::string_view value,
    const std::string_view field)
{
    auto parsed = Identifier::parse(value);
    if (!parsed) {
        invalid(std::string{"A continuity document contains an invalid "} +
                std::string{field} + '.');
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::PathText parsePath(
    const std::string_view value,
    const std::string_view field)
{
    auto parsed = Domain::PathText::create(value);
    if (!parsed || !Domain::isValidUtf8(value)) {
        invalid(std::string{"A continuity document contains an invalid "} +
                std::string{field} + '.');
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::Sha256Digest parseDigest(const std::string_view value)
{
    auto parsed = Domain::Sha256Digest::parse(value);
    if (!parsed) {
        integrity("The continuity document SHA-256 representation is invalid.");
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
        invalid("A continuity timestamp is not canonical UTC.");
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
        invalid("A continuity timestamp is invalid.");
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
        invalid("A continuity timestamp is outside the supported UTC range.");
    }
    return Domain::UtcTimePoint{std::chrono::seconds{encoded}} +
           std::chrono::milliseconds{*millisecond};
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto elapsed = timestamp.time_since_epoch();
    const auto totalMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    if (totalMilliseconds.count() < 0) {
        invalid("A continuity timestamp predates the supported UTC range.");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(totalMilliseconds);
    const auto milliseconds = static_cast<int>(
        (totalMilliseconds - seconds).count());
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0) {
        invalid("A continuity timestamp is outside the supported UTC range.");
    }
    std::array<char, 25U> buffer{};
    const int written = milliseconds == 0
        ? std::snprintf(
              buffer.data(),
              buffer.size(),
              "%04d-%02d-%02dT%02d:%02d:%02dZ",
              utc.tm_year + 1900,
              utc.tm_mon + 1,
              utc.tm_mday,
              utc.tm_hour,
              utc.tm_min,
              utc.tm_sec)
        : std::snprintf(
              buffer.data(),
              buffer.size(),
              "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              utc.tm_year + 1900,
              utc.tm_mon + 1,
              utc.tm_mday,
              utc.tm_hour,
              utc.tm_min,
              utc.tm_sec,
              milliseconds);
    const int expected = milliseconds == 0 ? 20 : 24;
    if (written != expected) {
        invalid("A continuity timestamp could not be formatted.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

void validateJsonTree(const Json& value, const std::size_t depth)
{
    if (depth > MaximumJsonDepth) {
        invalid("A continuity document exceeds the maximum JSON depth.");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text)) {
            invalid("A continuity document contains invalid UTF-8 text.");
        }
        return;
    }
    if (value.is_object()) {
        for (auto item = value.cbegin(); item != value.cend(); ++item) {
            if (item.key().find('\0') != std::string::npos ||
                !Domain::isValidUtf8(item.key())) {
                invalid("A continuity document contains an invalid object key.");
            }
            validateJsonTree(*item, depth + 1U);
        }
        return;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            validateJsonTree(item, depth + 1U);
        }
    }
}

[[nodiscard]] std::string canonicalJson(const Json& value)
{
    validateJsonTree(value, 0U);
    return value.dump(-1, ' ', false, Json::error_handler_t::strict);
}

[[nodiscard]] Json parseCanonicalJson(const std::string_view encoded)
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
        invalid("A continuity document is not one bounded JSON object.");
    }
    validateJsonTree(document, 0U);
    if (canonicalJson(document) != encoded) {
        invalid("A continuity document is not canonical compact sorted-key JSON.");
    }
    return document;
}

[[nodiscard]] std::vector<std::string> decodeStringArray(
    const Json& value,
    const std::string_view field)
{
    if (!value.is_array() ||
        value.size() > Domain::MaximumContinuityHandoffListItems) {
        invalid(std::string{"Continuity "} + std::string{field} +
                " must be a bounded string array.");
    }
    std::vector<std::string> result;
    result.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            invalid("A continuity string array contains a non-string value.");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

[[nodiscard]] Json encodeSession(const Domain::ContinuitySession& session)
{
    Json value = Json::object();
    if (session.model) {
        value["model"] = *session.model;
    } else {
        value["model"] = nullptr;
    }
    if (session.providerSessionId) {
        value["provider_session_id"] = session.providerSessionId->value();
    } else {
        value["provider_session_id"] = nullptr;
    }
    value["session_id"] = session.sessionId.value();
    if (session.provider) {
        value["provider"] = *session.provider;
    }
    return value;
}

[[nodiscard]] Domain::ContinuitySession decodeSession(const Json& value)
{
    requireObjectKeys(
        value,
        {"session_id", "provider_session_id", "model"},
        {"provider"});
    const auto providerSession = requiredNullableString(
        value, "provider_session_id");
    std::optional<Domain::ProviderSessionId> providerSessionId;
    if (providerSession) {
        providerSessionId = parseIdentifier<Domain::ProviderSessionId>(
            *providerSession, "provider session identifier");
    }
    return Domain::ContinuitySession{
        parseIdentifier<Domain::SessionId>(
            requiredString(value, "session_id"), "session identifier"),
        std::move(providerSessionId),
        requiredNullableString(value, "model"),
        optionalString(value, "provider")};
}

[[nodiscard]] Json encodeWorkEntry(const Domain::ContinuityWorkEntry& entry)
{
    Json value = Json::object();
    value["summary"] = entry.summary;
    if (entry.workItemId) {
        value["work_item_id"] = *entry.workItemId;
    }
    if (entry.status) {
        value["status"] = *entry.status;
    }
    return value;
}

[[nodiscard]] Domain::ContinuityWorkEntry decodeWorkEntry(const Json& value)
{
    requireObjectKeys(value, {"summary"}, {"work_item_id", "status"});
    return Domain::ContinuityWorkEntry{
        optionalString(value, "work_item_id"),
        requiredString(value, "summary"),
        optionalString(value, "status")};
}

[[nodiscard]] Json encodeDecision(const Domain::ContinuityDecision& decision)
{
    Json value = Json::object();
    value["decision"] = decision.decision;
    if (decision.rationale) {
        value["rationale"] = *decision.rationale;
    }
    return value;
}

[[nodiscard]] Domain::ContinuityDecision decodeDecision(const Json& value)
{
    requireObjectKeys(value, {"decision"}, {"rationale"});
    return Domain::ContinuityDecision{
        requiredString(value, "decision"),
        optionalString(value, "rationale")};
}

[[nodiscard]] Json encodeCommandEvidence(
    const Domain::ContinuityCommandEvidence& evidence)
{
    Json value = Json::object();
    value["command"] = evidence.command;
    value["exit_code"] = evidence.exitCode;
    if (evidence.evidenceId) {
        value["evidence_id"] = evidence.evidenceId->value();
    }
    return value;
}

[[nodiscard]] Domain::ContinuityCommandEvidence decodeCommandEvidence(
    const Json& value)
{
    requireObjectKeys(value, {"command", "exit_code"}, {"evidence_id"});
    std::optional<Domain::EvidenceId> evidenceId;
    if (const auto encoded = optionalString(value, "evidence_id")) {
        evidenceId = parseIdentifier<Domain::EvidenceId>(
            *encoded, "command evidence identifier");
    }
    return Domain::ContinuityCommandEvidence{
        requiredString(value, "command"),
        requiredInt32(value, "exit_code"),
        std::move(evidenceId)};
}

[[nodiscard]] std::string stateText(
    const Domain::ContinuityState state,
    const std::optional<std::string>& persisted)
{
    if (persisted) {
        const auto parsed = Domain::parseContinuityStateWireName(*persisted);
        if (!parsed || parsed.value() != state) {
            integrity("A persisted continuity state spelling does not match its typed value.");
        }
        return *persisted;
    }
    return std::string{Domain::wireName(state)};
}

[[nodiscard]] Json handoffJson(const Domain::ContinuityHandoff& handoff)
{
    Json project = Json::object();
    project["branch"] = handoff.project.branch;
    project["commit"] = handoff.project.commit;
    project["dirty_summary"] = handoff.project.dirtySummary;
    project["display_name"] = handoff.project.displayName;
    project["project_id"] = handoff.project.projectId.value();
    project["repository_root"] = handoff.project.repositoryRoot.value();

    Json currentWork = Json::object();
    currentWork["active_files"] = Json::array();
    for (const auto& path : handoff.currentWork.activeFiles) {
        currentWork["active_files"].push_back(path.value());
    }
    currentWork["phase_id"] = handoff.currentWork.phaseId;
    currentWork["summary"] = handoff.currentWork.summary;
    currentWork["work_item_id"] = handoff.currentWork.workItemId;

    Json completedWork = Json::array();
    for (const auto& entry : handoff.completedWork) {
        completedWork.push_back(encodeWorkEntry(entry));
    }
    Json openWork = Json::array();
    for (const auto& entry : handoff.openWork) {
        openWork.push_back(encodeWorkEntry(entry));
    }
    Json decisions = Json::array();
    for (const auto& decision : handoff.decisions) {
        decisions.push_back(encodeDecision(decision));
    }

    Json validation = Json::object();
    validation["commands"] = Json::array();
    for (const auto& evidence : handoff.validation.commands) {
        validation["commands"].push_back(encodeCommandEvidence(evidence));
    }
    validation["open_gates"] = handoff.validation.openGates;
    validation["passed_gates"] = handoff.validation.passedGates;

    Json memoryReferences = Json::array();
    for (const auto& reference : handoff.memoryReferences) {
        memoryReferences.push_back(Json{{"record_id", reference.value()}});
    }
    Json evidenceReferences = Json::array();
    for (const auto& reference : handoff.evidenceReferences) {
        if (reference.evidenceId.has_value() == reference.path.has_value()) {
            invalid("A continuity evidence reference is ambiguous or empty.");
        }
        evidenceReferences.push_back(reference.evidenceId
            ? Json{{"evidence_id", reference.evidenceId->value()}}
            : Json{{"path", reference.path->value()}});
    }

    Json nextActions = Json::array();
    for (const auto& action : handoff.nextActions) {
        nextActions.push_back(Json{
            {"action", action.action},
            {"command", action.command},
            {"order", action.order},
            {"success_condition", action.successCondition}});
    }

    Json retry = Json::object();
    retry["attempt"] = handoff.hostState.retry.attempt;
    if (handoff.hostState.retry.lastError) {
        retry["last_error"] = *handoff.hostState.retry.lastError;
    }
    if (handoff.hostState.retry.retryAt) {
        retry["retry_at"] = timestampText(*handoff.hostState.retry.retryAt);
    }
    if (handoff.hostState.retry.retryResumeState) {
        retry["retry_resume_state"] = stateText(
            *handoff.hostState.retry.retryResumeState,
            handoff.hostState.retry.persistedRetryResumeStateName);
    }
    Json hostState = Json::object();
    hostState["adapter_id"] = handoff.hostState.adapterId.value();
    hostState["context_budget_source"] = handoff.hostState.contextBudgetSource;
    hostState["continuity_state"] = stateText(
        handoff.hostState.continuityState,
        handoff.hostState.persistedContinuityStateName);
    if (handoff.hostState.remainingBudgetEstimate) {
        hostState["remaining_budget_estimate"] =
            *handoff.hostState.remainingBudgetEstimate;
    }
    hostState["retry"] = std::move(retry);

    Json integritySection = Json::object();
    integritySection["content_sha256"] = handoff.contentSha256.value();
    integritySection["redaction_complete"] = handoff.redactionComplete;

    Json root = Json::object();
    root["completed_work"] = std::move(completedWork);
    root["constraints"] = handoff.constraints;
    root["created_at"] = timestampText(handoff.createdAt);
    root["current_work"] = std::move(currentWork);
    root["decisions"] = std::move(decisions);
    root["evidence_references"] = std::move(evidenceReferences);
    root["handoff_id"] = handoff.handoffId.value();
    root["host_state"] = std::move(hostState);
    root["integrity"] = std::move(integritySection);
    root["memory_references"] = std::move(memoryReferences);
    root["mission"] = handoff.mission;
    root["next_actions"] = std::move(nextActions);
    root["open_work"] = std::move(openWork);
    root["operation_id"] = handoff.operationId.value();
    root["predecessor_session"] = encodeSession(handoff.predecessorSession);
    root["project"] = std::move(project);
    root["schema_version"] = std::string{
        Domain::ContinuityHandoffSchemaVersion};
    if (handoff.successorSession) {
        root["successor_session"] = encodeSession(*handoff.successorSession);
    } else {
        root["successor_session"] = nullptr;
    }
    root["validation"] = std::move(validation);
    return root;
}

[[nodiscard]] Domain::ContinuityHandoff decodeHandoff(const Json& root)
{
    requireObjectKeys(
        root,
        {"schema_version", "handoff_id", "operation_id", "created_at",
         "project", "predecessor_session", "successor_session", "mission",
         "constraints", "current_work", "completed_work", "open_work",
         "decisions", "validation", "memory_references",
         "evidence_references", "next_actions", "host_state", "integrity"});
    if (requiredString(root, "schema_version") !=
        Domain::ContinuityHandoffSchemaVersion) {
        fail(Domain::makeError(
            Domain::ErrorCodes::UnsupportedVersion,
            "The continuity document schema version is unsupported."));
    }

    const auto& projectJson = requiredField(root, "project");
    requireObjectKeys(
        projectJson,
        {"project_id", "display_name", "repository_root", "branch", "commit",
         "dirty_summary"});
    Domain::ContinuityProject project{
        parseIdentifier<Domain::ProjectId>(
            requiredString(projectJson, "project_id"), "project identifier"),
        requiredString(projectJson, "display_name"),
        parsePath(
            requiredString(projectJson, "repository_root"), "repository root"),
        requiredString(projectJson, "branch"),
        requiredString(projectJson, "commit"),
        decodeStringArray(
            requiredField(projectJson, "dirty_summary"), "dirty summary")};

    const auto& currentJson = requiredField(root, "current_work");
    requireObjectKeys(
        currentJson,
        {"phase_id", "work_item_id", "summary", "active_files"});
    const auto activeStrings = decodeStringArray(
        requiredField(currentJson, "active_files"), "active files");
    std::vector<Domain::PathText> activeFiles;
    activeFiles.reserve(activeStrings.size());
    for (const auto& path : activeStrings) {
        activeFiles.push_back(parsePath(path, "active file"));
    }
    Domain::ContinuityCurrentWork currentWork{
        requiredString(currentJson, "phase_id"),
        requiredString(currentJson, "work_item_id"),
        requiredString(currentJson, "summary"),
        std::move(activeFiles)};

    const auto decodeEntries = [](const Json& value) {
        if (!value.is_array() ||
            value.size() > Domain::MaximumContinuityHandoffListItems) {
            invalid("A continuity work list is not a bounded array.");
        }
        std::vector<Domain::ContinuityWorkEntry> entries;
        entries.reserve(value.size());
        for (const auto& item : value) {
            entries.push_back(decodeWorkEntry(item));
        }
        return entries;
    };
    const auto decodeDecisions = [](const Json& value) {
        if (!value.is_array() ||
            value.size() > Domain::MaximumContinuityHandoffListItems) {
            invalid("A continuity decision list is not a bounded array.");
        }
        std::vector<Domain::ContinuityDecision> decisions;
        decisions.reserve(value.size());
        for (const auto& item : value) {
            decisions.push_back(decodeDecision(item));
        }
        return decisions;
    };

    const auto& validationJson = requiredField(root, "validation");
    requireObjectKeys(
        validationJson, {"passed_gates", "open_gates", "commands"});
    const auto& commandsJson = requiredField(validationJson, "commands");
    if (!commandsJson.is_array() ||
        commandsJson.size() > Domain::MaximumContinuityHandoffListItems) {
        invalid("Continuity validation commands are not a bounded array.");
    }
    std::vector<Domain::ContinuityCommandEvidence> commands;
    commands.reserve(commandsJson.size());
    for (const auto& item : commandsJson) {
        commands.push_back(decodeCommandEvidence(item));
    }
    Domain::ContinuityValidation validation{
        decodeStringArray(
            requiredField(validationJson, "passed_gates"), "passed gates"),
        decodeStringArray(
            requiredField(validationJson, "open_gates"), "open gates"),
        std::move(commands)};

    const auto& memoryJson = requiredField(root, "memory_references");
    if (!memoryJson.is_array() ||
        memoryJson.size() > Domain::MaximumContinuityHandoffListItems) {
        invalid("Continuity memory references are not a bounded array.");
    }
    std::vector<Domain::MemoryRecordId> memoryReferences;
    memoryReferences.reserve(memoryJson.size());
    for (const auto& item : memoryJson) {
        requireObjectKeys(item, {"record_id"});
        memoryReferences.push_back(parseIdentifier<Domain::MemoryRecordId>(
            requiredString(item, "record_id"), "memory record identifier"));
    }

    const auto& evidenceJson = requiredField(root, "evidence_references");
    if (!evidenceJson.is_array() ||
        evidenceJson.size() > Domain::MaximumContinuityHandoffListItems) {
        invalid("Continuity evidence references are not a bounded array.");
    }
    std::vector<Domain::ContinuityEvidenceReference> evidenceReferences;
    evidenceReferences.reserve(evidenceJson.size());
    for (const auto& item : evidenceJson) {
        if (!item.is_object() || item.size() != 1U) {
            invalid("A continuity evidence reference has the wrong shape.");
        }
        if (item.contains("evidence_id")) {
            evidenceReferences.push_back(Domain::ContinuityEvidenceReference{
                parseIdentifier<Domain::EvidenceId>(
                    requiredString(item, "evidence_id"), "evidence identifier"),
                std::nullopt});
        } else if (item.contains("path")) {
            evidenceReferences.push_back(Domain::ContinuityEvidenceReference{
                std::nullopt,
                parsePath(requiredString(item, "path"), "evidence path")});
        } else {
            invalid("A continuity evidence reference has an unsupported field.");
        }
    }

    const auto& actionsJson = requiredField(root, "next_actions");
    if (!actionsJson.is_array() || actionsJson.empty() ||
        actionsJson.size() > Domain::MaximumContinuityHandoffListItems) {
        invalid("Continuity next actions are not one non-empty bounded array.");
    }
    std::vector<Domain::ContinuityNextAction> nextActions;
    nextActions.reserve(actionsJson.size());
    for (const auto& item : actionsJson) {
        requireObjectKeys(
            item, {"order", "action", "command", "success_condition"});
        nextActions.emplace_back(
            requiredUint32(item, "order"),
            requiredString(item, "action"),
            requiredString(item, "command"),
            requiredString(item, "success_condition"));
    }

    const auto& hostJson = requiredField(root, "host_state");
    requireObjectKeys(
        hostJson,
        {"adapter_id", "continuity_state", "context_budget_source", "retry"},
        {"remaining_budget_estimate"});
    const auto stateName = requiredString(hostJson, "continuity_state");
    auto parsedState = Domain::parseContinuityStateWireName(stateName);
    if (!parsedState) {
        invalid("The continuity host state is unsupported.");
    }
    const auto& retryJson = requiredField(hostJson, "retry");
    requireObjectKeys(
        retryJson, {"attempt"},
        {"last_error", "retry_at", "retry_resume_state"});
    std::optional<Domain::UtcTimePoint> retryAt;
    if (const auto encoded = optionalString(retryJson, "retry_at")) {
        retryAt = parseTimestamp(*encoded);
    }
    std::optional<Domain::ContinuityState> retryResumeState;
    std::optional<std::string> retryResumeStateName;
    if (const auto encoded = optionalString(retryJson, "retry_resume_state")) {
        auto parsed = Domain::parseContinuityStateWireName(*encoded);
        if (!parsed || !Domain::isRetryResumeState(parsed.value())) {
            invalid("The continuity retry-resume state is unsupported.");
        }
        retryResumeState = parsed.value();
        retryResumeStateName = *encoded;
    }
    std::optional<double> remainingBudgetEstimate;
    const auto remainingBudget = hostJson.find("remaining_budget_estimate");
    if (remainingBudget != hostJson.end()) {
        if (!remainingBudget->is_number()) {
            invalid("The continuity remaining budget estimate is not numeric.");
        }
        const auto parsed = remainingBudget->get<double>();
        if (!std::isfinite(parsed)) {
            invalid("The continuity remaining budget estimate is not finite.");
        }
        remainingBudgetEstimate = parsed;
    }
    Domain::ContinuityHostState hostState{
        parseIdentifier<Domain::AdapterId>(
            requiredString(hostJson, "adapter_id"), "adapter identifier"),
        parsedState.value(),
        requiredString(hostJson, "context_budget_source"),
        Domain::ContinuityRetryState{
            requiredUint32(retryJson, "attempt"),
            optionalString(retryJson, "last_error"),
            retryAt,
            retryResumeState,
            retryResumeStateName},
        stateName,
        remainingBudgetEstimate};

    const auto& integrityJson = requiredField(root, "integrity");
    requireObjectKeys(
        integrityJson, {"content_sha256", "redaction_complete"});
    const auto digest = parseDigest(requiredString(
        integrityJson, "content_sha256"));

    std::optional<Domain::ContinuitySession> successor;
    const auto& successorJson = requiredField(root, "successor_session");
    if (!successorJson.is_null()) {
        successor = decodeSession(successorJson);
    }

    return Domain::ContinuityHandoff{
        parseIdentifier<Domain::ContinuityHandoffId>(
            requiredString(root, "handoff_id"), "handoff identifier"),
        parseIdentifier<Domain::ContinuityOperationId>(
            requiredString(root, "operation_id"), "operation identifier"),
        parseTimestamp(requiredString(root, "created_at")),
        std::move(project),
        decodeSession(requiredField(root, "predecessor_session")),
        std::move(successor),
        requiredString(root, "mission"),
        decodeStringArray(requiredField(root, "constraints"), "constraints"),
        std::move(currentWork),
        decodeEntries(requiredField(root, "completed_work")),
        decodeEntries(requiredField(root, "open_work")),
        decodeDecisions(requiredField(root, "decisions")),
        std::move(validation),
        std::move(memoryReferences),
        std::move(evidenceReferences),
        std::move(nextActions),
        std::move(hostState),
        digest,
        requiredBoolean(integrityJson, "redaction_complete")};
}

[[nodiscard]] Domain::Result<void> validateDependenciesAndContext(
    const std::shared_ptr<Contracts::IHasher>& hasher,
    const std::shared_ptr<Contracts::IClock>& clock,
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    if (!hasher || !clock) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The continuity document codec is missing a required dependency."));
    }
    return Detail::validateOperationContext(
        context, clock->monotonicNow(), action);
}

[[nodiscard]] Domain::Sha256Digest hashJson(
    Contracts::IHasher& hasher,
    Json document)
{
    document["integrity"]["content_sha256"] = ZeroSha256;
    const auto hashInput = canonicalJson(document);
    auto digest = hasher.sha256(std::as_bytes(std::span<const char>{
        hashInput.data(), hashInput.size()}));
    if (!digest) {
        fail(std::move(digest).error());
    }
    return std::move(digest).value();
}

} // namespace

WindowsContinuityDocumentCodec::WindowsContinuityDocumentCodec(
    std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IClock> clock) noexcept
    : hasher_{std::move(hasher)}, clock_{std::move(clock)}
{
}

Domain::Result<Contracts::ContinuityDocument>
WindowsContinuityDocumentCodec::encode(
    const Domain::ContinuityHandoff& handoff,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto valid = validateDependenciesAndContext(
            hasher_, clock_, context, "encode the continuity handoff");
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }
        valid = Domain::validateContinuityHandoff(handoff, 0U);
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }

        auto finalized = handoff;
        auto unhashed = handoffJson(finalized);
        finalized.contentSha256 = hashJson(*hasher_, std::move(unhashed));
        auto finalJson = handoffJson(finalized);
        auto encoded = canonicalJson(finalJson);
        valid = Domain::validateContinuityHandoff(finalized, encoded.size());
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }
        valid = validateDependenciesAndContext(
            hasher_, clock_, context, "finish encoding the continuity handoff");
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }
        return Domain::Result<Contracts::ContinuityDocument>::success(
            Contracts::ContinuityDocument{
                std::move(finalized), std::move(encoded)});
    } catch (const CodecFailure& failure) {
        return Domain::Result<Contracts::ContinuityDocument>::failure(
            failure.error);
    } catch (...) {
        return Domain::Result<Contracts::ContinuityDocument>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The continuity handoff could not be encoded."));
    }
}

Domain::Result<Contracts::ContinuityDocument>
WindowsContinuityDocumentCodec::decode(
    const std::string_view encoded,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto valid = validateDependenciesAndContext(
            hasher_, clock_, context, "decode the continuity handoff");
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }
        if (encoded.empty() ||
            encoded.size() > Domain::MaximumContinuityHandoffEncodedBytes) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                Domain::makeError(
                    encoded.empty() ? Domain::ErrorCodes::InvalidRequest
                                    : Domain::ErrorCodes::PayloadTooLarge,
                    "The continuity handoff is empty or exceeds 131072 bytes."));
        }
        if (encoded.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(encoded)) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The continuity handoff is not valid NUL-free UTF-8."));
        }

        auto json = parseCanonicalJson(encoded);
        const auto handoff = decodeHandoff(json);
        const auto expectedDigest = hashJson(*hasher_, json);
        if (expectedDigest != handoff.contentSha256) {
            integrity("The continuity handoff content SHA-256 does not match its canonical bytes.");
        }
        valid = Domain::validateContinuityHandoff(handoff, encoded.size());
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }
        const auto reconstructed = canonicalJson(handoffJson(handoff));
        if (reconstructed != encoded) {
            const auto mismatch = std::mismatch(
                reconstructed.begin(),
                reconstructed.end(),
                encoded.begin(),
                encoded.end());
            const auto offset = static_cast<std::size_t>(
                std::distance(reconstructed.begin(), mismatch.first));
            integrity(
                "The typed continuity handoff cannot reproduce its exact canonical bytes at byte " +
                std::to_string(offset) + " (typed=" +
                std::to_string(offset < reconstructed.size()
                    ? static_cast<unsigned char>(reconstructed[offset])
                    : 256U) + ", stored=" +
                std::to_string(offset < encoded.size()
                    ? static_cast<unsigned char>(encoded[offset])
                    : 256U) + ").");
        }
        valid = validateDependenciesAndContext(
            hasher_, clock_, context, "finish decoding the continuity handoff");
        if (!valid) {
            return Domain::Result<Contracts::ContinuityDocument>::failure(
                std::move(valid).error());
        }
        return Domain::Result<Contracts::ContinuityDocument>::success(
            Contracts::ContinuityDocument{
                handoff, std::string{encoded}});
    } catch (const CodecFailure& failure) {
        return Domain::Result<Contracts::ContinuityDocument>::failure(
            failure.error);
    } catch (...) {
        return Domain::Result<Contracts::ContinuityDocument>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The continuity handoff could not be decoded."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
