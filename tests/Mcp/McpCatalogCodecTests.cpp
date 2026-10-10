#include "ForgeConductor/Mcp/Mcp.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
using Json = nlohmann::json;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] const Domain::McpToolDescriptor& descriptor(
    const std::span<const Domain::McpToolDescriptor> tools,
    const std::string_view name)
{
    const auto match = std::find_if(
        tools.begin(), tools.end(), [name](const auto& candidate) {
            return candidate.tool.name == name;
        });
    REQUIRE(match != tools.end());
    return *match;
}

[[nodiscard]] Json schema(
    const std::span<const Domain::McpToolDescriptor> tools,
    const std::string_view name)
{
    return Json::parse(descriptor(tools, name).inputSchema);
}

void testCanonicalCatalog()
{
    static_assert(std::is_final_v<Mcp::McpToolCatalog>);
    static_assert(!std::is_copy_constructible_v<Mcp::McpToolCatalog>);
    static_assert(!std::is_move_constructible_v<Mcp::McpToolCatalog>);

    constexpr std::array<std::string_view, 125U> ExpectedNames{
        "agent_cancel",
        "agent_context",
        "agent_get",
        "agent_list",
        "agent_poll",
        "agent_recommend",
        "agent_run_complete",
        "agent_run_start",
        "agent_run_status",
        "agent_spawn",
        "browser_open",
        "clu.evaluate",
        "clu.export_log",
        "clu.findings",
        "clu.resolve",
        "cmake_test_run",
        "cmake_test_status",
        "comfy_catalog",
        "comfy_control",
        "comfy_job_cancel",
        "comfy_job_list",
        "comfy_job_resume",
        "comfy_job_status",
        "comfy_prepare",
        "comfy_run",
        "comfy_status",
        "comfy_validate",
        "comfy_workflow",
        "context_get",
        "context_list",
        "continuity.acknowledge_handoff",
        "continuity.checkpoint",
        "continuity.get_pending_handoff",
        "continuity.prepare_handoff",
        "continuity.request_rollover",
        "continuity.resume",
        "continuity.status",
        "desktop_capture",
        "desktop_click",
        "desktop_drag",
        "desktop_key",
        "desktop_list",
        "desktop_read",
        "desktop_scroll",
        "desktop_type",
        "document_write",
        "evidence_digest",
        "evidence_log_read",
        "forge_status",
        "fs_delete",
        "fs_edit",
        "fs_glob",
        "fs_list",
        "fs_mkdir",
        "fs_move",
        "fs_read",
        "fs_write",
        "get_forge_status",
        "git_add",
        "git_commit",
        "git_diff",
        "git_log",
        "git_status",
        "github_read",
        "host_capabilities",
        "http_request",
        "image_analyze",
        "image_edit",
        "image_generate",
        "image_job_cancel",
        "image_job_resume",
        "image_job_status",
        "image_provider_status",
        "image_read",
        "image_write",
        "instruction_package.read",
        "memory_delete",
        "memory_get",
        "memory_list",
        "memory_search",
        "memory_set",
        "pdf_from_file",
        "pdf_write",
        "presentation_write",
        "process_adopt",
        "process_kill",
        "process_launch",
        "process_list",
        "process_poll",
        "process_read_log",
        "process_status",
        "process_wait",
        "project_memory.forget",
        "project_memory.get",
        "project_memory.initialize",
        "project_memory.link",
        "project_memory.list_recent",
        "project_memory.remember",
        "project_memory.remember_batch",
        "project_memory.search",
        "project_memory.status",
        "project_memory.update",
        "project_policy.read",
        "provider_status",
        "reviewer_cancel",
        "reviewer_start",
        "reviewer_status",
        "schedule_cancel",
        "schedule_create",
        "schedule_list",
        "schedule_run_now",
        "search_text",
        "session_checkpoint",
        "session_handoff",
        "shell_exec",
        "shell_job_cancel",
        "shell_job_list",
        "shell_job_start",
        "shell_job_status",
        "spreadsheet_write",
        "verification_env_create",
        "verification_env_status",
        "web_fetch",
        "web_search",
        "workspace_authority_bind"};

    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();
    REQUIRE(tools.size() == Mcp::McpToolCatalog::ExpectedToolCount);
    REQUIRE(tools.size() == ExpectedNames.size());
    REQUIRE(std::is_sorted(
        tools.begin(), tools.end(), [](const auto& left, const auto& right) {
            return left.tool.name < right.tool.name;
        }));

    Mcp::McpJsonCodec codec;
    std::size_t readEffects{};
    std::size_t writeEffects{};
    for (std::size_t index{}; index < tools.size(); ++index) {
        REQUIRE(tools[index].tool.name == ExpectedNames[index]);
        REQUIRE(!tools[index].tool.description.empty());
        REQUIRE(!tools[index].tool.pack.empty());
        REQUIRE(tools[index].tool.availability == Domain::ToolAvailability::Available);
        const auto canonical = take(codec.canonicalize(tools[index].inputSchema));
        REQUIRE(canonical == tools[index].inputSchema);
        const auto parsed = Json::parse(canonical);
        REQUIRE(parsed.is_object());
        REQUIRE(parsed.at("type") == "object");
        if (tools[index].tool.effect == Domain::ToolEffect::Read) {
            ++readEffects;
        } else if (tools[index].tool.effect == Domain::ToolEffect::Write) {
            ++writeEffects;
        }
    }
    REQUIRE(readEffects == 56U);
    REQUIRE(writeEffects == 69U);
    REQUIRE(descriptor(tools, "agent_run_status").tool.effect == Domain::ToolEffect::Write);
    REQUIRE(descriptor(tools, "agent_run_start").tool.requiresProject);
    REQUIRE(descriptor(tools, "agent_run_status").tool.requiresProject);
    REQUIRE(descriptor(tools, "instruction_package.read").tool.effect == Domain::ToolEffect::Read);
    REQUIRE(descriptor(tools, "fs_read").tool.description.find("next_offset") !=
        std::string::npos);
    REQUIRE(descriptor(tools, "session_handoff").tool.description.ends_with(
        "Prefer before context is full."));
    REQUIRE(descriptor(tools, "shell_exec").tool.description.find("PowerShell") !=
        std::string::npos);
    REQUIRE(descriptor(tools, "shell_exec").tool.requiresShell);
    const auto reviewerSchema = Json::parse(descriptor(tools, "reviewer_start").inputSchema);
    REQUIRE(reviewerSchema.at("required") == Json::array({"authorization"}));
    REQUIRE(reviewerSchema.at("oneOf").size() == 2U);
    REQUIRE(reviewerSchema.at("properties").at("opening_message").at("maxLength") == 65536);
    REQUIRE(reviewerSchema.at("properties").at("receive_timeout_sec").at("default") == 600);
    REQUIRE(reviewerSchema.at("properties").at("receive_timeout_sec").at("maximum") == 3600);
    const auto analysisSchema = Json::parse(descriptor(tools, "image_analyze").inputSchema);
    REQUIRE(descriptor(tools, "image_analyze").tool.effect == Domain::ToolEffect::Write);
    REQUIRE(descriptor(tools, "image_analyze").tool.requiresProject);
    REQUIRE(!descriptor(tools, "image_analyze").tool.requiresShell);
    REQUIRE(analysisSchema.at("additionalProperties") == false);
    REQUIRE(analysisSchema.at("required") == Json::array({"path", "authorization"}));
    REQUIRE(analysisSchema.at("properties").at("question").at("maxLength") == 4096);
    REQUIRE(analysisSchema.at("properties").at("authorization").at("maxLength") == 1024);
    REQUIRE(analysisSchema.at("properties").at("receive_timeout_sec").at("default") == 600);
    REQUIRE(analysisSchema.at("properties").at("receive_timeout_sec").at("maximum") == 3600);
    for (const auto toolName : {"image_generate", "image_edit"}) {
        const auto provider = schema(tools, toolName);
        REQUIRE(descriptor(tools, toolName).tool.effect == Domain::ToolEffect::Write);
        REQUIRE(descriptor(tools, toolName).tool.requiresProject && !descriptor(tools, toolName).tool.requiresShell);
        REQUIRE(provider.at("additionalProperties") == false);
        REQUIRE(provider.at("properties").at("prompt").at("minLength") == 1);
        REQUIRE(provider.at("properties").at("prompt").at("maxLength") == 4096);
        REQUIRE(provider.at("properties").at("seed").at("minimum") == 0);
        REQUIRE(provider.at("properties").at("seed").at("maximum") == 9007199254740991LL);
        REQUIRE(provider.at("properties").at("steps").at("default") == 20);
        REQUIRE(provider.at("properties").at("cfg").at("default") == 7);
        REQUIRE(provider.at("properties").at("denoise").at("minimum") == 0.05);
        REQUIRE(provider.at("properties").at("denoise").at("default") == 1.0);
        REQUIRE(provider.at("properties").at("timeout_sec").at("default") == 1800);
        REQUIRE(provider.at("properties").at("timeout_sec").at("maximum") == 3600);
        REQUIRE(!provider.at("properties").contains("workflow") && !provider.at("properties").contains("provider"));
        auto required = Json::array({"prompt", "path", "seed", "width", "height"});
        if (std::string_view{toolName} == "image_edit") required.push_back("source_path");
        REQUIRE(provider.at("required") == required);
    }
    for (const auto toolName : {"image_provider_status", "image_job_status", "image_job_cancel", "image_job_resume"}) {
        const auto provider = schema(tools, toolName);
        const auto effect = std::string_view{toolName} == "image_provider_status" || std::string_view{toolName} == "image_job_status"
            ? Domain::ToolEffect::Read : Domain::ToolEffect::Write;
        REQUIRE(descriptor(tools, toolName).tool.effect == effect);
        REQUIRE(provider.at("additionalProperties") == false);
        REQUIRE(provider.at("required") == (std::string_view{toolName} == "image_provider_status" ? Json::array() : Json::array({"job_id"})));
    }
    REQUIRE(schema(tools, "image_job_status").at("properties").at("wait_sec").at("maximum") == 60);
    REQUIRE(schema(tools, "image_job_resume").at("properties").size() == 1U);
    REQUIRE(schema(tools, "image_read").at("required") == Json::array({"path"}));
    const auto sampleSchema = schema(tools, "image_read").at("properties").at("samples");
    REQUIRE(sampleSchema.at("type") == "array" && sampleSchema.at("minItems") == 1 && sampleSchema.at("maxItems") == 64);
    REQUIRE(sampleSchema.at("items").at("required") == Json::array({"x", "y"}));
    REQUIRE(sampleSchema.at("items").at("additionalProperties") == false);
    for (const auto coordinate : {"x", "y"}) {
        const auto bounds = sampleSchema.at("items").at("properties").at(coordinate);
        REQUIRE(bounds.at("type") == "integer" && bounds.at("minimum") == 0 && bounds.at("maximum") == 4095);
    }
    const auto desktopReadSchema = schema(tools, "desktop_read");
    REQUIRE(desktopReadSchema.at("required") == Json::array({"window_id", "pid"}));
    REQUIRE(desktopReadSchema.at("additionalProperties") == false);
    REQUIRE(desktopReadSchema.at("properties").at("offset").at("minimum") == 0);
    REQUIRE(desktopReadSchema.at("properties").at("offset").at("maximum") == 2147483647);
    REQUIRE(desktopReadSchema.at("properties").at("limit").at("maximum") == 300);
    REQUIRE(descriptor(tools, "desktop_read").tool.description.find("next_offset") != std::string::npos);
    const auto run = schema(tools, "comfy_run");
    // LM Studio's installed bridge forwards tools/list descriptions to its
    // model tool definitions; routing must not rely only on initialize text.
    for (const auto* name : {"comfy_status", "image_generate", "image_provider_status"}) {
        REQUIRE(descriptor(tools, name).tool.description.find("comfy_run") != std::string::npos);
        REQUIRE(descriptor(tools, name).tool.description.find("enabled") != std::string::npos);
    }
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("including ordinary images") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("approval_reply_choices") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("[source_node_id, output_index]") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("checkpoint/model names") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("output node IDs") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Read comfy_status for effective limits and configuration.quality_preference; honor that default when quality is omitted") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("When video duration is omitted, propose a motion draft of about five seconds") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Choose playable MP4 with H.264 for final video using discovered output node contracts unless the operator requests a supported alternative format or codec") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("save the format in the final graph before preview approval; a later format change requires a new preview and approval") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Do not apply a universal output-format rewrite to unknown graphs") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("For an image preview, call image_read with the verified preview image artifact.path to display its larger bounded image before requesting approval; the job-status thumbnail alone is insufficient") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Successful decoding verifies readable media, not the requested subject or motion quality") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("call image_analyze with the path of the published artifact whose role is sampled_video_contact_sheet, then poll reviewer_status for actual findings") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("For a verified video preview or final artifact with provider_view_url, call browser_open with that exact URL while ComfyUI is running") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("then use desktop_read on the observed browser window to check its actual address") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Launch acceptance alone does not verify page loading or playback") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Report actual browser-launch or observation failures") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Present the URL as a copyable reference and the complete artifacts[].path in a copyable fenced block") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("ordinary Markdown link path rejects loopback URLs") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Retain the sampled-frame preview") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("Report unavailable contact sheets and review failures with their actual errors") != std::string::npos);
    REQUIRE(descriptor(tools, "comfy_run").tool.description.find("limit visual claims to reviewed sampled frames and do not claim unverified motion quality or repeat the prompt as observed content") != std::string::npos);
    for (const auto* key : {"expected_outputs", "preview_expected_outputs", "final_expected_outputs"}) {
        const auto& outputs = run.at("properties").at(key);
        REQUIRE(outputs.at("description").get<std::string>().find("output node IDs") != std::string::npos);
        REQUIRE(outputs.at("examples") == Json::array({Json::array({"25"})}));
    }
    const auto validateOutputs = schema(tools, "comfy_validate").at("properties").at("expected_outputs");
    REQUIRE(validateOutputs.at("description").get<std::string>().find("output node IDs") != std::string::npos);
    REQUIRE(validateOutputs.at("examples") == Json::array({Json::array({"25"})}));
    REQUIRE(descriptor(tools, "comfy_workflow").tool.description.find("returned workflow is executable API JSON") != std::string::npos);
    REQUIRE(run.at("additionalProperties") == false && run.at("required") == Json::array({"stage"}));
    REQUIRE(run.at("properties").at("stage").at("enum") == Json::array({"preview", "final"}));
    REQUIRE(run.at("properties").at("media_kind").at("enum") == Json::array({"image", "video", "mixed"}));
    REQUIRE(run.at("oneOf").at(0).at("required") == Json::array({"stage", "media_kind", "preview_workflow", "final_workflow", "output_directory"}));
    REQUIRE(run.at("oneOf").at(1).at("additionalProperties") == false);
    REQUIRE(!run.at("properties").contains("approved") && !run.at("properties").contains("approval"));
    const auto input = run.at("properties").at("inputs").at("items");
    REQUIRE(input.at("additionalProperties") == false && input.at("required") == Json::array({"node_id", "path"}));
    REQUIRE(schema(tools, "comfy_job_status").at("properties").at("wait_sec").at("maximum") == 60);
    REQUIRE(schema(tools, "comfy_catalog").at("properties").at("kind").at("enum") ==
        Json::array({"nodes", "models", "workflows", "templates"}));
    REQUIRE(schema(tools, "comfy_catalog").at("properties").at("offset").at("maximum") == 100000);
    for (const auto toolName : {"comfy_control", "comfy_prepare", "comfy_run"})
        REQUIRE(schema(tools, toolName).at("properties").at("timeout_sec").at("maximum") == 7200);
    REQUIRE(descriptor(tools, "comfy_job_status").tool.effect == Domain::ToolEffect::Read);
    REQUIRE(descriptor(tools, "comfy_run").tool.effect == Domain::ToolEffect::Write);
    REQUIRE(schema(tools, "desktop_scroll").at("properties").at("delta").at("minimum") == -12000);
    REQUIRE(schema(tools, "desktop_drag").at("properties").at("duration_ms").at("default") == 500);

    for (const auto toolName : {"image_read", "image_write", "desktop_capture", "image_analyze", "image_generate", "image_edit"}) {
        const auto imageSchema = schema(tools, toolName);
        const auto& dimension = imageSchema.at("properties").at("preview_max_dimension");
        REQUIRE(dimension.at("minimum") == 128 && dimension.at("maximum") == 2048 && dimension.at("default") == 256);
        REQUIRE(imageSchema.at("additionalProperties") == false);
        REQUIRE(std::find(imageSchema.at("required").begin(), imageSchema.at("required").end(), "preview_max_dimension") ==
            imageSchema.at("required").end());
    }
}

