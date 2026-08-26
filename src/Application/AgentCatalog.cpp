#include "ForgeConductor/Application/AgentCatalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

using StringList = std::initializer_list<std::string_view>;

[[nodiscard]] Domain::Error catalogError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Contracts::IClock& clock,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(catalogError(
                Domain::ErrorCodes::Cancelled,
                "The agent-catalog operation was cancelled."));
        }
        if (context.isExpired(clock.monotonicNow())) {
            return Domain::Result<void>::failure(catalogError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The agent-catalog operation deadline expired."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(catalogError(
            Domain::ErrorCodes::InternalFailure,
            "The agent-catalog operation context could not be validated."));
    }
}

[[nodiscard]] bool decodeUtf8Scalar(
    const std::string_view value,
    std::size_t& offset,
    std::uint32_t& scalar) noexcept
{
    if (offset >= value.size()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first <= 0x7FU) {
        scalar = first;
        ++offset;
        return true;
    }

    std::size_t length{};
    std::uint32_t minimum{};
    if (first >= 0xC2U && first <= 0xDFU) {
        length = 2U;
        scalar = first & 0x1FU;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3U;
        scalar = first & 0x0FU;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4U;
        scalar = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return false;
    }
    if (length > value.size() - offset) {
        return false;
    }
    for (std::size_t index = 1U; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(value[offset + index]);
        if ((continuation & 0xC0U) != 0x80U) {
            return false;
        }
        scalar = (scalar << 6U) | (continuation & 0x3FU);
    }
    if (scalar < minimum || scalar > 0x10FFFFU ||
        (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
        return false;
    }
    offset += length;
    return true;
}

[[nodiscard]] bool isValidText(
    const std::string_view value,
    const bool allowLayoutWhitespace) noexcept
{
    std::size_t offset{};
    while (offset < value.size()) {
        std::uint32_t scalar{};
        if (!decodeUtf8Scalar(value, offset, scalar)) {
            return false;
        }
        if (scalar == 0U || scalar == 0x7FU ||
            (scalar >= 0x80U && scalar <= 0x9FU)) {
            return false;
        }
        if (scalar < 0x20U &&
            !(allowLayoutWhitespace && (scalar == 0x09U || scalar == 0x0AU ||
                                        scalar == 0x0DU))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isAsciiWhitespace(const char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] std::string_view trimView(std::string_view value) noexcept
{
    while (!value.empty() && isAsciiWhitespace(value.front())) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && isAsciiWhitespace(value.back())) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] std::string trim(std::string_view value)
{
    return std::string{trimView(value)};
}

[[nodiscard]] std::string normalizeNewlines(const std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (std::size_t index{}; index < value.size(); ++index) {
        if (value[index] != '\r') {
            normalized.push_back(value[index]);
            continue;
        }
        if (index + 1U < value.size() && value[index + 1U] == '\n') {
            ++index;
        }
        normalized.push_back('\n');
    }
    return normalized;
}

[[nodiscard]] bool isSafeToken(const std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](const char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' ||
                character == '_' || character == '.';
        });
}

constexpr std::array<std::string_view, 53U> CanonicalTools{
    "forge_status", "agent_list", "agent_get", "agent_context", "agent_recommend",
    "agent_run_start", "agent_run_status", "agent_run_complete", "fs_read",
    "fs_write", "fs_edit", "fs_list", "fs_glob", "fs_mkdir", "fs_delete",
    "fs_move", "git_status", "git_diff", "git_log", "git_add", "git_commit",
    "search_text", "pdf_write", "pdf_from_file", "shell_exec", "memory_set",
    "memory_get", "memory_list", "memory_delete", "memory_search",
    "session_checkpoint", "session_handoff", "context_get", "context_list",
    "project_memory.initialize", "project_memory.remember",
    "project_memory.remember_batch", "project_memory.search", "project_memory.get",
    "project_memory.update", "project_memory.forget", "project_memory.list_recent",
    "project_memory.link", "project_memory.export", "project_memory.import",
    "project_memory.status", "continuity.checkpoint", "continuity.prepare_handoff",
    "continuity.get_pending_handoff", "continuity.acknowledge_handoff",
    "continuity.resume", "continuity.status", "continuity.request_rollover"};

[[nodiscard]] bool isCanonicalTool(const std::string_view tool) noexcept
{
    return std::find(CanonicalTools.begin(), CanonicalTools.end(), tool) !=
        CanonicalTools.end();
}

[[nodiscard]] Domain::Result<void> validateTextField(
    const std::string_view value,
    const std::string_view field,
    const std::size_t maximumBytes,
    const bool allowLayoutWhitespace = false)
{
    if (value.size() > maximumBytes) {
        return Domain::Result<void>::failure(catalogError(
            Domain::ErrorCodes::PayloadTooLarge,
            std::string{"Agent definition field exceeds its byte limit: "} +
                std::string{field} + "."));
    }
    if (!isValidText(value, allowLayoutWhitespace)) {
        return Domain::Result<void>::failure(catalogError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{"Agent definition field is not valid UTF-8 text: "} +
                std::string{field} + "."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateList(
    const std::vector<std::string>& values,
    const std::string_view field)
{
    if (values.size() > AgentCatalog::MaximumListItems) {
        return Domain::Result<void>::failure(catalogError(
            Domain::ErrorCodes::LimitExceeded,
            std::string{"Agent definition list exceeds its item limit: "} +
                std::string{field} + "."));
    }
    for (const auto& value : values) {
        auto valid = validateTextField(
            value, field, AgentCatalog::MaximumFieldBytes);
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::size_t saturatingAdd(
    const std::size_t left,
    const std::size_t right) noexcept
{
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

void addListBytes(std::size_t& total, const std::vector<std::string>& values) noexcept
{
    for (const auto& value : values) {
        total = saturatingAdd(total, value.size());
    }
}

[[nodiscard]] Domain::Result<void> validateSpec(const Domain::AgentSpec& spec)
{
    auto displayName = validateTextField(
        spec.displayName, "display_name", AgentCatalog::MaximumFieldBytes);
    if (!displayName) return displayName;
    auto description = validateTextField(
        spec.description, "description", AgentCatalog::MaximumFieldBytes);
    if (!description) return description;
    auto body = validateTextField(
        spec.body, "body", AgentCatalog::MaximumDefinitionBytes, true);
    if (!body) return body;

    const std::array<std::pair<const std::vector<std::string>*, std::string_view>, 8U>
        lists{{
            {&spec.tools, "tools"},
            {&spec.toolsForbidden, "tools_forbidden"},
            {&spec.whenToUse, "when_to_use"},
            {&spec.firstMoves, "first_moves"},
            {&spec.doneDefinition, "done_definition"},
            {&spec.outputSchema, "output_schema"},
            {&spec.handoff, "handoff"},
            {&spec.qualityBar, "quality_bar"}}};

    std::size_t itemCount{1U};
    for (const auto& [values, field] : lists) {
        auto valid = validateList(*values, field);
        if (!valid) return valid;
        itemCount = saturatingAdd(itemCount, values->size());
    }
    if (itemCount > AgentCatalog::MaximumSpecItems) {
        return Domain::Result<void>::failure(catalogError(
            Domain::ErrorCodes::LimitExceeded,
            "Agent definition exceeds its aggregate item limit."));
    }

    for (const auto& tool : spec.tools) {
        if (!isSafeToken(tool) || !isCanonicalTool(tool)) {
            return Domain::Result<void>::failure(catalogError(
                Domain::ErrorCodes::InvalidRequest,
                "Agent definition grants a tool outside the canonical inventory."));
        }
    }
    for (const auto& tool : spec.toolsForbidden) {
        if (!isSafeToken(tool)) {
            return Domain::Result<void>::failure(catalogError(
                Domain::ErrorCodes::InvalidRequest,
                "Agent definition contains an invalid forbidden-tool token."));
        }
    }
    for (const auto& id : spec.handoff) {
        if (!Domain::AgentId::parse(id)) {
            return Domain::Result<void>::failure(catalogError(
                Domain::ErrorCodes::InvalidRequest,
                "Agent definition contains an invalid handoff agent id."));
        }
    }
    for (const auto& key : spec.outputSchema) {
        if (!isSafeToken(key)) {
            return Domain::Result<void>::failure(catalogError(
                Domain::ErrorCodes::InvalidRequest,
                "Agent definition contains an invalid output-schema key."));
        }
    }

    std::size_t textBytes = spec.id.value().size();
    textBytes = saturatingAdd(textBytes, spec.displayName.size());
    textBytes = saturatingAdd(textBytes, spec.description.size());
    textBytes = saturatingAdd(textBytes, spec.body.size());
    textBytes = saturatingAdd(textBytes, spec.source.size());
    for (const auto& [values, field] : lists) {
        static_cast<void>(field);
        addListBytes(textBytes, *values);
    }
    if (textBytes > AgentCatalog::MaximumSpecTextBytes) {
        return Domain::Result<void>::failure(catalogError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Agent definition exceeds its aggregate text limit."));
    }
    return Domain::Result<void>::success();
}

class SimpleFrontmatter final {
public:
    [[nodiscard]] static std::map<std::string, std::string> parse(
        const std::string_view text)
    {
        std::map<std::string, std::string> output;
        std::string currentKey;
        std::string currentValue;

        const auto flush = [&]() {
            if (!currentKey.empty()) {
                output[currentKey] = trim(currentValue);
            }
            currentKey.clear();
            currentValue.clear();
        };

        std::size_t offset{};
        while (offset <= text.size()) {
            const auto newline = text.find('\n', offset);
            const auto end = newline == std::string_view::npos ? text.size() : newline;
            const auto line = text.substr(offset, end - offset);
            const auto trimmedLine = trimView(line);
            if (!trimmedLine.empty() && trimmedLine.front() == '#') {
                // Comment-only lines do not change the active key.
            } else {
                const auto colon = line.find(':');
                const bool topLevel = colon != std::string_view::npos &&
                    !line.empty() && line.front() != ' ' && line.front() != '\t' &&
                    line.front() != '-';
                if (topLevel) {
                    flush();
                    currentKey = trim(line.substr(0U, colon));
                    auto value = trim(line.substr(colon + 1U));
                    if (!value.empty() && (value.front() == '>' || value.front() == '|')) {
                        value.clear();
                    }
                    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
                        value = value.substr(1U, value.size() - 2U);
                    }
                    currentValue = std::move(value);
                } else if (!currentKey.empty()) {
                    if (trimmedLine.starts_with("- ")) {
                        const auto item = std::string{trimmedLine.substr(2U)};
                        if (currentValue.empty()) {
                            currentValue = item;
                        } else if (currentValue.front() == '[' && currentValue.back() == ']') {
                            currentValue.pop_back();
                            currentValue.append(", ").append(item).push_back(']');
                        } else {
                            currentValue.append(", ").append(item);
                        }
                    } else if (!trimmedLine.empty()) {
                        if (!currentValue.empty()) {
                            currentValue.push_back(' ');
                        }
                        currentValue.append(trimmedLine);
                    }
                }
            }
            if (newline == std::string_view::npos) {
                break;
            }
            offset = newline + 1U;
        }
        flush();
        return output;
    }
};

[[nodiscard]] std::vector<std::string> parseList(const std::string* const raw)
{
    if (raw == nullptr || raw->empty()) {
        return {};
    }
    auto value = trim(*raw);
    if (value.size() >= 2U && value.front() == '[' && value.back() == ']') {
        value = value.substr(1U, value.size() - 2U);
    }

    std::vector<std::string> result;
    std::size_t offset{};
    while (offset <= value.size()) {
        const auto comma = value.find(',', offset);
        const auto end = comma == std::string::npos ? value.size() : comma;
        auto item = trim(std::string_view{value}.substr(offset, end - offset));
        while (!item.empty() && (item.front() == '\'' || item.front() == '"')) {
            item.erase(item.begin());
        }
        while (!item.empty() && (item.back() == '\'' || item.back() == '"')) {
            item.pop_back();
        }
        if (!item.empty()) {
            result.push_back(std::move(item));
        }
        if (comma == std::string::npos) {
            break;
        }
        offset = comma + 1U;
    }
    return result;
}

[[nodiscard]] const std::string* findValue(
    const std::map<std::string, std::string>& values,
    const std::string_view key) noexcept
{
    const auto iterator = values.find(std::string{key});
    return iterator == values.end() ? nullptr : &iterator->second;
}

[[nodiscard]] Domain::Result<Domain::AgentSpec> parseDefinitionUnchecked(
    const AgentDefinitionDocument& definition)
{
    if (definition.stableName.empty()) {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::InvalidRequest,
            "Agent definition stable name is empty."));
    }
    if (definition.stableName.size() > AgentCatalog::MaximumStableNameBytes) {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Agent definition stable name exceeds its byte limit."));
    }
    auto stableName = validateTextField(
        definition.stableName,
        "stable_name",
        AgentCatalog::MaximumStableNameBytes);
    if (!stableName) {
        return Domain::Result<Domain::AgentSpec>::failure(std::move(stableName).error());
    }
    if (definition.markdown.size() > AgentCatalog::MaximumDefinitionBytes) {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Agent definition Markdown exceeds its byte limit."));
    }
    auto sourceText = validateTextField(
        definition.markdown,
        "markdown",
        AgentCatalog::MaximumDefinitionBytes,
        true);
    if (!sourceText) {
        return Domain::Result<Domain::AgentSpec>::failure(std::move(sourceText).error());
    }

    const auto normalized = normalizeNewlines(definition.markdown);
    const auto openingFenceEnd = normalized.find('\n');
    if (openingFenceEnd == std::string::npos ||
        std::string_view{normalized}.substr(0U, openingFenceEnd) != "---") {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::InvalidRequest,
            "Agent Markdown must start with a frontmatter fence."));
    }

    const auto metadataStart = openingFenceEnd + 1U;
    auto closingFence = std::string::npos;
    auto bodyStart = normalized.size();
    auto lineStart = metadataStart;
    while (lineStart <= normalized.size()) {
        const auto lineEnd = normalized.find('\n', lineStart);
        const auto lineLength = lineEnd == std::string::npos
            ? normalized.size() - lineStart
            : lineEnd - lineStart;
        if (std::string_view{normalized}.substr(lineStart, lineLength) == "---") {
            closingFence = lineStart;
            bodyStart = lineEnd == std::string::npos
                ? normalized.size()
                : lineEnd + 1U;
            break;
        }
        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1U;
    }
    if (closingFence == std::string::npos) {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::InvalidRequest,
            "Agent Markdown has invalid frontmatter fences."));
    }

    const auto metadata = SimpleFrontmatter::parse(
        std::string_view{normalized}.substr(
            metadataStart, closingFence - metadataStart));
    const auto* rawId = findValue(metadata, "id");
    if (rawId == nullptr || rawId->empty()) {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::InvalidRequest,
            "Agent frontmatter requires an id."));
    }
    auto id = Domain::AgentId::parse(*rawId);
    if (!id) {
        return Domain::Result<Domain::AgentSpec>::failure(std::move(id).error());
    }

    const auto* displayName = findValue(metadata, "display_name");
    const auto* description = findValue(metadata, "description");
    Domain::AgentSpec spec{
        std::move(id).value(),
        displayName == nullptr ? *rawId : *displayName,
        description == nullptr ? std::string{} : *description,
        parseList(findValue(metadata, "tools")),
        parseList(findValue(metadata, "tools_forbidden")),
        parseList(findValue(metadata, "when_to_use")),
        parseList(findValue(metadata, "first_moves")),
        parseList(findValue(metadata, "done_definition")),
        parseList(findValue(metadata, "output_schema")),
        parseList(findValue(metadata, "handoff")),
        parseList(findValue(metadata, "quality_bar")),
        trim(std::string_view{normalized}.substr(bodyStart)),
        definition.origin == AgentDefinitionOrigin::Custom ? "custom" : "builtin"};

    auto valid = validateSpec(spec);
    if (!valid) {
        return Domain::Result<Domain::AgentSpec>::failure(std::move(valid).error());
    }
    return Domain::Result<Domain::AgentSpec>::success(std::move(spec));
}

