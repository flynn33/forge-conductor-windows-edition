#include "ForgeConductor/Mcp/McpToolCatalog.h"

#include "ForgeConductor/Mcp/McpJsonCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Mcp {
namespace {

using Json = nlohmann::json;

enum class AdditionalProperties { Omitted, Allowed, Denied };

struct SourceDescriptor final {
    std::string_view name;
    std::string_view description;
    std::string_view pack;
    Domain::ToolEffect effect;
    bool requiresProject;
    bool requiresShell;
};

constexpr auto Read = Domain::ToolEffect::Read;
constexpr auto Write = Domain::ToolEffect::Write;

// The source inventory's advertised_index is alphabetical. The two apparent
// read operations below are writes because status transfers/touches ownership
// and export publishes an artifact.
constexpr std::array<SourceDescriptor, McpToolCatalog::ExpectedToolCount>
    SourceDescriptors{{
        {"agent_context", "Alias of agent_get - full playbook body.", "AgentToolPack", Read, false, false},
        {"agent_get", "Get a specialist agent playbook by id.", "AgentToolPack", Read, false, false},
        {"agent_list", "List specialist agent playbooks.", "AgentToolPack", Read, false, false},
        {"agent_recommend", "Recommend a specialist agent for a task description.", "AgentToolPack", Read, false, false},
        {"agent_run_complete", "Close a session with a report matching output_schema.", "AgentToolPack", Write, false, false},
        {"agent_run_start", "Start a durable specialist session (supersedes prior open sessions).", "AgentToolPack", Write, false, false},
        {"agent_run_status", "Status of an agent session; reminds host to complete open runs.", "AgentToolPack", Write, false, false},
        {"context_get", "Load latest (or id) handoff packet - call first in every new chat bootstrap.", "ContinuityToolPack", Read, false, false},
        {"context_list", "List recent context handoff packets.", "ContinuityToolPack", Read, false, false},
        {"continuity.acknowledge_handoff", "Compare-and-set acknowledgment for an exact successor and handoff.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.checkpoint", "Persist a compact project checkpoint and rollover operation.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.get_pending_handoff", "Fetch the latest unsealed project handoff.", "ContinuityLifecycleToolPack", Read, true, false},
        {"continuity.prepare_handoff", "Build and persist a canonical successor handoff.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.request_rollover", "Prepare rollover; reports memory-only readiness unless a host adapter confirms creation.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.resume", "Seal an acknowledged rollover and atomically select the successor.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.status", "Report durable continuity state, retry metadata, and active session.", "ContinuityLifecycleToolPack", Read, true, false},
        {"forge_status", "Runtime status: home, agents, open sessions, tools.", "AgentToolPack", Read, false, false},
        {"fs_delete", "Delete a file or directory.", "FilesystemToolPack", Write, true, false},
        {"fs_edit", "Replace occurrences of old with new in a file.", "FilesystemToolPack", Write, true, false},
        {"fs_glob", "Find files by name pattern under a path.", "FilesystemToolPack", Read, true, false},
        {"fs_list", "List directory entries.", "FilesystemToolPack", Read, true, false},
        {"fs_mkdir", "Create a directory.", "FilesystemToolPack", Write, true, false},
        {"fs_move", "Move/rename a path.", "FilesystemToolPack", Write, true, false},
        {"fs_read", "Read a UTF-8 text file with optional 1-based line windowing.", "FilesystemToolPack", Read, true, false},
        {"fs_write", "Write a UTF-8 text file.", "FilesystemToolPack", Write, true, false},
        {"git_add", "git add path or -A.", "GitToolPack", Write, true, false},
        {"git_commit", "git commit -m message.", "GitToolPack", Write, true, false},
        {"git_diff", "git diff (optional staged).", "GitToolPack", Read, true, false},
        {"git_log", "git log --oneline.", "GitToolPack", Read, true, false},
        {"git_status", "git status --porcelain.", "GitToolPack", Read, true, false},
        {"memory_delete", "Delete a durable memory note by key.", "MemoryToolPack", Write, false, false},
        {"memory_get", "Read a durable memory note by key.", "MemoryToolPack", Read, false, false},
        {"memory_list", "List durable memory notes (optional prefix/tag; hides internal agent and continuity keys by default).", "MemoryToolPack", Read, false, false},
        {"memory_search", "Search durable memory notes by substring in key/body/tags.", "MemoryToolPack", Read, false, false},
        {"memory_set", "Store a durable key/value note in Forge local memory (survives chat sessions).", "MemoryToolPack", Write, false, false},
        {"pdf_from_file", "Convert a local markdown/text file to PDF.", "DocsToolPack", Write, true, false},
        {"pdf_write", "Write a PDF from markdown-ish text (stdlib, no pandoc).", "DocsToolPack", Write, true, false},
        {"project_memory.export", "Create a checksummed project memory export artifact.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.forget", "Tombstone a record in one project.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.get", "Fetch project memory records by stable ID.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.import", "Preview or transactionally import a checksummed export.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.initialize", "Create or open a durable project-scoped memory store.", "ProjectMemoryToolPack", Write, false, false},
        {"project_memory.link", "Create an idempotent typed link between records.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.list_recent", "List recent project records with bounded pagination.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.remember", "Store one redacted, deduplicated project memory record.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.remember_batch", "Store a bounded batch transactionally.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.search", "Search one project with deterministic bounded pagination.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.status", "Report project memory health, sizes, capabilities, and limits.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.update", "Update a record with optimistic version checking.", "ProjectMemoryToolPack", Write, true, false},
        {"search_text", "Recursive text search (grep).", "SearchToolPack", Read, true, false},
        {"session_checkpoint", "Soft-save context + open agent sessions for continuity (continue working).", "ContinuityToolPack", Write, false, false},
        {"session_handoff", "Finalize context/agent handoff for a new chat; returns resume_seed.", "ContinuityToolPack", Write, false, false},
        {"shell_exec", "Run an opt-in bounded PowerShell command with timeout.", "ShellToolPack", Write, true, true},
    }};

using Property = std::pair<std::string_view, Json>;

[[nodiscard]] Json primitive(const std::string_view type)
{
    return Json{{"type", type}};
}

[[nodiscard]] Json arrayOf(
    Json items,
    const std::optional<std::size_t> maximumItems = std::nullopt)
{
    Json result{{"type", "array"}, {"items", std::move(items)}};
    if (maximumItems) {
        result["maxItems"] = *maximumItems;
    }
    return result;
}

[[nodiscard]] Json objectSchema(
    const std::initializer_list<Property> properties,
    const std::initializer_list<std::string_view> required,
    const AdditionalProperties additional = AdditionalProperties::Omitted)
{
    Json propertyObject = Json::object();
    for (const auto& [name, value] : properties) {
        propertyObject[std::string{name}] = value;
    }
    Json requiredArray = Json::array();
    for (const auto name : required) {
        requiredArray.push_back(name);
    }
    Json result{
        {"type", "object"},
        {"properties", std::move(propertyObject)},
        {"required", std::move(requiredArray)}};
    if (additional != AdditionalProperties::Omitted) {
        result["additionalProperties"] = additional == AdditionalProperties::Allowed;
    }
    return result;
}

[[nodiscard]] Json permissiveObjectSchema()
{
    return Json{
        {"type", "object"},
        {"properties", Json::object()},
        {"additionalProperties", true}};
}

[[nodiscard]] Json legacySchema(const std::string_view name)
{
    const auto string = primitive("string");
    if (name == "agent_run_start") {
        return objectSchema(
            {{"agent_id", string}, {"goal", string}, {"cwd", string}},
            {"agent_id", "goal"});
    }
    if (name == "agent_run_status" || name == "agent_run_complete") {
        return objectSchema(
            {{"session_id", string}, {"report", primitive("object")}},
            {"session_id"});
    }
    if (name == "agent_get" || name == "agent_context") {
        return objectSchema({{"agent_id", string}}, {"agent_id"});
    }
    if (name == "agent_recommend") {
        return objectSchema({{"task", string}}, {"task"});
    }
    if (name == "session_checkpoint" || name == "session_handoff") {
        return objectSchema(
            {{"goal", string},
             {"status", string},
             {"project_slug", string},
             {"cwd", string},
             {"narrative", string},
             {"summary", Json{{"type", "string"}, {"description", "Alias for narrative"}}},
             {"next_actions", arrayOf(string)},
             {"blockers", arrayOf(string)},
             {"key_files", arrayOf(string)},
             {"decisions", arrayOf(string)},
             {"chat_label", string},
             {"handoff_id", Json{{"type", "string"}, {"description", "Update an existing packet"}}},
             {"resume_seed", string}},
            {});
    }
    if (name == "context_get") {
        return objectSchema(
            {{"handoff_id", string},
             {"id", string},
             {"resume_ready", Json{{"type", "boolean"}, {"description", "Prefer latest resume-ready packet"}}}},
            {});
    }
    if (name == "context_list") {
        return objectSchema({{"limit", primitive("integer")}}, {});
    }
    if (name == "fs_read") {
        const auto dash = std::string{"Alias for length "} + "\xE2\x80\x94" +
            " number of lines to return";
        return objectSchema(
            {{"path", string},
             {"offset", Json{{"type", "integer"}, {"description", "1-based start line for a partial read"}}},
             {"length", Json{{"type", "integer"}, {"description", "Number of lines to return (alias: limit)"}}},
             {"limit", Json{{"type", "integer"}, {"description", dash}}}},
            {"path"});
    }
    if (name == "fs_list" || name == "fs_delete" || name == "fs_mkdir") {
        if (name == "fs_list") {
            return objectSchema({{"path", string}}, {});
        }
        return objectSchema({{"path", string}}, {"path"});
    }
    if (name == "fs_write") {
        return objectSchema({{"path", string}, {"content", string}}, {"path", "content"});
    }
    if (name == "shell_exec") {
        return objectSchema(
            {{"command", string},
             {"cwd", string},
             {"timeout_sec", Json{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 120}}}},
            {"command"});
    }
    if (name == "pdf_write") {
        return objectSchema(
            {{"path", string}, {"content", string}, {"title", string}},
            {"path", "content"});
    }
    if (name == "pdf_from_file") {
        return objectSchema(
            {{"source_path", string}, {"dest_path", string}, {"title", string}},
            {"source_path"});
    }
    if (name == "search_text") {
        return objectSchema({{"pattern", string}, {"path", string}}, {"pattern"});
    }
    if (name == "memory_set") {
        return objectSchema(
            {{"key", string},
             {"body", string},
             {"content", Json{{"type", "string"}, {"description", "Alias of body"}}},
             {"tags", arrayOf(string)}},
            {"key", "body"});
    }
    if (name == "memory_get" || name == "memory_delete") {
        return objectSchema({{"key", string}}, {"key"});
    }
    if (name == "memory_list") {
        return objectSchema(
            {{"prefix", string},
             {"tag", string},
             {"include_system", primitive("boolean")},
             {"include_body", primitive("boolean")},
             {"limit", primitive("integer")}},
            {});
    }
    if (name == "memory_search") {
        return objectSchema(
            {{"query", string},
             {"include_system", primitive("boolean")},
             {"include_body", primitive("boolean")},
             {"limit", primitive("integer")}},
            {"query"});
    }
    return permissiveObjectSchema();
}