void testSourceSchemasAndWindowsDelta()
{
    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();

    REQUIRE(schema(tools, "clu.findings") == Json({
        {"additionalProperties", false},
        {"properties", Json::object()},
        {"required", Json::array()},
        {"type", "object"}}));
    REQUIRE(schema(tools, "clu.export_log") == schema(tools, "clu.findings"));
    const auto cluEvaluate = schema(tools, "clu.evaluate");
    REQUIRE(cluEvaluate.at("additionalProperties") == false);
    REQUIRE(cluEvaluate.at("required") == Json::array({"evidence"}));
    REQUIRE(cluEvaluate.at("properties").at("evidence").at("type") == "object");
    const auto cluResolve = schema(tools, "clu.resolve");
    REQUIRE(cluResolve.at("additionalProperties") == false);
    REQUIRE(cluResolve.at("required") ==
        Json::array({"finding_id", "correction_evidence"}));
    REQUIRE(cluResolve.at("properties").at("finding_id").at("maxLength") == 128U);

    const auto agentList = schema(tools, "agent_list");
    REQUIRE(agentList == Json({
        {"type", "object"},
        {"properties", Json::object()},
        {"additionalProperties", true}}));

    const auto status = schema(tools, "agent_run_status");
    REQUIRE(status.at("required") == Json::array({"session_id"}));
    REQUIRE(status.at("properties").at("report").at("type") == "object");
    REQUIRE(!status.contains("additionalProperties"));

    const auto fsEdit = schema(tools, "fs_edit");
    REQUIRE(fsEdit.at("required") == Json::array({"path", "old", "new"}));
    REQUIRE(fsEdit.at("properties").at("path").at("type") == "string");
    REQUIRE(fsEdit.at("properties").at("old").at("type") == "string");
    REQUIRE(fsEdit.at("properties").at("new").at("type") == "string");
    REQUIRE(fsEdit.at("additionalProperties") == true);
    const auto fsGlob = schema(tools, "fs_glob");
    REQUIRE(fsGlob.at("required").empty());
    REQUIRE(fsGlob.at("properties").at("pattern").at("type") == "string");
    const auto fsMove = schema(tools, "fs_move");
    REQUIRE(fsMove.at("required") == Json::array({"path", "dest"}));
    REQUIRE(fsMove.at("properties").at("src").at("type") == "string");
    const auto fsRead = schema(tools, "fs_read");
    REQUIRE(fsRead.at("required") == Json::array({"path"}));
    REQUIRE(fsRead.at("properties").at("offset").at("description") ==
        "1-based start line for a partial read");
    REQUIRE(fsRead.at("properties").at("byte_offset").at("description") ==
        "UTF-8 byte offset returned by next_byte_offset for an oversized line");
    REQUIRE(fsRead.at("properties").at("length").at("description") ==
        "Number of lines to return (alias: limit)");

    const auto memorySet = schema(tools, "memory_set");
    REQUIRE(memorySet.at("required") == Json::array({"key", "body"}));
    REQUIRE(memorySet.at("properties").at("content").at("description") ==
        "Alias of body");

    const auto batch = schema(tools, "project_memory.remember_batch");
    REQUIRE(batch.at("additionalProperties") == false);
    REQUIRE(batch.at("properties").at("items").at("maxItems") == 50U);

    const auto search = schema(tools, "project_memory.search");
    REQUIRE(search.at("additionalProperties") == false);
    REQUIRE(search.at("properties").at("kinds").at("maxItems") ==
        Mcp::McpToolCatalog::MaximumProjectMemoryKinds);
    REQUIRE(!search.at("properties").at("tags").contains("maxItems"));

    const auto recent = schema(tools, "project_memory.list_recent");
    REQUIRE(recent.at("properties").at("kinds").at("maxItems") == 100U);
    REQUIRE(std::none_of(tools.begin(), tools.end(), [](const auto& item) {
        return item.tool.name == "project_memory.export" || item.tool.name == "project_memory.import";
    }));
    const auto package = schema(tools, "instruction_package.read");
    REQUIRE(package.at("required") == Json::array({"queue_row_id"}));
    REQUIRE(package.at("properties").at("offset").at("minimum") == 0);

    const auto checkpoint = schema(tools, "continuity.checkpoint");
    REQUIRE(checkpoint.at("additionalProperties") == false);
    REQUIRE(checkpoint.at("required") ==
        Json::array({"project_id", "predecessor_session_id", "mission"}));
    REQUIRE(checkpoint.at("properties").at("constraints").at("maxItems") == 128U);
    REQUIRE(checkpoint.at("properties").at("next_actions").at("maxItems") == 128U);

    const auto testRun = schema(tools, "cmake_test_run");
    REQUIRE(testRun.at("additionalProperties") == false);
    REQUIRE(testRun.at("required") == Json::array({"build_dir"}));
    REQUIRE(testRun.at("properties").at("mode").at("default") == "test");
    REQUIRE(testRun.at("properties").at("timeout_sec").at("default") == 1800);
    REQUIRE(testRun.at("properties").at("timeout_sec").at("maximum") == 3600);
    REQUIRE(descriptor(tools, "cmake_test_run").tool.effect == Domain::ToolEffect::Write);
    REQUIRE(descriptor(tools, "cmake_test_run").tool.requiresShell);
    REQUIRE(descriptor(tools, "cmake_test_run").tool.requiresProject);
    REQUIRE(descriptor(tools, "cmake_test_status").tool.effect == Domain::ToolEffect::Read);
    REQUIRE(!descriptor(tools, "cmake_test_status").tool.requiresShell);
    const auto testStatus = schema(tools, "cmake_test_status");
    REQUIRE(testStatus.at("required") == Json::array({"job_id"}));
    REQUIRE(testStatus.at("properties").at("max_failures").at("default") == 16);
    REQUIRE(testStatus.at("properties").at("max_failures").at("maximum") == 32);

    const auto shell = schema(tools, "shell_exec");
    REQUIRE(shell.at("properties").at("timeout_sec").at("exclusiveMinimum") == 0);
    REQUIRE(shell.at("properties").at("timeout_sec").at("maximum") == 120);
    const auto jobStart = schema(tools, "shell_job_start");
    REQUIRE(jobStart.at("additionalProperties") == false);
    REQUIRE(jobStart.at("required") == Json::array({"command"}));
    REQUIRE(jobStart.at("properties").at("timeout_sec").at("maximum") == 3600);
    REQUIRE(schema(tools, "shell_job_status").at("required") == Json::array({"job_id"}));
    REQUIRE(schema(tools, "shell_job_cancel") == schema(tools, "shell_job_status"));
    REQUIRE(descriptor(tools, "shell_job_start").tool.requiresShell);
    REQUIRE(!descriptor(tools, "shell_job_status").tool.requiresShell);
}

