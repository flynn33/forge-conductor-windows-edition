#include "ForgeConductor/Mcp/McpToolCatalog.h"

#include "ForgeConductor/Mcp/McpJsonCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <limits>
#include <cstdint>
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
        {"agent_cancel", "Cancel one independent Manager-owned worker in this project; follow agent_poll to confirm final state.", "AgentWorkerToolPack", Write, true, false},
        {"agent_context", "Alias of agent_get \xE2\x80\x94 full playbook body.", "AgentToolPack", Read, false, false},
        {"agent_get", "Get a specialist agent playbook by id.", "AgentToolPack", Read, false, false},
        {"agent_list", "List specialist agent playbooks.", "AgentToolPack", Read, false, false},
        {"agent_poll", "Read an independent worker's actual state, usage, sealed receipt and UTF-8 output page across MCP reconnects.", "AgentWorkerToolPack", Read, true, false},
        {"agent_recommend", "Recommend a specialist agent for a task description.", "AgentToolPack", Read, false, false},
        {"agent_run_complete", "Close a session with a report matching output_schema.", "AgentToolPack", Write, false, false},
        {"agent_run_start", "Start a durable specialist session (supersedes prior open sessions).", "AgentToolPack", Write, true, false},
        {"agent_run_status", "Status of an agent session; reminds host to complete open runs.", "AgentToolPack", Write, true, false},
        {"agent_spawn", "Start a fresh independent Manager-owned model task with explicit task authorization. Inherits current roots/grants and cannot grant policy approvals or spawn recursively; existing specialist sessions remain available.", "AgentWorkerToolPack", Write, true, false},
        {"browser_open", "Open an HTTP/HTTPS URL in the user's registered browser. Returns launch acceptance, not proof of page loading; observe the browser with desktop tools.", "DesktopToolPack", Write, true, false},
        {"clu.evaluate", "Evaluate development evidence against the exact bound policy revision and return any CLU finding.", "CluGovernanceToolPack", Write, true, false},
        {"clu.export_log", "Export the redacted project-bound CLU governance log without full private policy content.", "CluGovernanceToolPack", Read, true, false},
        {"clu.findings", "List CLU findings, correction requests, notification receipts, and policy coverage state.", "CluGovernanceToolPack", Read, true, false},
        {"clu.resolve", "Attach correction evidence and resolve one exact CLU finding while retaining original evidence.", "CluGovernanceToolPack", Write, true, false},
        {"context_get", "Load latest (or id) handoff packet \xE2\x80\x94 call first in every new chat bootstrap.", "ContinuityToolPack", Read, false, false},
        {"context_list", "List recent context handoff packets.", "ContinuityToolPack", Read, false, false},
        {"continuity.acknowledge_handoff", "Compare-and-set acknowledgment for an exact successor and handoff.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.checkpoint", "Persist a compact project checkpoint and rollover operation.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.get_pending_handoff", "Fetch the latest unsealed project handoff.", "ContinuityLifecycleToolPack", Read, true, false},
        {"continuity.prepare_handoff", "Build and persist a canonical successor handoff.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.request_rollover", "Prepare rollover; reports memory-only readiness unless a host adapter confirms creation.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.resume", "Seal an acknowledged rollover and atomically select the successor.", "ContinuityLifecycleToolPack", Write, true, false},
        {"continuity.status", "Report durable continuity state, retry metadata, and active session.", "ContinuityLifecycleToolPack", Read, true, false},
        {"desktop_capture", "Capture the visible desktop region of a listed window/PID to an authorized PNG and return a model-visible image preview. Overlapping windows may appear; this is visible-screen capture.", "DesktopToolPack", Write, true, false},
        {"desktop_click", "Click a window-relative point in a listed visible window/PID. Requires task authorization and a fresh observation. Returns input submission; observe the resulting UI.", "DesktopToolPack", Write, true, false},
        {"desktop_key", "Submit a named key and optional ctrl/alt/shift modifiers to a listed foreground window/PID. Requires task authorization; observe the resulting UI.", "DesktopToolPack", Write, true, false},
        {"desktop_list", "List visible Windows windows with exact window_id/PID/title and screen geometry under the current Windows account.", "DesktopToolPack", Read, false, false},
        {"desktop_read", "Read bounded Windows accessibility controls for an exact listed visible window/PID, with window-relative rectangles and enabled/offscreen states.", "DesktopToolPack", Read, false, false},
        {"desktop_type", "Submit literal Unicode text to the focused control in an exact listed foreground window/PID. Requires task authorization; observe focus and the resulting UI.", "DesktopToolPack", Write, true, false},
        {"document_write", "Create a valid native Word DOCX ZIP package from a title and paragraphs. No Office installation or Python required. Does not render or approve its content.", "OfficeDocumentToolPack", Write, true, false},
        {"evidence_digest", "Hash authorized binary evidence and append a durable SHA256 capture chain; returns byte lengths and capture/head identities. This is an integrity record, not an identity signature.", "EvidenceToolPack", Write, true, false},
        {"evidence_log_read", "Read and verify the project evidence capture chain with bounded record paging. Retain head_digest independently to detect replacement of the complete chain.", "EvidenceToolPack", Read, true, false},
        {"forge_status", "Runtime and project context: project folder, ordered instruction-package folders, development-policy source, home, agents, sessions, and tools.", "AgentToolPack", Read, false, false},
        {"fs_delete", "Delete a file or directory.", "FilesystemToolPack", Write, true, false},
        {"fs_edit", "Replace occurrences of old with new in a file.", "FilesystemToolPack", Write, true, false},
        {"fs_glob", "Find files by name pattern under a path.", "FilesystemToolPack", Read, true, false},
        {"fs_list", "List directory entries.", "FilesystemToolPack", Read, true, false},
        {"fs_mkdir", "Create a directory.", "FilesystemToolPack", Write, true, false},
        {"fs_move", "Move/rename a path.", "FilesystemToolPack", Write, true, false},
        {"fs_read", "Read a UTF-8 text file. Optional 1-based line window: offset (start line) + length/limit (line count). Response includes total_lines, start_line, end_line, has_more, next_offset. Do not re-call with the same offset when content was returned.", "FilesystemToolPack", Read, true, false},
        {"fs_write", "Write a UTF-8 text file.", "FilesystemToolPack", Write, true, false},
        {"get_forge_status", "Report the project folder, ordered instruction packages, development-policy folder, tool count, and agent count.", "AgentToolPack", Read, false, false},
        {"git_add", "git add path or -A.", "GitToolPack", Write, true, false},
        {"git_commit", "git commit -m message.", "GitToolPack", Write, true, false},
        {"git_diff", "git diff (optional staged).", "GitToolPack", Read, true, false},
        {"git_log", "git log --oneline.", "GitToolPack", Read, true, false},
        {"git_status", "git status --porcelain.", "GitToolPack", Read, true, false},
        {"github_read", "Read GitHub repository runs, artifacts, refs and pull requests via fixed HTTPS GET routes. Uses configured credentials when available; reports permission and network failures explicitly.", "GitHubReadToolPack", Read, true, false},
        {"host_capabilities", "Report actual dedicated capabilities, tool names, filesystem mode and root authority, and external services needing a connection. Consult before claiming a capability is absent.", "HostInspectionToolPack", Read, false, false},
        {"http_request", "Perform an explicit HTTP/HTTPS GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS with bounded caller-supplied headers/body. Mutating methods can change remote state and require task authorization; only GET/HEAD follow redirects. No ambient credentials/cookies; report actual HTTP status and truncation.", "WebAccessToolPack", Write, true, false},
        {"image_read", "Decode an authorized local PNG/JPEG/GIF/BMP/TIFF/ICO using native Windows codecs and return a model-visible PNG preview with original dimensions.", "ImageToolPack", Read, true, false},
        {"image_write", "Render rectangles, ellipses, lines and Unicode text to an authorized native PNG, returning an image preview. Supports diagrams/charts; generative artwork requires a separate image provider.", "ImageToolPack", Write, true, false},
        {"instruction_package.read", "Read a selected instruction package by queue_row_id from get_forge_status. Returns its pinned text and coverage, with cursor and byte-offset paging.", "InstructionPackageToolPack", Read, true, false},
        {"memory_delete", "Delete a durable memory note by key.", "MemoryToolPack", Write, false, false},
        {"memory_get", "Read a durable memory note by key.", "MemoryToolPack", Read, false, false},
        {"memory_list", "List durable memory notes (optional prefix/tag; hides internal agent and continuity keys by default).", "MemoryToolPack", Read, false, false},
        {"memory_search", "Search durable memory notes by substring in key/body/tags.", "MemoryToolPack", Read, false, false},
        {"memory_set", "Store a durable key/value note in Forge local memory (survives chat sessions).", "MemoryToolPack", Write, false, false},
        {"pdf_from_file", "Convert a local markdown/text file to PDF.", "DocsToolPack", Write, true, false},
        {"pdf_write", "Write a PDF from markdown-ish text (stdlib, no pandoc).", "DocsToolPack", Write, true, false},
        {"presentation_write", "Create a valid native PowerPoint PPTX ZIP package with title/body slides, layout, master, theme and relationships. No Office installation or Python required.", "OfficeDocumentToolPack", Write, true, false},
        {"process_adopt", "Adopt a Forge-owned durable job receipt by job_id after reconnecting. Verifies receipt/log integrity and process identity; cannot adopt an arbitrary PID.", "ProcessToolPack", Write, true, false},
        {"process_kill", "Cancel one Forge-owned process tree by job_id; poll until final termination is confirmed.", "ProcessToolPack", Write, true, false},
        {"process_launch", "Launch an authorized executable with exact argv, cwd and environment. Returns stable job_id, actual PID and named stdout/stderr logs. A matching Manager owns the job across MCP reconnects; otherwise status reports connector-owned lifetime.", "ProcessToolPack", Write, true, true},
        {"process_list", "List this project's active and retained durable jobs. Interrupted broker jobs report unknown exit status rather than invented success.", "ProcessToolPack", Read, true, false},
        {"process_poll", "Read a durable job's alive/done state, actual PID, elapsed time, exit status and log/receipt paths. Poll no faster than every five seconds.", "ProcessToolPack", Read, true, false},
        {"process_read_log", "Read live or final stdout/stderr by job_id with tail_lines or byte offset and bounded paging; returns next_offset and truncation state.", "ProcessToolPack", Read, true, false},
        {"process_status", "Read a bounded native Windows process and service snapshot as the current user, without elevation.", "HostInspectionToolPack", Read, false, false},
        {"process_wait", "Wait up to 30 seconds for a durable job to finish. Timeout returns done=false and leaves the job running; repeat or poll for longer runs.", "ProcessToolPack", Read, true, false},
        {"project_memory.forget", "Tombstone a record in one project.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.get", "Fetch project memory records by stable ID.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.initialize", "Create or open a durable project-scoped memory store.", "ProjectMemoryToolPack", Write, false, false},
        {"project_memory.link", "Create an idempotent typed link between records.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.list_recent", "List recent project records with bounded pagination.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.remember", "Store one redacted, deduplicated project memory record.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.remember_batch", "Store a bounded batch transactionally.", "ProjectMemoryToolPack", Write, true, false},
        {"project_memory.search", "Search one project with deterministic bounded pagination.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.status", "Report project memory health, sizes, capabilities, and limits.", "ProjectMemoryToolPack", Read, true, false},
        {"project_memory.update", "Update a record with optimistic version checking.", "ProjectMemoryToolPack", Write, true, false},
        {"project_policy.read", "Read the adopted policy index or an exact pinned document. Follow next_cursor for every index page; supply path and next_offset as offset for every document part. This tool cannot adopt policy or approve reviews.", "ProjectPolicyToolPack", Read, true, false},
        {"provider_status", "Inspect actually loaded LM Studio models and configured context/reserves. Missing file, revision or version facts remain null with provenance and reasons.", "HostInspectionToolPack", Read, false, false},
        {"reviewer_cancel", "Cancel one independently started read-only reviewer run for this project.", "ReviewerToolPack", Write, true, false},
        {"reviewer_start", "Start a fresh Manager-owned reviewer with exactly one authorized opening_message_path or bounded inline opening_message and no executor history. Requires explicit review authorization; read-only tools are enforced. receive_timeout_sec budgets each provider Responses request from send through response body, defaults to 600 (1...3600), and remains bounded by its caller deadline; mode=text_only omits tools and reviews only supplied text.", "ReviewerToolPack", Write, true, false},
        {"reviewer_status", "Read the separate reviewer run's actual state, provider response, token usage and bounded UTF-8 output page; follow next_output_offset for more. A running or failed review is not an approved gate.", "ReviewerToolPack", Read, true, false},
        {"schedule_cancel", "Cancel one persistent model-task schedule and request cancellation of its active worker. Requires explicit authorization.", "ScheduledTaskToolPack", Write, true, false},
        {"schedule_create", "Persist an authorized model task for a UTC time or interval. Freezes current authority/tool allowlist, runs through the Manager, and blocks uncertain interrupted effects from automatic replay.", "ScheduledTaskToolPack", Write, true, false},
        {"schedule_list", "Read this project's persistent model schedules, actual run state, and required owner attention.", "ScheduledTaskToolPack", Read, true, false},
        {"schedule_run_now", "Explicitly start a stored task now, including resumption after uncertain interrupted effects. Requires authorization and current owner authority.", "ScheduledTaskToolPack", Write, true, false},
        {"search_text", "Recursive text search (grep).", "SearchToolPack", Read, true, false},
        {"session_checkpoint", "Soft-save context + open agent sessions for continuity (continue working).", "ContinuityToolPack", Write, false, false},
        {"session_handoff", "Finalize context/agent handoff for a new chat; returns resume_seed. Prefer before context is full.", "ContinuityToolPack", Write, false, false},
        {"shell_exec", "Run an opt-in PowerShell command for at most 120 seconds. Its process tree is terminated when the command exits or times out. Use shell_job_start for longer foreground builds/tests.", "ShellToolPack", Write, true, true},
        {"shell_job_cancel", "Request cancellation of one tracked shell job and its process tree; poll shell_job_status until terminal state confirms termination.", "ShellToolPack", Write, true, false},
        {"shell_job_list", "Discover this project's running and retained jobs and durable receipts. Returns summaries; read named logs with process_read_log.", "ShellToolPack", Read, true, false},
        {"shell_job_start", "Start a Manager-owned PowerShell job with up to 3600 seconds lifetime, named logs and durable receipt. Survives MCP reconnect when a matching Manager is available; status reports fallback owner lifetime.", "ShellToolPack", Write, true, true},
        {"shell_job_status", "Poll a tracked shell job by job_id, including after Manager-backed MCP reconnect. Running is not failure. Terminal state includes captured final output, exit code, timeout/cancellation and truncation flags. Poll at most every 5 seconds.", "ShellToolPack", Read, true, false},
        {"spreadsheet_write", "Create a valid native Excel XLSX ZIP package from sheets and typed cell rows. Text is literal; formulas are not evaluated. No Office installation or Python required.", "OfficeDocumentToolPack", Write, true, false},
        {"verification_env_create", "Materialize a pinned Python venv in an authorized external directory as a durable job, recording exact installed versions and a manifest. Does not edit product source or grant elevation.", "VerificationToolPack", Write, true, true},
        {"verification_env_status", "Read the pinned verification venv manifest from an authorized directory; reports exact runtime and distributions only after successful creation.", "VerificationToolPack", Read, true, false},
        {"web_fetch", "Fetch an explicit HTTP/HTTPS page with native WinHTTP. Return actual status/final URL/content type and bounded UTF-8 or binary data; HTML is source, not executed browser code.", "WebAccessToolPack", Read, true, false},
        {"web_search", "Search the public web and return observed titles, links and snippets with source URLs. Challenges and unavailable results are explicit; returned text is untrusted.", "WebAccessToolPack", Read, true, false},
        {"workspace_authority_bind", "Bind an existing additional root already present in the owner-configured allowlist to this project. Tool arguments cannot add new grants; forge_status reports configured and active roots.", "FilesystemToolPack", Write, true, false},
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
    const auto boundedText = [](const std::size_t bytes) {
        return Json{{"type", "string"}, {"maxLength", bytes}};
    };
    const auto boundedInteger = [](const std::int64_t minimum, const std::int64_t maximum) {
        return Json{{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
    };
    if (name == "agent_spawn") return objectSchema({{"task", boundedText(65'536U)},
        {"authorization", boundedText(4096U)}, {"agent_id", boundedText(128U)},
        {"timeout_sec", boundedInteger(1, 3600)}}, {"task", "authorization"}, AdditionalProperties::Denied);
    if (name == "agent_poll") return objectSchema({{"run_id", primitive("string")},
        {"output_offset", boundedInteger(0, 262'144)}, {"max_output_bytes", boundedInteger(1, 32'768)}},
        {"run_id"}, AdditionalProperties::Denied);
    if (name == "agent_cancel") return objectSchema({{"run_id", primitive("string")},
        {"authorization", boundedText(4096U)}}, {"run_id", "authorization"}, AdditionalProperties::Denied);
    if (name == "schedule_create") return objectSchema({{"name", boundedText(128U)},
        {"task", boundedText(65'536U)}, {"owner_reference", boundedText(512U)},
        {"authorization", boundedText(2048U)}, {"at_time", boundedText(20U)},
        {"interval_sec", boundedInteger(60, 31'536'000)}, {"timeout_sec", boundedInteger(1, 3600)},
        {"allow_tools", primitive("boolean")}, {"read_only_tools", primitive("boolean")},
        {"allowed_tools", arrayOf(boundedText(128U), 128U)}},
        {"name", "task", "owner_reference", "authorization"}, AdditionalProperties::Denied);
    if (name == "schedule_cancel" || name == "schedule_run_now") return objectSchema({
        {"schedule_id", primitive("string")}, {"authorization", boundedText(2048U)}},
        {"schedule_id", "authorization"}, AdditionalProperties::Denied);
    if (name == "schedule_list") return objectSchema({{"schedule_id", boundedText(36U)},
        {"offset", boundedInteger(0, 8'388'608)}, {"max_bytes", boundedInteger(1, 32'768)},
        {"revision", boundedText(20U)}}, {}, AdditionalProperties::Denied);
    if (name == "host_capabilities") return objectSchema({}, {}, AdditionalProperties::Denied);
    if (name == "web_fetch" || name == "http_request") {
        auto properties = Json{{"url", boundedText(8192U)}, {"max_bytes", boundedInteger(1, 49'152)},
            {"timeout_sec", boundedInteger(1, 60)}};
        if (name == "http_request") {
            properties["method"] = Json{{"type", "string"}, {"enum", {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"}}};
            properties["headers"] = Json{{"type", "object"}, {"additionalProperties", boundedText(16'384U)}};
            properties["body"] = boundedText(262'144U);
        }
        Json schema{{"type", "object"}, {"properties", std::move(properties)},
            {"required", {"url"}}, {"additionalProperties", false}};
        return schema;
    }
    if (name == "web_search") return objectSchema({{"query", boundedText(2048U)},
        {"limit", boundedInteger(1, 10)}, {"timeout_sec", boundedInteger(1, 60)}},
        {"query"}, AdditionalProperties::Denied);
    if (name == "document_write") return objectSchema({{"path", boundedText(32'768U)},
        {"title", boundedText(65'536U)}, {"paragraphs", arrayOf(boundedText(65'536U), 4096U)}},
        {"path", "paragraphs"}, AdditionalProperties::Denied);
    if (name == "spreadsheet_write") {
        const Json cell{{"type", {"string", "number", "boolean", "null"}}};
        const auto sheet = objectSchema({{"name", boundedText(128U)},
            {"rows", arrayOf(arrayOf(cell, 256U), 10'000U)}}, {"name", "rows"}, AdditionalProperties::Denied);
        return objectSchema({{"path", boundedText(32'768U)}, {"sheets", arrayOf(sheet, 32U)}},
            {"path", "sheets"}, AdditionalProperties::Denied);
    }
    if (name == "presentation_write") {
        const auto slide = objectSchema({{"title", boundedText(65'536U)},
            {"body", arrayOf(boundedText(65'536U), 128U)}}, {"title", "body"}, AdditionalProperties::Denied);
        return objectSchema({{"path", boundedText(32'768U)}, {"title", boundedText(65'536U)},
            {"slides", arrayOf(slide, 128U)}}, {"path", "slides"}, AdditionalProperties::Denied);
    }
    if (name == "browser_open") return objectSchema({{"url", boundedText(16'384U)}},
        {"url"}, AdditionalProperties::Denied);
    if (name == "desktop_list") return objectSchema({{"limit", boundedInteger(1, 500)}}, {},
        AdditionalProperties::Denied);
    if (name == "desktop_capture" || name == "desktop_click" || name == "desktop_key" ||
        name == "desktop_read" || name == "desktop_type") {
        Json properties{{"window_id", boundedInteger(1, (std::numeric_limits<std::int64_t>::max)())},
            {"pid", boundedInteger(1, 4'294'967'295LL)}};
        Json required = Json::array({"window_id", "pid"});
        if (name == "desktop_read") properties["limit"] = boundedInteger(1, 300);
        if (name == "desktop_capture") { properties["path"] = boundedText(32'768U); required.push_back("path"); }
        if (name == "desktop_click") {
            properties["x"] = boundedInteger(0, 8192); properties["y"] = boundedInteger(0, 8192);
            properties["button"] = Json{{"type", "string"}, {"enum", {"left", "right"}}};
            required.push_back("x"); required.push_back("y");
        }
        if (name == "desktop_type") { properties["text"] = boundedText(65'536U); required.push_back("text"); }
        if (name == "desktop_key") {
            properties["key"] = boundedText(32U); required.push_back("key");
            for (const auto field : {"ctrl", "alt", "shift"}) properties[field] = primitive("boolean");
        }
        return Json{{"type", "object"}, {"properties", std::move(properties)},
            {"required", std::move(required)}, {"additionalProperties", false}};
    }
    if (name == "image_read") return objectSchema({{"path", boundedText(32'768U)}},
        {"path"}, AdditionalProperties::Denied);
    if (name == "image_write") {
        const auto shape = objectSchema({{"type", Json{{"type", "string"}, {"enum", {"rectangle", "ellipse", "line", "text"}}}},
            {"x", boundedInteger(-8192, 8192)}, {"y", boundedInteger(-8192, 8192)},
            {"x2", boundedInteger(-8192, 8192)}, {"y2", boundedInteger(-8192, 8192)},
            {"width", boundedInteger(1, 8192)}, {"height", boundedInteger(1, 8192)},
            {"stroke_width", boundedInteger(1, 128)}, {"color", boundedText(7U)},
            {"text", boundedText(16'384U)}, {"size", boundedInteger(6, 256)}}, {"type"}, AdditionalProperties::Denied);
        return objectSchema({{"path", boundedText(32'768U)}, {"width", boundedInteger(1, 4096)},
            {"height", boundedInteger(1, 4096)}, {"background", boundedText(7U)}, {"elements", arrayOf(shape, 1000U)}},
            {"path", "elements"}, AdditionalProperties::Denied);
    }
    if (name == "instruction_package.read") {
        return objectSchema({
            {"queue_row_id", primitive("string")},
            {"path", primitive("string")},
            {"cursor", primitive("string")},
            {"offset", Json{{"type", "integer"}, {"minimum", 0}}}},
            {"queue_row_id"}, AdditionalProperties::Denied);
    }
    if (name == "project_policy.read") {
        return Json{{"type", "object"}, {"properties", {{"path", {{"type", "string"}}},
            {"offset", {{"type", "integer"}, {"minimum", 0}}},
            {"cursor", {{"type", "string"}}}}}, {"additionalProperties", false}};
    }
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
        auto schema = objectSchema(
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
        if (name == "session_handoff") {
            schema["properties"]["decisions"]["description"] =
                "For Auto Continuity, record all explicit user constraints as nonblank strings in this nonempty array.";
            schema["properties"]["packet_json"] = Json{
                {"type", "string"},
                {"description", "JSON-string containing the complete model-authored packet object: goal, narrative, resume_seed, key_files array, next_actions array, and any other session_handoff fields. Arrays must be JSON arrays inside that object."}};
        }
        return schema;
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
             {"byte_offset", Json{{"type", "integer"}, {"description", "UTF-8 byte offset returned by next_byte_offset for an oversized line"}}},
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
    if (name == "fs_edit") {
        return objectSchema(
            {{"path", string}, {"old", string}, {"new", string}},
            {"path", "old", "new"}, AdditionalProperties::Allowed);
    }
    if (name == "fs_glob") {
        return objectSchema({{"path", string}, {"pattern", string}}, {},
            AdditionalProperties::Allowed);
    }
    if (name == "fs_move") {
        return objectSchema(
            {{"path", string}, {"dest", string}, {"src", string},
             {"source", string}, {"destination", string}},
            {"path", "dest"}, AdditionalProperties::Allowed);
    }

    if (name == "provider_status" || name == "process_status" || name == "process_list")
        return objectSchema({}, {}, AdditionalProperties::Denied);
    if (name == "workspace_authority_bind")
        return objectSchema({{"root", string}}, {"root"}, AdditionalProperties::Denied);
    if (name == "evidence_digest")
        return objectSchema({{"paths", arrayOf(string, 64)}}, {"paths"}, AdditionalProperties::Denied);
    if (name == "evidence_log_read")
        return objectSchema({{"offset", Json{{"type", "integer"}, {"minimum", 0}}},
            {"limit", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
            {"verify", primitive("boolean")}}, {}, AdditionalProperties::Denied);
    if (name == "github_read")
        return objectSchema({{"repository", string}, {"operation", Json{{"type", "string"},
            {"enum", {"runs", "run", "artifacts", "artifact", "refs", "pull_requests", "pull_request", "pull_request_files"}}}},
            {"id", Json{{"type", "integer"}, {"minimum", 1}}}, {"ref", string},
            {"page", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
            {"per_page", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
            {"repository", "operation"}, AdditionalProperties::Denied);
    const auto jobId = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
    if (name == "process_poll" || name == "process_kill" || name == "process_adopt")
        return objectSchema({{"job_id", jobId}}, {"job_id"}, AdditionalProperties::Denied);
    if (name == "process_wait")
        return objectSchema({{"job_id", jobId}, {"timeout_sec", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 30}}}},
            {"job_id"}, AdditionalProperties::Denied);
    if (name == "process_read_log")
        return objectSchema({{"job_id", jobId}, {"stream", Json{{"type", "string"}, {"enum", {"stdout", "stderr"}}}},
            {"tail_lines", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
            {"offset", Json{{"type", "integer"}, {"minimum", 0}}}}, {"job_id"}, AdditionalProperties::Denied);
    if (name == "process_launch")
        return objectSchema({{"command", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
            {"args", arrayOf(Json{{"type", "string"}, {"maxLength", 4096}}, 256)}, {"cwd", string},
            {"env", Json{{"type", "object"}, {"maxProperties", 128},
                {"additionalProperties", Json{{"type", "string"}, {"maxLength", 4096}}}}},
            {"timeout_sec", Json{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 3600}}}},
            {"command"}, AdditionalProperties::Denied);
    if (name == "reviewer_start") {
        auto schema = objectSchema({{"opening_message_path", string},
            {"opening_message", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 65536},
                {"description", "Inline UTF-8 opening message, at most 65536 bytes; mutually exclusive with opening_message_path."}}},
            {"task", Json{{"type", "string"}, {"maxLength", 32768}}},
            {"authorization", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
            {"receive_timeout_sec", Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}, {"default", 600},
                {"description", "Per-provider Responses request budget from send through complete response body; caller deadline may shorten it. This is not the total reviewer lifetime."}}},
            {"mode", Json{{"type", "string"}, {"enum", {"tools", "text_only"}}, {"default", "tools"}}}},
            {"authorization"}, AdditionalProperties::Denied);
        schema["oneOf"] = Json::array({Json{{"required", {"opening_message_path"}}},
            Json{{"required", {"opening_message"}}}});
        return schema;
    }
    if (name == "reviewer_status")
        return objectSchema({{"run_id", jobId},
            {"output_offset", Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 262144}}},
            {"max_output_bytes", Json{{"type", "integer"}, {"minimum", 4}, {"maximum", 32768}}}},
            {"run_id"}, AdditionalProperties::Denied);
    if (name == "reviewer_cancel")
        return objectSchema({{"run_id", jobId}}, {"run_id"}, AdditionalProperties::Denied);
    if (name == "verification_env_create")
        return objectSchema({{"path", string}, {"python_path", string},
            {"requirements", arrayOf(Json{{"type", "string"},
                {"enum", {"jsonschema==4.25.1", "PyYAML==6.0.3"}}}, 2)}},
            {"path", "python_path"}, AdditionalProperties::Denied);
    if (name == "verification_env_status")
        return objectSchema({{"path", string}}, {"path"}, AdditionalProperties::Denied);
    if (name == "shell_job_start") {
        return objectSchema(
            {{"command", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 65536}}},
             {"cwd", string},
             {"timeout_sec", Json{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 3600}}}},
            {"command"}, AdditionalProperties::Denied);
    }
    if (name == "shell_job_status" || name == "shell_job_cancel") {
        return objectSchema({{"job_id", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}},
            {"job_id"}, AdditionalProperties::Denied);
    }
    if (name == "shell_job_list") {
        return objectSchema({}, {}, AdditionalProperties::Denied);
    }
    if (name == "shell_exec") {
        return objectSchema(
            {{"command", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 65536}}},
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
        properties["expected_checksum"] = string;
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

[[nodiscard]] Json cluGovernanceSchema(const std::string_view name)
{
    if (name == "clu.findings" || name == "clu.export_log") {
        return objectSchema({}, {}, AdditionalProperties::Denied);
    }
    if (name == "clu.evaluate") {
        return objectSchema(
            {{"evidence", primitive("object")}}, {"evidence"},
            AdditionalProperties::Denied);
    }
    auto findingId = primitive("string");
    findingId["minLength"] = 1U;
    findingId["maxLength"] = 128U;
    return objectSchema(
        {{"finding_id", std::move(findingId)},
         {"correction_evidence", primitive("object")}},
        {"finding_id", "correction_evidence"},
        AdditionalProperties::Denied);
}

[[nodiscard]] Json schemaFor(const std::string_view name)
{
    if (name.starts_with("clu.")) {
        return cluGovernanceSchema(name);
    }
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