[[nodiscard]] Json projectMemoryWriteProperties()
{
    const auto string = primitive("string");
    Json result = Json::object();
    result["kind"] = string;
    result["title"] = string;
    result["summary"] = string;
    result["body"] = string;
    result["tags"] = arrayOf(string);
    result["importance"] = primitive("number");
    result["confidence"] = primitive("number");
    result["source_kind"] = string;
    result["source_reference"] = string;
    result["session_id"] = string;
    result["expires_at"] = string;
    result["related_ids"] = arrayOf(string);
    result["idempotency_key"] = string;
    result["deadline_ms"] = primitive("integer");
    return result;
}

[[nodiscard]] Json closedObjectFromProperties(
    Json properties,
    const std::initializer_list<std::string_view> required)
{
    Json requiredArray = Json::array();
    for (const auto name : required) {
        requiredArray.push_back(name);
    }
    return Json{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(requiredArray)},
        {"additionalProperties", false}};
}

void addProjectProperty(Json& properties)
{
    properties["project_id"] = primitive("string");
}

[[nodiscard]] Json projectMemorySchema(const std::string_view name)
{
    const auto string = primitive("string");
    Json properties = Json::object();
    if (name == "project_memory.initialize") {
        properties["project_path"] = string;
        properties["project_id"] = string;
        properties["display_name"] = string;
        properties["repository_identity"] = string;
        properties["idempotency_key"] = string;
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(std::move(properties), {"project_path"});
    }

    addProjectProperty(properties);
    if (name == "project_memory.remember") {
        properties.update(projectMemoryWriteProperties());
        return closedObjectFromProperties(
            std::move(properties), {"project_id", "kind", "title", "summary"});
    }
    if (name == "project_memory.remember_batch") {
        properties["items"] = arrayOf(primitive("object"), 50U);
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(std::move(properties), {"project_id", "items"});
    }
    if (name == "project_memory.search") {
        properties["query"] = string;
        properties["kinds"] = arrayOf(string, McpToolCatalog::MaximumProjectMemoryKinds);
        properties["tags"] = arrayOf(string);
        properties["session_id"] = string;
        properties["limit"] = primitive("integer");
        properties["cursor"] = string;
        properties["include_body"] = primitive("boolean");
        properties["maximum_response_bytes"] = primitive("integer");
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(std::move(properties), {"project_id", "query"});
    }
    if (name == "project_memory.get") {
        properties["id"] = string;
        properties["ids"] = arrayOf(string);
        properties["include_body"] = primitive("boolean");
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(std::move(properties), {"project_id"});
    }
    if (name == "project_memory.update") {
        properties["id"] = string;
        properties["expected_version"] = primitive("integer");
        properties["title"] = string;
        properties["summary"] = string;
        properties["body"] = string;
        properties["tags"] = arrayOf(string);
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(
            std::move(properties), {"project_id", "id", "expected_version"});
    }
    if (name == "project_memory.forget") {
        properties["id"] = string;
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(std::move(properties), {"project_id", "id"});
    }
    if (name == "project_memory.list_recent") {
        properties["kinds"] = arrayOf(string, McpToolCatalog::MaximumProjectMemoryKinds);
        properties["session_id"] = string;
        properties["limit"] = primitive("integer");
        properties["cursor"] = string;
        properties["include_body"] = primitive("boolean");
        properties["maximum_response_bytes"] = primitive("integer");
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(std::move(properties), {"project_id"});
    }
    if (name == "project_memory.link") {
        properties["source_id"] = string;
        properties["target_id"] = string;
        properties["relation"] = string;
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(
            std::move(properties), {"project_id", "source_id", "target_id", "relation"});
    }
    if (name == "project_memory.import") {
        properties["artifact"] = string;
        properties["preview"] = primitive("boolean");
        properties["merge_policy"] = string;
        properties["deadline_ms"] = primitive("integer");
        return closedObjectFromProperties(
            std::move(properties), {"project_id", "artifact"});
    }

    properties["deadline_ms"] = primitive("integer");
    return closedObjectFromProperties(std::move(properties), {"project_id"});
}