[[nodiscard]] Domain::AgentId requiredAgentId(const std::string_view value)
{
    auto id = Domain::AgentId::parse(value);
    if (!id) {
        throw std::logic_error{"An embedded agent id is invalid."};
    }
    return std::move(id).value();
}

[[nodiscard]] std::vector<std::string> strings(const StringList values)
{
    std::vector<std::string> output;
    output.reserve(values.size());
    for (const auto value : values) {
        output.emplace_back(value);
    }
    return output;
}

[[nodiscard]] Domain::AgentSpec fallback(
    const std::string_view id,
    const std::string_view displayName,
    const std::string_view description,
    const StringList tools,
    const StringList forbidden,
    const StringList whenToUse,
    const StringList firstMoves,
    const StringList doneDefinition,
    const StringList outputSchema,
    const StringList handoff,
    const StringList qualityBar,
    const std::string_view body)
{
    return Domain::AgentSpec{
        requiredAgentId(id),
        std::string{displayName},
        std::string{description},
        strings(tools),
        strings(forbidden),
        strings(whenToUse),
        strings(firstMoves),
        strings(doneDefinition),
        strings(outputSchema),
        strings(handoff),
        strings(qualityBar),
        std::string{body},
        "builtin"};
}

[[nodiscard]] std::vector<Domain::AgentSpec> mandatoryFallbacks()
{
    std::vector<Domain::AgentSpec> values;
    values.reserve(AgentCatalog::MandatoryEntryCount);
    values.push_back(fallback(
        "debug", "Debug",
        "Diagnose failures from logs, stack traces, and failing tests with evidence.",
        {"fs_read", "fs_list", "fs_glob", "search_text", "shell_exec", "git_status", "git_diff", "git_log"},
        {"git_push"},
        {"Failing tests or crashes", "Unexpected behavior needing root-cause evidence"},
        {"Capture the exact error and exit code", "Trace the failing path with fs_read and search_text", "Form a hypothesis before large edits", "Call agent_run_complete with the full report"},
        {"Root cause supported by evidence", "Fix or next experiment is explicit", "agent_run_complete called"},
        {"symptom", "repro", "root_cause", "fix", "verify"},
        {"test", "implement", "review"},
        {"Prefer evidence before rewrites", "Use bounded shell execution only when policy enables it", "Always call agent_run_complete"},
        "# Debug agent\n\nDiagnose the smallest reproducible failure, cite concrete paths and command results,\nand separate observations from hypotheses. Always call `agent_run_complete` with\n`symptom`, `repro`, `root_cause`, `fix`, and `verify` before stopping."));
    values.push_back(fallback(
        "docs", "Docs", "Write accurate Markdown documentation and native PDF manuals.",
        {"fs_read", "fs_write", "fs_edit", "fs_list", "fs_glob", "fs_mkdir", "search_text", "git_status", "git_diff", "git_log", "shell_exec", "pdf_write", "pdf_from_file"},
        {"git_push", "git_commit"},
        {"README or API documentation", "Runbooks and operator manuals", "Native PDF guides"},
        {"Discover the existing documentation layout", "Read the implementation being documented", "Draft Markdown with bounded file tools", "Use pdf_from_file or pdf_write for PDF requests", "Verify every output path", "Call agent_run_complete"},
        {"Requested artifacts exist on disk", "Content matches verified project facts", "Requested PDF is nonempty", "files_touched is complete", "agent_run_complete called"},
        {"files_touched", "summary", "formats", "how_to_open"}, {"review"},
        {"Do not document unimplemented behavior as complete", "Update existing documents when appropriate", "Report exact output paths", "Always call agent_run_complete"},
        "# Docs agent\n\nRead authoritative sources before writing. Use `pdf_write` or `pdf_from_file` for\nPDF output and report an exact typed failure if native export cannot complete.\nAlways call `agent_run_complete` with every created or updated path."));
    values.push_back(fallback(
        "explore", "Explore",
        "Map a codebase and report structure, entry points, build commands, risks, and the next specialist.",
        {"fs_list", "fs_read", "fs_glob", "search_text", "git_status", "git_log", "git_diff", "shell_exec"},
        {"fs_write", "fs_edit", "fs_delete", "fs_move", "git_commit", "git_push", "git_add"},
        {"Unfamiliar repository or module", "Structure mapping before planning or implementation"},
        {"List the repository root", "Locate CMake presets solutions projects and declared manifests", "Inspect repository status and recent history", "Read primary entry points", "Call agent_run_complete"},
        {"Layout and entry points cite real paths", "Build test and run commands are identified or explicitly unknown", "Risks and next_agent are filled", "agent_run_complete called"},
        {"layout", "entry_points", "build_test_run", "dependencies_config", "risks", "next_agent"},
        {"plan", "implement", "debug"},
        {"Remain read-only", "Verify every cited path", "Prefer evidence over speculation", "Always call agent_run_complete"},
        "# Explore agent\n\nProduce a read-only, tool-verified map of the selected workspace. Do not invent\npaths or commands. Always call `agent_run_complete` with all output fields and a\nclear recommendation for the next specialist."));
    values.push_back(fallback(
        "implement", "Implement", "Implement features and bug fixes with focused, verified code changes.",
        {"fs_read", "fs_write", "fs_edit", "fs_list", "fs_glob", "fs_mkdir", "search_text", "shell_exec", "git_status", "git_diff", "git_add", "git_commit", "session_checkpoint", "session_handoff", "context_get", "memory_set", "memory_get", "memory_search"},
        {"git_push"},
        {"Feature implementation or bug fix with known scope", "Apply an approved change plan"},
        {"Recover context and checkpoint the goal", "Read surrounding code and tests", "Make the smallest correct edit", "Run the relevant bounded verification", "Inspect the final diff", "Call agent_run_complete"},
        {"Change is applied on disk", "Verification is concrete", "Residual risks are listed", "agent_run_complete called"},
        {"what_changed", "files_touched", "how_to_verify", "residual_risks"},
        {"test", "review", "precommit-audit"},
        {"Read before writing", "Match existing modularity", "Never claim unexecuted verification passed", "Commit only when explicitly requested", "Always call agent_run_complete"},
        "# Implement agent\n\nApply the smallest correct change within the authorized workspace and preserve\nunrelated work. Use durable checkpoints during meaningful progress. Always call\n`agent_run_complete` with changed paths, verification, and residual risks."));
    values.push_back(fallback(
        "plan", "Plan", "Design ordered implementation plans with files, risks, and verification.",
        {"fs_read", "fs_list", "fs_glob", "search_text", "git_status", "git_log", "shell_exec"},
        {"fs_write", "fs_edit", "fs_delete", "git_commit", "git_push", "git_add"},
        {"Architecture or multi-file feature design", "Ordered implementation planning"},
        {"Map relevant modules", "Read key interfaces", "Identify ownership boundaries", "Produce ordered steps and verification", "Call agent_run_complete"},
        {"Goal steps files risks verification and next_agent are filled", "agent_run_complete called"},
        {"goal", "steps", "files", "risks", "verify", "next_agent"}, {"implement", "explore"},
        {"Remain read-only", "Make steps ordered and actionable", "Use paths established by evidence", "Prefer interfaces and contracts", "Always call agent_run_complete"},
        "# Plan agent\n\nDesign an executable plan grounded in repository evidence. Identify affected\ninterfaces, owners, files, risks, and verification in dependency order. Always\ncall `agent_run_complete` before handing the plan to another specialist."));
    values.push_back(fallback(
        "precommit-audit", "Pre-commit Audit",
        "Gate a commit or change review on a structured OK_TO_COMMIT decision.",
        {"git_status", "git_diff", "git_log", "fs_read", "fs_glob", "search_text", "shell_exec"},
        {"git_commit", "git_push", "gh_pr_create"},
        {"Before every requested commit", "Before opening or updating a change review"},
        {"Inspect repository status", "Review staged and unstaged changes", "Scan for credentials and debug leftovers", "Call agent_run_complete with OK_TO_COMMIT"},
        {"Structured report is complete", "OK_TO_COMMIT is yes or no", "Every blocker is listed", "agent_run_complete called"},
        {"diff_summary", "risks", "OK_TO_COMMIT", "blockers"}, {"implement"},
        {"Remain read-only", "Block on exposed credentials", "Distinguish blockers from observations", "Always call agent_run_complete"},
        "# Pre-commit Audit agent\n\nReview the complete pending diff without committing it. Return a defensible gate\ndecision with explicit blockers and risks. Always call `agent_run_complete` with\n`OK_TO_COMMIT` set to `yes` or `no`."));
    values.push_back(fallback(
        "research", "Research",
        "Gather facts from authoritative project sources and report evidence-backed findings.",
        {"fs_read", "fs_list", "fs_glob", "search_text", "git_log", "git_status", "shell_exec"},
        {"fs_write", "fs_edit", "fs_delete", "git_commit", "git_push"},
        {"Factual question about system behavior", "Need citations from local authoritative sources"},
        {"Locate the relevant modules", "Read authoritative sources", "Separate facts from inferences", "Call agent_run_complete with citations"},
        {"Answer cites concrete evidence", "Uncertainties are explicit", "next_agent is selected", "agent_run_complete called"},
        {"question", "findings", "citations", "uncertainties", "next_agent"},
        {"plan", "implement", "docs"},
        {"Remain read-only", "Cite every material claim", "Mark uncertainty instead of guessing", "Always call agent_run_complete"},
        "# Research agent\n\nPrefer authoritative project evidence over assumptions. Cite paths and commands,\nseparate facts from inference, and preserve unresolved uncertainty. Always call\n`agent_run_complete` with findings, citations, and the recommended next agent."));
    values.push_back(fallback(
        "review", "Review", "Review changes for correctness, security, tests, and maintainability.",
        {"git_status", "git_diff", "git_log", "fs_read", "search_text", "shell_exec"},
        {"git_commit", "git_push", "fs_write", "fs_edit", "fs_delete"},
        {"After implementation and before integration", "Critique of a proposed diff"},
        {"Inspect status and the complete diff", "Read high-risk surrounding code", "Check verification and security impact", "Call agent_run_complete with a verdict"},
        {"Verdict is approve or request_changes", "Blockers and nits are separate", "Test gaps are explicit", "agent_run_complete called"},
        {"summary", "blockers", "nits", "test_gaps", "security", "verdict"},
        {"implement", "test", "precommit-audit"},
        {"Remain read-only", "Make blockers actionable and path-specific", "Do not promote style preferences to blockers", "Always call agent_run_complete"},
        "# Review agent\n\nReview the actual diff in its surrounding context. Prioritize correctness and\nsecurity, distinguish blockers from nits, and identify missing verification.\nAlways call `agent_run_complete` with an explicit verdict."));
    values.push_back(fallback(
        "security", "Security",
        "Threat-model changes and identify credentials, injection, and unsafe patterns.",
        {"git_status", "git_diff", "fs_read", "fs_glob", "search_text", "shell_exec"},
        {"git_commit", "git_push", "fs_write", "fs_edit", "fs_delete"},
        {"Authentication credentials network shell database or privilege changes", "Pre-release security review"},
        {"Inspect the diff for new attack surface", "Search for credential and injection patterns", "Trace concrete trust boundaries", "Call agent_run_complete with ranked findings"},
        {"Findings are ranked by severity", "Remediation is concrete", "Residual risk is explicit", "agent_run_complete called"},
        {"scope", "findings", "severity_summary", "remediations", "residual_risk"},
        {"implement", "precommit-audit", "review"},
        {"Remain read-only", "Prefer concrete exploit paths over vague concern", "Never print live credentials", "Always call agent_run_complete"},
        "# Security agent\n\nAnalyze the selected change through explicit trust boundaries and realistic\nattack paths. Redact sensitive values and rank supported findings by severity.\nAlways call `agent_run_complete` with remediation and residual risk."));
    values.push_back(fallback(
        "test", "Test", "Discover, run, and report verification while identifying coverage gaps.",
        {"shell_exec", "fs_read", "fs_list", "fs_glob", "search_text", "git_status"},
        {"git_push", "git_commit"},
        {"Need evidence that a change passes or fails", "Improve or document verification"},
        {"Discover CMake CTest MSBuild or native test entry points", "Run the smallest relevant suite with a deadline", "Capture exact output and exit status", "Call agent_run_complete"},
        {"Commands and results are recorded", "Gaps and follow-ups are listed", "agent_run_complete called"},
        {"commands", "results", "gaps", "follow_ups"}, {"implement", "debug"},
        {"Never invent pass or fail results", "Bound long-running verification", "Prefer targeted tests before full suites", "Always call agent_run_complete"},
        "# Test agent\n\nRun real, bounded verification and preserve exact commands, exit status, and\nsalient output. Report partial execution honestly and identify remaining gaps.\nAlways call `agent_run_complete` with commands, results, gaps, and follow-ups."));

    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left.id.value() < right.id.value();
    });
    for (const auto& value : values) {
        auto valid = validateSpec(value);
        if (!valid) {
            throw std::logic_error{"An embedded agent fallback is invalid."};
        }
    }
    return values;
}

