#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] Result<void> invalidAgentValue(const std::string_view message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::InvalidRequest, std::string{message}));
}

[[nodiscard]] Result<void> payloadTooLarge(const std::string_view message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::PayloadTooLarge, std::string{message}));
}

[[nodiscard]] Result<void> validateText(
    const std::string_view value,
    const std::size_t maximumBytes,
    const bool allowEmpty,
    const std::string_view field) noexcept
{
    try {
        if ((!allowEmpty && value.empty()) ||
            value.find('\0') != std::string_view::npos || !isValidUtf8(value)) {
            return invalidAgentValue(
                std::string{field} + " is empty, contains NUL, or is not valid UTF-8.");
        }
        if (value.size() > maximumBytes) {
            return payloadTooLarge(
                std::string{field} + " exceeds its UTF-8 byte limit.");
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The agent text boundary could not be validated."));
    }
}

[[nodiscard]] Result<void> validateItems(
    const std::vector<std::string>& values,
    const std::size_t maximumItems,
    const std::string_view field) noexcept
{
    try {
        if (values.size() > maximumItems) {
            return payloadTooLarge(
                std::string{field} + " exceeds its item limit.");
        }
        for (const auto& value : values) {
            auto valid = validateText(
                value,
                AgentSessionLimits::MaximumItemBytes,
                false,
                field);
            if (!valid) {
                return valid;
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The agent item collection could not be validated."));
    }
}

void appendJsonString(std::string& output, const std::string_view value)
{
    constexpr char Hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(Hex[(character >> 4U) & 0x0fU]);
                output.push_back(Hex[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::size_t utf8SequenceBytes(
    const unsigned char first) noexcept
{
    if (first < 0x80U) {
        return 1U;
    }
    if ((first & 0xe0U) == 0xc0U) {
        return 2U;
    }
    if ((first & 0xf0U) == 0xe0U) {
        return 3U;
    }
    if ((first & 0xf8U) == 0xf0U) {
        return 4U;
    }
    return 0U;
}

} // namespace

bool isOpen(const SessionStatus status) noexcept
{
    switch (status) {
    case SessionStatus::Open:
    case SessionStatus::Active:
    case SessionStatus::Running:
    case SessionStatus::Started:
        return true;
    case SessionStatus::Closed:
    case SessionStatus::Completed:
    case SessionStatus::Failed:
        return false;
    }
    return false;
}

std::string_view wireName(const SessionStatus status) noexcept
{
    switch (status) {
    case SessionStatus::Open: return "open";
    case SessionStatus::Active: return "active";
    case SessionStatus::Running: return "running";
    case SessionStatus::Started: return "started";
    case SessionStatus::Closed: return "closed";
    case SessionStatus::Completed: return "completed";
    case SessionStatus::Failed: return "failed";
    }
    return "failed";
}

Result<SessionStatus> sessionStatusFromWire(const std::string_view value) noexcept
{
    if (value == "open") return Result<SessionStatus>::success(SessionStatus::Open);
    if (value == "active") return Result<SessionStatus>::success(SessionStatus::Active);
    if (value == "running") return Result<SessionStatus>::success(SessionStatus::Running);
    if (value == "started") return Result<SessionStatus>::success(SessionStatus::Started);
    if (value == "closed") return Result<SessionStatus>::success(SessionStatus::Closed);
    if (value == "completed") return Result<SessionStatus>::success(SessionStatus::Completed);
    if (value == "failed") return Result<SessionStatus>::success(SessionStatus::Failed);
    return Result<SessionStatus>::failure(makeError(
        ErrorCodes::IntegrityFailure,
        "The durable agent session contains an unknown status."));
}

bool isMissingAgentReportField(const AgentReportField& field) noexcept
{
    return (field.kind == AgentReportValueKind::String ||
            field.kind == AgentReportValueKind::Array ||
            field.kind == AgentReportValueKind::Object) &&
        field.logicalSize == 0U;
}

Result<void> validateAgentRunStartRequest(
    const AgentRunStartRequest& request) noexcept
{
    return validateText(
        request.goal,
        AgentSessionLimits::MaximumGoalBytes,
        true,
        "Agent goal");
}

Result<void> validateAgentCompletionReport(
    const AgentCompletionReport& report) noexcept
{
    try {
        auto valid = validateText(
            report.canonicalJson,
            AgentSessionLimits::MaximumReportJsonBytes,
            false,
            "Agent completion report");
        if (!valid) {
            return valid;
        }
        if (report.canonicalJson.front() != '{' ||
            report.canonicalJson.back() != '}') {
            return invalidAgentValue(
                "Agent completion report must be a canonical JSON object.");
        }
        if (report.fields.size() > AgentSessionLimits::MaximumReportFields) {
            return payloadTooLarge(
                "Agent completion report exceeds its field limit.");
        }
        std::set<std::string> keys;
        for (const auto& field : report.fields) {
            valid = validateText(
                field.key,
                AgentSessionLimits::MaximumItemBytes,
                false,
                "Agent completion report key");
            if (!valid) {
                return valid;
            }
            if (!keys.insert(field.key).second) {
                return invalidAgentValue(
                    "Agent completion report contains a duplicate key.");
            }
            if (field.logicalSize > AgentSessionLimits::MaximumReportJsonBytes) {
                return payloadTooLarge(
                    "Agent completion report field exceeds its logical-size limit.");
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The agent completion report could not be validated."));
    }
}

Result<void> validateAgentRunRecord(const AgentRunRecord& run) noexcept
{
    Result<void> valid = Result<void>::success();
    if (run.goal) {
        valid = validateText(
            *run.goal,
            AgentSessionLimits::MaximumGoalBytes,
            true,
            "Agent goal");
        if (!valid) {
            return valid;
        }
    }
    valid = validateItems(
        run.outputSchema,
        AgentSessionLimits::MaximumSchemaItems,
        "Agent output schema");
    if (!valid) {
        return valid;
    }
    valid = validateItems(
        run.firstMoves,
        AgentSessionLimits::MaximumBindingItems,
        "Agent first moves");
    if (!valid) {
        return valid;
    }
    if (run.reportJson) {
        valid = validateText(
            *run.reportJson,
            AgentSessionLimits::MaximumReportJsonBytes,
            false,
            "Agent report JSON");
        if (!valid) {
            return valid;
        }
    }
    return Result<void>::success();
}

Result<void> validateActiveBinding(const ActiveBinding& binding) noexcept
{
    auto valid = validateText(
        binding.goal,
        AgentSessionLimits::MaximumGoalBytes,
        true,
        "Agent binding goal");
    if (!valid) {
        return valid;
    }
    for (const auto* const values : {
             &binding.toolsPrimary,
             &binding.toolsForbidden,
             &binding.outputSchema,
             &binding.doneDefinition}) {
        valid = validateItems(
            *values,
            AgentSessionLimits::MaximumBindingItems,
            "Agent binding collection");
        if (!valid) {
            return valid;
        }
    }
    return Result<void>::success();
}

Result<void> validateAgentRunReattachRequest(
    const AgentRunReattachRequest&) noexcept
{
    return Result<void>::success();
}

Result<void> validateAgentRunRecoveryRequest(
    const AgentRunRecoveryRequest&) noexcept
{
    return Result<void>::success();
}

std::string makeAgentSupersedeSummary(
    const std::string_view eventMessage,
    const std::optional<AgentId>& newAgentId,
    const std::optional<SessionId>& reattachedSessionId)
{
    std::string result{"{\"event\":\"superseded\",\"message\":"};
    appendJsonString(result, eventMessage);
    result += ",\"ok_to_reuse\":true";
    if (newAgentId) {
        result += ",\"new_agent_id\":";
        appendJsonString(result, newAgentId->value());
    }
    if (reattachedSessionId) {
        result += ",\"reattached_session_id\":";
        appendJsonString(result, reattachedSessionId->value());
    }
    result.push_back('}');
    return result;
}

std::string makeAgentStaleSummary(const std::chrono::seconds age)
{
    return "{\"age_sec\":" + std::to_string(age.count()) +
        ",\"event\":\"auto_closed_stale\",\"message\":\"Session abandoned "
        "without agent_run_complete (idle " + std::to_string(age.count()) +
        "s).\",\"ok_to_reuse\":true}";
}

Result<std::string> makeAgentCompletionSummary(
    const std::string_view goal,
    const AgentCompletionReport& report,
    const std::vector<std::string>& missingSchemaKeys) noexcept
{
    try {
        auto valid = validateText(
            goal,
            AgentSessionLimits::MaximumGoalBytes,
            true,
            "Agent goal");
        if (!valid) {
            return Result<std::string>::failure(std::move(valid).error());
        }
        valid = validateAgentCompletionReport(report);
        if (!valid) {
            return Result<std::string>::failure(std::move(valid).error());
        }
        valid = validateItems(
            missingSchemaKeys,
            AgentSessionLimits::MaximumSchemaItems,
            "Missing agent schema keys");
        if (!valid) {
            return Result<std::string>::failure(std::move(valid).error());
        }

        std::string summary{"{\"goal\":"};
        appendJsonString(summary, goal);
        summary += ",\"missing_schema_keys\":[";
        for (std::size_t index{}; index < missingSchemaKeys.size(); ++index) {
            if (index != 0U) {
                summary.push_back(',');
            }
            appendJsonString(summary, missingSchemaKeys[index]);
        }
        summary += "],\"report\":";
        summary += report.canonicalJson;
        summary.push_back('}');
        return Result<std::string>::success(truncateAgentSummaryUtf8(summary));
    } catch (...) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The agent completion summary could not be constructed."));
    }
}

std::string truncateAgentSummaryUtf8(
    const std::string_view value,
    const std::size_t maximumUnits) noexcept
{
    try {
        if (maximumUnits == 0U || value.empty()) {
            return {};
        }
        std::size_t offset{};
        std::size_t units{};
        while (offset < value.size() && units < maximumUnits) {
            const auto sequenceBytes = utf8SequenceBytes(
                static_cast<unsigned char>(value[offset]));
            if (sequenceBytes == 0U || offset + sequenceBytes > value.size()) {
                break;
            }
            bool continuationValid{true};
            for (std::size_t index{1U}; index < sequenceBytes; ++index) {
                if ((static_cast<unsigned char>(value[offset + index]) & 0xc0U) !=
                    0x80U) {
                    continuationValid = false;
                    break;
                }
            }
            if (!continuationValid) {
                break;
            }
            offset += sequenceBytes;
            ++units;
        }
        return std::string{value.substr(0U, offset)};
    } catch (...) {
        return {};
    }
}

} // namespace ForgeConductor::Domain