[[nodiscard]] Json continuityLifecycleSchema(const std::string_view name)
{
    const auto string = primitive("string");
    const auto boundedStrings = arrayOf(string, 128U);
    if (name == "continuity.checkpoint" ||
        name == "continuity.prepare_handoff" ||
        name == "continuity.request_rollover") {
        return objectSchema(
            {{"project_id", string},
             {"operation_id", string},
             {"handoff_id", string},
             {"predecessor_session_id", string},
             {"provider_session_id", string},
             {"model", string},
             {"mission", string},
             {"constraints", boundedStrings},
             {"phase_id", string},
             {"work_item_id", string},
             {"summary", string},
             {"repository_root", string},
             {"branch", string},
             {"commit", string},
             {"dirty_summary", boundedStrings},
             {"active_files", boundedStrings},
             {"open_work", boundedStrings},
             {"decisions", boundedStrings},
             {"passed_gates", boundedStrings},
             {"open_gates", boundedStrings},
             {"memory_record_ids", boundedStrings},
             {"evidence_ids", boundedStrings},
             {"next_actions", boundedStrings},
             {"adapter_id", string},
             {"idempotency_key", string},
             {"context_budget_source", string},
             {"remaining_budget_estimate", primitive("number")}},
            {"project_id", "predecessor_session_id", "mission"},
            AdditionalProperties::Denied);
    }
    if (name == "continuity.acknowledge_handoff") {
        return objectSchema(
            {{"project_id", string},
             {"operation_id", string},
             {"handoff_id", string},
             {"successor_session_id", string},
             {"adapter_id", string}},
            {"project_id", "operation_id", "handoff_id", "successor_session_id"},
            AdditionalProperties::Denied);
    }
    if (name == "continuity.resume") {
        return objectSchema(
            {{"project_id", string}, {"operation_id", string}},
            {"project_id", "operation_id"},
            AdditionalProperties::Denied);
    }
    return objectSchema(
        {{"project_id", string}}, {"project_id"}, AdditionalProperties::Denied);
}