constexpr std::array<std::string_view, AgentCatalog::MandatoryEntryCount> MandatoryIds{
    "debug", "docs", "explore", "implement", "plan", "precommit-audit",
    "research", "review", "security", "test"};

[[nodiscard]] bool isMandatoryId(const std::string_view id) noexcept
{
    return std::find(MandatoryIds.begin(), MandatoryIds.end(), id) != MandatoryIds.end();
}

struct RecommendationRule final {
    std::string_view agentId;
    std::string_view keyword;
};

constexpr auto RecommendationRules = std::to_array<RecommendationRule>({
    {"precommit-audit", "commit"}, {"precommit-audit", "precommit"},
    {"precommit-audit", "pull request"}, {"precommit-audit", "pr "},
    {"precommit-audit", "ok_to_commit"}, {"security", "security"},
    {"security", "auth"}, {"security", "secret"}, {"security", "injection"},
    {"debug", "debug"}, {"debug", "crash"}, {"debug", "traceback"},
    {"debug", "exception"}, {"debug", "failing"}, {"test", "test"},
    {"test", "ctest"}, {"test", "coverage"}, {"docs", "docs"},
    {"docs", "readme"}, {"docs", "pdf"}, {"docs", "manual"},
    {"docs", "handbook"}, {"docs", "runbook"}, {"docs", "documentation"},
    {"research", "research"}, {"research", "web search"}, {"research", "http"},
    {"review", "review"}, {"review", "critique"}, {"plan", "plan"},
    {"plan", "design"}, {"plan", "architecture"}, {"explore", "explore"},
    {"explore", "map"}, {"explore", "codebase"}, {"explore", "structure"},
    {"explore", "overview"}, {"explore", "unfamiliar"},
    {"implement", "implement"}, {"implement", "feature"},
    {"implement", "bugfix"}, {"implement", "write code"}, {"implement", "edit"}});