void testDescriptorValidation()
{
    auto catalog = take(Mcp::McpToolCatalog::create());
    const auto tools = catalog->tools();
    REQUIRE(Mcp::McpToolCatalog::validateDescriptors(tools).hasValue());

    std::vector<Domain::McpToolDescriptor> shortened{tools.begin(), tools.end()};
    shortened.pop_back();
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(shortened).hasValue());

    std::vector<Domain::McpToolDescriptor> duplicated{tools.begin(), tools.end()};
    duplicated[1] = duplicated[0];
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(duplicated).hasValue());

    std::vector<Domain::McpToolDescriptor> unsorted{tools.begin(), tools.end()};
    std::swap(unsorted[0], unsorted[1]);
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(unsorted).hasValue());

    std::vector<Domain::McpToolDescriptor> invalidSchema{tools.begin(), tools.end()};
    invalidSchema[0].inputSchema = "[]";
    REQUIRE(!Mcp::McpToolCatalog::validateDescriptors(invalidSchema).hasValue());
}

void testCanonicalJsonCodec()
{
    const Mcp::McpJsonCodec codec;
    REQUIRE(take(codec.canonicalize(
        R"({"z":1,"a":{"y":2,"b":3},"m":[{"d":4,"c":5}]})")) ==
        R"({"a":{"b":3,"y":2},"m":[{"c":5,"d":4}],"z":1})");
    REQUIRE(take(codec.canonicalize(" { \"values\": [ true, null, 2 ] } ")) ==
        "{\"values\":[true,null,2]}");
    REQUIRE(!codec.canonicalize("[true,null,2]").hasValue());
    REQUIRE(!codec.canonicalize(R"({"a":1,"a":2})").hasValue());
    REQUIRE(!codec.canonicalize(R"({"a":{"b":1,"b":2}})").hasValue());
    REQUIRE(!codec.canonicalize(R"({"value":"\u0000"})").hasValue());
    REQUIRE(!codec.canonicalize("{").hasValue());
    REQUIRE(!codec.canonicalize("").hasValue());
    REQUIRE(!codec.canonicalize(
        std::string(Mcp::McpJsonCodec::MaximumDocumentBytes + 1U, 'x')).hasValue());

    std::string tooDeep;
    for (std::size_t index{};
         index < Mcp::McpJsonCodec::MaximumNestingDepth + 2U;
         ++index) {
        tooDeep.append("{\"a\":");
    }
    tooDeep.append("0");
    tooDeep.append(Mcp::McpJsonCodec::MaximumNestingDepth + 2U, '}');
    REQUIRE(!codec.canonicalize(tooDeep).hasValue());
}

void testProtocolNegotiation()
{
    REQUIRE(Mcp::McpProtocol::SupportedVersions.size() == 4U);
    for (const auto version : Mcp::McpProtocol::SupportedVersions) {
        REQUIRE(Mcp::McpProtocol::negotiate(version) == version);
    }
    REQUIRE(Mcp::McpProtocol::negotiate(" 2024-11-05\r\n") == "2024-11-05");
    REQUIRE(Mcp::McpProtocol::negotiate("") == "2025-11-25");
    REQUIRE(Mcp::McpProtocol::negotiate("2099-01-01") == "2025-11-25");
}

} // namespace

int main()
{
    try {
        testCanonicalCatalog();
        testSourceSchemasAndWindowsDelta();
        testDescriptorValidation();
        testCanonicalJsonCodec();
        testProtocolNegotiation();
        std::cout << "MCP catalog/codec tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP catalog/codec tests failed after " << assertions
                  << " assertions: " << error.what() << '\n';
        return 1;
    }
}