[[nodiscard]] Json schemaFor(const std::string_view name)
{
    if (name.starts_with("project_memory.")) {
        return projectMemorySchema(name);
    }
    if (name.starts_with("continuity.")) {
        return continuityLifecycleSchema(name);
    }
    return legacySchema(name);
}

[[nodiscard]] std::vector<Domain::McpToolDescriptor> buildDescriptors()
{
    std::vector<Domain::McpToolDescriptor> descriptors;
    descriptors.reserve(SourceDescriptors.size());
    for (const auto& source : SourceDescriptors) {
        descriptors.push_back(Domain::McpToolDescriptor{
            Domain::ToolDescriptor{
                std::string{source.name},
                std::string{source.description},
                std::string{source.pack},
                source.effect,
                Domain::ToolAvailability::Available,
                source.requiresProject,
                source.requiresShell},
            schemaFor(source.name).dump()});
    }
    return descriptors;
}

[[nodiscard]] Domain::Result<void> validationFailure(std::string message)
{
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest, std::move(message)));
}

} // namespace

McpToolCatalog::McpToolCatalog(
    std::vector<Domain::McpToolDescriptor> descriptors) noexcept
    : descriptors_{std::move(descriptors)}
{
}

Domain::Result<std::unique_ptr<McpToolCatalog>> McpToolCatalog::create() noexcept
{
    try {
        auto descriptors = buildDescriptors();
        auto validation = validateDescriptors(descriptors);
        if (!validation) {
            return Domain::Result<std::unique_ptr<McpToolCatalog>>::failure(
                std::move(validation).error());
        }
        return Domain::Result<std::unique_ptr<McpToolCatalog>>::success(
            std::unique_ptr<McpToolCatalog>{
                new McpToolCatalog{std::move(descriptors)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<McpToolCatalog>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The canonical MCP tool catalog could not be created."));
    }
}

Domain::Result<void> McpToolCatalog::validateDescriptors(
    const std::span<const Domain::McpToolDescriptor> descriptors,
    const std::size_t expectedCount) noexcept
{
    try {
        if (expectedCount == 0U || descriptors.size() != expectedCount) {
            return validationFailure(
                "The MCP descriptor count does not match the canonical inventory.");
        }

        McpJsonCodec codec;
        std::set<std::string, std::less<>> names;
        std::string_view previous;
        for (const auto& descriptor : descriptors) {
            auto validTool = Domain::validateToolDescriptor(descriptor.tool);
            if (!validTool) {
                return validTool;
            }
            if (!names.insert(descriptor.tool.name).second) {
                return validationFailure(
                    "The MCP descriptor inventory contains a duplicate tool name.");
            }
            if (!previous.empty() && previous >= descriptor.tool.name) {
                return validationFailure(
                    "MCP descriptors must be advertised in ascending name order.");
            }
            previous = descriptor.tool.name;

            auto canonicalSchema = codec.canonicalize(descriptor.inputSchema);
            if (!canonicalSchema) {
                return validationFailure(
                    "An MCP descriptor contains an invalid input schema.");
            }
            const auto schema = Json::parse(canonicalSchema.value());
            if (!schema.is_object() || schema.value("type", std::string{}) != "object") {
                return validationFailure(
                    "Every MCP input schema must describe an object.");
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return validationFailure(
            "The MCP descriptor inventory could not be validated.");
    }
}

std::span<const Domain::McpToolDescriptor> McpToolCatalog::tools() const noexcept
{
    return descriptors_;
}

} // namespace ForgeConductor::Mcp