[[nodiscard]] std::string lowerAscii(const std::string_view value)
{
    std::string lowered{value};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const char value) {
        return value >= 'A' && value <= 'Z'
            ? static_cast<char>(value + ('a' - 'A'))
            : value;
    });
    return lowered;
}

} // namespace

class AgentCatalog::Impl final {
public:
    struct Snapshot final {
        std::vector<Domain::AgentSpec> specifications;
    };

    explicit Impl(std::shared_ptr<Contracts::IClock> clock)
        : clock_{std::move(clock)}
    {
        if (!clock_) {
            throw std::invalid_argument{"An agent catalog clock is required."};
        }
        snapshot_.store(
            std::make_shared<const Snapshot>(Snapshot{mandatoryFallbacks()}),
            std::memory_order_release);
    }

    [[nodiscard]] Domain::Result<AgentCatalogReloadSummary> reload(
        const std::span<const AgentDefinitionDocument> definitions,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = validateContext(*clock_, context);
            if (!admitted) {
                return Domain::Result<AgentCatalogReloadSummary>::failure(
                    std::move(admitted).error());
            }
            if (definitions.size() > AgentCatalog::MaximumDefinitionDocuments) {
                return Domain::Result<AgentCatalogReloadSummary>::failure(catalogError(
                    Domain::ErrorCodes::LimitExceeded,
                    "Agent catalog reload exceeds its document-count limit."));
            }

            std::size_t aggregateBytes{};
            std::vector<const AgentDefinitionDocument*> ordered;
            ordered.reserve(definitions.size());
            for (const auto& definition : definitions) {
                if (definition.markdown.size() > AgentCatalog::MaximumDefinitionBytes ||
                    definition.stableName.size() > AgentCatalog::MaximumStableNameBytes) {
                    return Domain::Result<AgentCatalogReloadSummary>::failure(catalogError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Agent catalog reload contains an oversized definition document."));
                }
                aggregateBytes = saturatingAdd(aggregateBytes, definition.stableName.size());
                aggregateBytes = saturatingAdd(aggregateBytes, definition.markdown.size());
                if (aggregateBytes > AgentCatalog::MaximumAggregateDefinitionBytes) {
                    return Domain::Result<AgentCatalogReloadSummary>::failure(catalogError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Agent catalog reload exceeds its aggregate source limit."));
                }
                ordered.push_back(&definition);
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
                if (left->origin != right->origin) {
                    return left->origin == AgentDefinitionOrigin::BuiltIn;
                }
                if (left->stableName != right->stableName) {
                    return left->stableName < right->stableName;
                }
                return left->markdown < right->markdown;
            });

            std::map<std::string, Domain::AgentSpec> definitionsById;
            for (auto& fallbackSpec : mandatoryFallbacks()) {
                const auto fallbackId = fallbackSpec.id.value();
                definitionsById.emplace(fallbackId, std::move(fallbackSpec));
            }

            AgentCatalogReloadSummary summary;
            for (const auto* definition : ordered) {
                auto active = validateContext(*clock_, context);
                if (!active) {
                    return Domain::Result<AgentCatalogReloadSummary>::failure(
                        std::move(active).error());
                }
                auto parsed = parseDefinitionUnchecked(*definition);
                if (!parsed) {
                    if (parsed.error().code == Domain::ErrorCodes::Cancelled ||
                        parsed.error().code == Domain::ErrorCodes::DeadlineExceeded ||
                        parsed.error().code == Domain::ErrorCodes::InternalFailure) {
                        return Domain::Result<AgentCatalogReloadSummary>::failure(
                            std::move(parsed).error());
                    }
                    ++summary.rejectedDocuments;
                    continue;
                }
                auto specification = std::move(parsed).value();
                const auto specificationId = specification.id.value();
                definitionsById.insert_or_assign(
                    specificationId, std::move(specification));
                ++summary.acceptedDocuments;
            }

            std::size_t additionalCapacity =
                AgentCatalog::MaximumEntries - AgentCatalog::MandatoryEntryCount;
            std::vector<Domain::AgentSpec> retained;
            retained.reserve((std::min)(definitionsById.size(), AgentCatalog::MaximumEntries));
            for (auto& [id, specification] : definitionsById) {
                if (isMandatoryId(id)) {
                    retained.push_back(std::move(specification));
                } else if (additionalCapacity > 0U) {
                    retained.push_back(std::move(specification));
                    --additionalCapacity;
                }
            }
            std::sort(retained.begin(), retained.end(), [](const auto& left, const auto& right) {
                return left.id.value() < right.id.value();
            });
            summary.retainedDefinitions = retained.size();
            summary.truncatedDefinitions = definitionsById.size() - retained.size();

            auto stillActive = validateContext(*clock_, context);
            if (!stillActive) {
                return Domain::Result<AgentCatalogReloadSummary>::failure(
                    std::move(stillActive).error());
            }
            snapshot_.store(
                std::make_shared<const Snapshot>(Snapshot{std::move(retained)}),
                std::memory_order_release);
            return Domain::Result<AgentCatalogReloadSummary>::success(summary);
        } catch (...) {
            return Domain::Result<AgentCatalogReloadSummary>::failure(catalogError(
                Domain::ErrorCodes::InternalFailure,
                "The bounded agent catalog could not reload its immutable snapshot."));
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> all(
        const Domain::OperationContext& context) const noexcept
    {
        try {
            auto active = validateContext(*clock_, context);
            if (!active) {
                return Domain::Result<std::vector<Domain::AgentSpec>>::failure(
                    std::move(active).error());
            }
            const auto snapshot = snapshot_.load(std::memory_order_acquire);
            auto result = snapshot->specifications;
            active = validateContext(*clock_, context);
            if (!active) {
                return Domain::Result<std::vector<Domain::AgentSpec>>::failure(
                    std::move(active).error());
            }
            return Domain::Result<std::vector<Domain::AgentSpec>>::success(
                std::move(result));
        } catch (...) {
            return Domain::Result<std::vector<Domain::AgentSpec>>::failure(catalogError(
                Domain::ErrorCodes::InternalFailure,
                "The agent catalog could not copy its bounded snapshot."));
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSpec>> get(
        const Domain::AgentId& agentId,
        const Domain::OperationContext& context) const noexcept
    {
        try {
            auto active = validateContext(*clock_, context);
            if (!active) {
                return Domain::Result<std::optional<Domain::AgentSpec>>::failure(
                    std::move(active).error());
            }
            const auto snapshot = snapshot_.load(std::memory_order_acquire);
            const auto iterator = lowerBound(*snapshot, agentId.value());
            if (iterator == snapshot->specifications.end() ||
                iterator->id.value() != agentId.value()) {
                return Domain::Result<std::optional<Domain::AgentSpec>>::success(
                    std::nullopt);
            }
            return Domain::Result<std::optional<Domain::AgentSpec>>::success(*iterator);
        } catch (...) {
            return Domain::Result<std::optional<Domain::AgentSpec>>::failure(catalogError(
                Domain::ErrorCodes::InternalFailure,
                "The agent catalog could not read its immutable snapshot."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentSpec> recommend(
        const std::string_view task,
        const Domain::OperationContext& context) const noexcept
    {
        try {
            auto active = validateContext(*clock_, context);
            if (!active) {
                return Domain::Result<Domain::AgentSpec>::failure(
                    std::move(active).error());
            }
            if (task.size() > AgentCatalog::MaximumRecommendationTaskBytes) {
                return Domain::Result<Domain::AgentSpec>::failure(catalogError(
                    Domain::ErrorCodes::LimitExceeded,
                    "Agent recommendation task exceeds its byte limit."));
            }
            if (!isValidText(task, true)) {
                return Domain::Result<Domain::AgentSpec>::failure(catalogError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Agent recommendation task is not valid UTF-8 text."));
            }
            const auto lowered = lowerAscii(task);
            const auto snapshot = snapshot_.load(std::memory_order_acquire);
            for (const auto& rule : RecommendationRules) {
                if (lowered.find(rule.keyword) == std::string::npos) {
                    continue;
                }
                const auto iterator = lowerBound(*snapshot, rule.agentId);
                if (iterator != snapshot->specifications.end() &&
                    iterator->id.value() == rule.agentId) {
                    return Domain::Result<Domain::AgentSpec>::success(*iterator);
                }
            }
            const auto fallback = lowerBound(*snapshot, "explore");
            if (fallback == snapshot->specifications.end() ||
                fallback->id.value() != "explore") {
                return Domain::Result<Domain::AgentSpec>::failure(catalogError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The mandatory explore agent is missing from the catalog snapshot."));
            }
            return Domain::Result<Domain::AgentSpec>::success(*fallback);
        } catch (...) {
            return Domain::Result<Domain::AgentSpec>::failure(catalogError(
                Domain::ErrorCodes::InternalFailure,
                "The agent catalog could not select a recommendation."));
        }
    }

private:
    [[nodiscard]] static std::vector<Domain::AgentSpec>::const_iterator lowerBound(
        const Snapshot& snapshot,
        const std::string_view id) noexcept
    {
        return std::lower_bound(
            snapshot.specifications.begin(),
            snapshot.specifications.end(),
            id,
            [](const Domain::AgentSpec& specification, const std::string_view candidate) {
                return specification.id.value() < candidate;
            });
    }

    std::shared_ptr<Contracts::IClock> clock_;
    std::atomic<std::shared_ptr<const Snapshot>> snapshot_;
};

AgentCatalog::AgentCatalog(std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

AgentCatalog::~AgentCatalog() noexcept = default;

Domain::Result<std::unique_ptr<AgentCatalog>> AgentCatalog::create(
    std::shared_ptr<Contracts::IClock> clock,
    const std::span<const AgentDefinitionDocument> definitions,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (!clock) {
            return Domain::Result<std::unique_ptr<AgentCatalog>>::failure(catalogError(
                Domain::ErrorCodes::InvalidRequest,
                "An agent catalog clock is required."));
        }
        auto active = validateContext(*clock, context);
        if (!active) {
            return Domain::Result<std::unique_ptr<AgentCatalog>>::failure(
                std::move(active).error());
        }
        auto catalog = std::unique_ptr<AgentCatalog>{
            new AgentCatalog{std::make_unique<Impl>(std::move(clock))}};
        auto loaded = catalog->reload(definitions, context);
        if (!loaded) {
            return Domain::Result<std::unique_ptr<AgentCatalog>>::failure(
                std::move(loaded).error());
        }
        return Domain::Result<std::unique_ptr<AgentCatalog>>::success(
            std::move(catalog));
    } catch (...) {
        return Domain::Result<std::unique_ptr<AgentCatalog>>::failure(catalogError(
            Domain::ErrorCodes::InternalFailure,
            "The bounded agent catalog could not be created."));
    }
}

Domain::Result<Domain::AgentSpec> AgentCatalog::parseDefinition(
    const AgentDefinitionDocument& definition,
    const Contracts::IClock& clock,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = validateContext(clock, context);
        if (!active) {
            return Domain::Result<Domain::AgentSpec>::failure(
                std::move(active).error());
        }
        auto parsed = parseDefinitionUnchecked(definition);
        if (!parsed) {
            return parsed;
        }
        active = validateContext(clock, context);
        if (!active) {
            return Domain::Result<Domain::AgentSpec>::failure(
                std::move(active).error());
        }
        return parsed;
    } catch (...) {
        return Domain::Result<Domain::AgentSpec>::failure(catalogError(
            Domain::ErrorCodes::InternalFailure,
            "The agent definition could not be parsed safely."));
    }
}

Domain::Result<AgentCatalogReloadSummary> AgentCatalog::reload(
    const std::span<const AgentDefinitionDocument> definitions,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->reload(definitions, context);
}

Domain::Result<std::vector<Domain::AgentSpec>> AgentCatalog::all(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->all(context);
}

Domain::Result<std::optional<Domain::AgentSpec>> AgentCatalog::get(
    const Domain::AgentId& agentId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->get(agentId, context);
}

Domain::Result<Domain::AgentSpec> AgentCatalog::recommend(
    const std::string_view task,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->recommend(task, context);
}

} // namespace ForgeConductor::Application
