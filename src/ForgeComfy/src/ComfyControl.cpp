// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeComfy/ComfyControl.h"

#include "ForgePlatform/HttpUrl.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <thread>

namespace Forge::Comfy {
namespace {

constexpr const char* kOwnedComfyId = "owned-comfyui";

std::string actionOf(const nlohmann::json& arguments) {
    if (arguments.contains("action") && arguments["action"].is_string()) {
        return arguments["action"].get<std::string>();
    }
    return {};
}

nlohmann::json argsObject(const nlohmann::json& arguments) {
    if (arguments.contains("arguments") && arguments["arguments"].is_object()) {
        return arguments["arguments"];
    }
    return nlohmann::json::object();
}

Domain::ToolResult okJson(nlohmann::json payload) {
    auto result = Domain::ToolResult::success();
    result.payload["result"] = payload.dump();
    return result;
}

bool looksLikeApiGraph(const nlohmann::json& value) {
    if (!value.is_object() || value.empty()) {
        return false;
    }
    if (value.contains("nodes") && value["nodes"].is_array()) {
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it.value().is_object()) {
            return false;
        }
        if (!it.value().contains("class_type")) {
            return false;
        }
    }
    return true;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isVideoWorkflowName(const std::string& name) {
    const auto n = lower(name);
    return n.find("wan") != std::string::npos || n.find("hunyuan") != std::string::npos ||
        n.find("i2v") != std::string::npos || n.find("video") != std::string::npos ||
        n.find("ltx") != std::string::npos || n.find("svd") != std::string::npos ||
        n.find("animate") != std::string::npos;
}

const std::vector<std::string> kSubmitActions{
    "execute",
    "render_simple",
    "vram_acquire",
    "vram_handoff",
    "submit",
    "render_batch",
    "render_shot",
};

bool isSubmitAction(const std::string& action) {
    return std::find(kSubmitActions.begin(), kSubmitActions.end(), action) != kSubmitActions.end();
}

std::string schemaGroup(const std::vector<std::string>& actions) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties",
            {{"action", {{"type", "string"}, {"enum", actions}}},
             {"arguments", {{"type", "object"}}}}},
        {"required", nlohmann::json::array({"action"})},
    };
    return schema.dump();
}

} // namespace

ComfyControl::ComfyControl(
    Persistence::AppPaths paths,
    ComfySettings settings,
    std::shared_ptr<Platform::IHttpClient> http,
    std::shared_ptr<IProcessLauncher> launcher,
    std::shared_ptr<IProcessInspector> inspector,
    std::shared_ptr<Domain::IClock> clock)
    : paths_(std::move(paths))
    , settings_(std::move(settings))
    , http_(std::move(http))
    , launcher_(std::move(launcher))
    , inspector_(std::move(inspector))
    , clock_(std::move(clock))
    , store_(paths_.comfySQLite())
    , client_(settings_.baseUrl, *http_) {}

std::shared_ptr<ComfyControl> ComfyControl::create(const Persistence::AppPaths& paths) {
    auto settings = loadComfySettings(paths);
    return std::make_shared<ComfyControl>(
        paths,
        std::move(settings),
        std::make_shared<Platform::WinHttpClient>(),
        std::make_shared<Win32ProcessLauncher>(),
        std::make_shared<Win32ProcessInspector>());
}

Domain::DoctorReport ComfyControl::status() {
    return doctor();
}

Domain::DoctorReport ComfyControl::doctor() {
    Domain::DoctorReport report;
    auto check = [&](const std::string& name, bool ok, const std::string& detail, bool hard = true) {
        if (hard && !ok) {
            report.ok = false;
        }
        report.checks.push_back({name, ok, detail, hard});
    };
    check("comfy_enabled", settings_.enabled, settings_.enabled ? "true" : "false");
    check("execution_policy", settings_.executionPolicy == "prepare_only" || settings_.executionPolicy == "full",
        settings_.executionPolicy);
    check("transport", settings_.transport == "loopback", settings_.transport);
    try {
        const auto url = Platform::validateLoopbackHttpBaseUrl(settings_.baseUrl, true);
        check("base_url", true, url);
    } catch (const std::exception& ex) {
        check("base_url", false, ex.what());
    }
    check("comfy_root", std::filesystem::is_directory(settings_.comfyRoot), settings_.comfyRoot.string());
    check("comfy_main", std::filesystem::is_regular_file(settings_.mainPy()), settings_.mainPy().string());
    check("comfy_python", std::filesystem::is_regular_file(settings_.comfyPython), settings_.comfyPython.string(), false);
    check("comfy_sqlite", std::filesystem::exists(paths_.comfySQLite()), paths_.comfySQLite().string(), false);
    nlohmann::json stats;
    check("comfy_reachable", client_.trySystemStats(stats), settings_.baseUrl, false);
    return report;
}

std::vector<Domain::ComfyToolSpec> ComfyControl::tools() const {
    std::vector<Domain::ComfyToolSpec> out;
    out.push_back({
        "comfy_prepare_video",
        "Set up ComfyUI for video. Starts ComfyUI if needed, discovers nodes, locates or imports API workflows, and returns next steps. Does not submit GPU jobs while execution_policy is prepare_only.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
                {{"prompt", {{"type", "string"}}},
                 {"concept", {{"type", "string"}}},
                 {"workflow_id", {{"type", "string"}}},
                 {"api_prompt", {{"type", "object"}}},
                 {"start_comfy", {{"type", "boolean"}}}}},
        }.dump(),
    });
    out.push_back({
        "comfy_system",
        "Discover and control loopback ComfyUI. Actions: health, discover, configuration, start, stop, processes. GPU submit and VRAM handoff are omitted in prepare_only.",
        schemaGroup({"health", "discover", "configuration", "start", "stop", "processes"}),
    });
    out.push_back({
        "comfy_nodes",
        "Inspect installed ComfyUI node types. Actions: features, list, search, describe, schemas.",
        schemaGroup({"features", "list", "search", "describe", "schemas"}),
    });
    out.push_back({
        "comfy_workflow",
        "Import, inspect, validate, and prepare API-format ComfyUI workflows. Actions: list, inspect, import_api, validate, prepare. execute is rejected in prepare_only.",
        schemaGroup({"list", "inspect", "import_api", "validate", "prepare"}),
    });
    return out;
}

Domain::ToolResult ComfyControl::call(const std::string& name, const std::string& argumentsJson) {
    nlohmann::json arguments = nlohmann::json::object();
    if (!argumentsJson.empty()) {
        try {
            arguments = nlohmann::json::parse(argumentsJson);
            if (!arguments.is_object()) {
                arguments = nlohmann::json::object();
            }
        } catch (...) {
            return Domain::ToolResult::failure("invalid_arguments", "arguments must be a JSON object");
        }
    }
    try {
        if (name == "comfy_prepare_video") {
            return prepareVideo(arguments);
        }
        if (name == "comfy_system") {
            return dispatchSystem(arguments);
        }
        if (name == "comfy_nodes") {
            return dispatchNodes(arguments);
        }
        if (name == "comfy_workflow") {
            return dispatchWorkflow(arguments);
        }
        if (name == "comfy_render_simple") {
            return rejected("comfy_render_simple");
        }
        return Domain::ToolResult::failure("unknown_tool", "No comfy tool named " + name);
    } catch (const std::exception& ex) {
        return Domain::ToolResult::failure("comfy_error", ex.what());
    }
}

std::optional<Domain::PrepareSession> ComfyControl::lastPrepareSession() const {
    return store_.lastPrepareSession();
}

Domain::ToolResult ComfyControl::rejected(const std::string& action) const {
    return Domain::ToolResult::failure(
        "prepare_only",
        "Action '" + action + "' is blocked while execution_policy=prepare_only. Call comfy_prepare_video, then tell the user to free VRAM and queue in ComfyUI.");
}

nlohmann::json ComfyControl::configurationJson() const {
    return {
        {"enabled", settings_.enabled},
        {"execution_policy", settings_.executionPolicy},
        {"transport", settings_.transport},
        {"base_url", settings_.baseUrl},
        {"comfy_root", settings_.comfyRoot.string()},
        {"comfy_python", settings_.comfyPython.string()},
        {"comfy_output", settings_.comfyOutput.string()},
    };
}

bool ComfyControl::ensureReachable(bool startIfNeeded, std::string& error) {
    nlohmann::json stats;
    if (client_.trySystemStats(stats)) {
        return true;
    }
    if (!startIfNeeded) {
        error = "ComfyUI is not reachable at " + settings_.baseUrl;
        return false;
    }
    if (!std::filesystem::is_regular_file(settings_.comfyPython) || !std::filesystem::is_regular_file(settings_.mainPy())) {
        error = "ComfyUI python or main.py was not found";
        return false;
    }
    const auto parsed = Platform::parseHttpUrl(settings_.baseUrl);
    const int port = parsed ? parsed->port : 8188;
    auto identity = launcher_->start(
        settings_.comfyPython,
        {"-s", settings_.mainPy().string(), "--listen", "127.0.0.1", "--port", std::to_string(port)},
        settings_.comfyRoot);
    store_.saveProcess(kOwnedComfyId, "comfyui", identity);
    for (int i = 0; i < 120; ++i) {
        if (client_.trySystemStats(stats)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    error = "started ComfyUI but it did not become reachable at " + settings_.baseUrl;
    return false;
}

std::vector<std::filesystem::path> ComfyControl::findVideoWorkflows() const {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    const auto dir = settings_.workflowsDir();
    if (!std::filesystem::is_directory(dir, ec)) {
        return found;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        if (isVideoWorkflowName(entry.path().filename().string())) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<std::string> ComfyControl::videoNodeHints(const nlohmann::json& objectInfo) const {
    std::vector<std::string> hints;
    if (!objectInfo.is_object()) {
        return hints;
    }
    for (auto it = objectInfo.begin(); it != objectInfo.end(); ++it) {
        const auto key = lower(it.key());
        if (key.find("wan") != std::string::npos || key.find("hunyuan") != std::string::npos ||
            key.find("ltx") != std::string::npos || key.find("video") != std::string::npos ||
            key.find("svd") != std::string::npos || key.find("animate") != std::string::npos ||
            key.find("vhs_") != std::string::npos) {
            hints.push_back(it.key());
        }
    }
    std::sort(hints.begin(), hints.end());
    if (hints.size() > 24) {
        hints.resize(24);
    }
    return hints;
}

Domain::ToolResult ComfyControl::dispatchSystem(const nlohmann::json& arguments) {
    const auto action = actionOf(arguments);
    if (isSubmitAction(action) || action == "raw_request") {
        return rejected(action);
    }
    if (action == "health" || action.empty()) {
        nlohmann::json stats;
        const bool reachable = client_.trySystemStats(stats);
        return okJson({
            {"reachable", reachable},
            {"execution_policy", settings_.executionPolicy},
            {"base_url", settings_.baseUrl},
            {"system_stats", reachable ? stats : nlohmann::json::object()},
        });
    }
    if (action == "configuration") {
        return okJson(configurationJson());
    }
    if (action == "discover") {
        const auto caps = client_.discover();
        nlohmann::json routes = nlohmann::json::array();
        for (const auto& [op, path] : caps.routeSelections) {
            routes.push_back({{"operation", op}, {"path", path}});
        }
        return okJson({
            {"reachable", caps.reachable},
            {"base_url", caps.baseUrl},
            {"object_info_available", caps.objectInfoAvailable},
            {"route_selections", routes},
            {"video_nodes", videoNodeHints(caps.objectInfo)},
        });
    }
    if (action == "processes") {
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& [id, identity] : store_.listProcesses()) {
            rows.push_back({
                {"id", id},
                {"pid", identity.pid},
                {"executable", identity.executable},
                {"command_line", identity.commandLine},
            });
        }
        return okJson({{"processes", rows}});
    }
    if (action == "start") {
        std::string error;
        if (!ensureReachable(true, error)) {
            return Domain::ToolResult::failure("comfy_start_failed", error);
        }
        return okJson({{"reachable", true}, {"base_url", settings_.baseUrl}});
    }
    if (action == "stop") {
        auto owned = store_.loadProcess(kOwnedComfyId);
        if (!owned) {
            return Domain::ToolResult::failure("not_owned", "No controller-owned ComfyUI process is recorded");
        }
        inspector_->terminate(*owned);
        store_.deleteProcess(kOwnedComfyId);
        return okJson({{"stopped", true}, {"pid", owned->pid}});
    }
    return Domain::ToolResult::failure("unknown_action", "unknown comfy_system action: " + action);
}

Domain::ToolResult ComfyControl::dispatchNodes(const nlohmann::json& arguments) {
    const auto action = actionOf(arguments);
    const auto info = client_.objectInfo();
    std::vector<std::string> names;
    for (auto it = info.begin(); it != info.end(); ++it) {
        names.push_back(it.key());
    }
    std::sort(names.begin(), names.end());
    if (action == "features" || action == "capabilities") {
        return okJson({
            {"node_count", names.size()},
            {"video_nodes", videoNodeHints(info)},
        });
    }
    if (action == "list" || action.empty()) {
        return okJson({{"node_types", names}});
    }
    if (action == "search") {
        const auto args = argsObject(arguments);
        const auto query = lower(args.value("query", ""));
        nlohmann::json matches = nlohmann::json::array();
        for (const auto& name : names) {
            if (query.empty() || lower(name).find(query) != std::string::npos) {
                matches.push_back(name);
            }
        }
        return okJson({{"matches", matches}});
    }
    if (action == "describe") {
        const auto args = argsObject(arguments);
        const auto nodeType = args.value("node_type", "");
        if (!info.contains(nodeType)) {
            return Domain::ToolResult::failure("not_found", "unknown node type: " + nodeType);
        }
        return okJson({{"node_type", nodeType}, {"schema", info[nodeType]}});
    }
    if (action == "schemas" || action == "object_info") {
        return okJson({{"object_info", info}});
    }
    return Domain::ToolResult::failure("unknown_action", "unknown comfy_nodes action: " + action);
}

Domain::ToolResult ComfyControl::dispatchWorkflow(const nlohmann::json& arguments) {
    const auto action = actionOf(arguments);
    if (isSubmitAction(action)) {
        return rejected(action);
    }
    const auto args = argsObject(arguments);
    const auto now = Domain::iso8601(clock_->now());
    if (action == "list") {
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& record : store_.listWorkflows()) {
            rows.push_back({
                {"workflow_id", record.id},
                {"name", record.name},
                {"revision", record.revision},
                {"api_hash", record.apiHash},
            });
        }
        return okJson({{"workflows", rows}});
    }
    if (action == "inspect") {
        const auto id = args.value("workflow_id", "");
        const auto record = store_.getWorkflow(id);
        if (!record) {
            return Domain::ToolResult::failure("not_found", "unknown workflow_id");
        }
        return okJson({
            {"workflow_id", record->id},
            {"name", record->name},
            {"revision", record->revision},
            {"api_hash", record->apiHash},
            {"api_prompt", nlohmann::json::parse(record->apiJson)},
        });
    }
    if (action == "import_api" || action == "import") {
        nlohmann::json api = args.contains("api_prompt") ? args["api_prompt"] : nlohmann::json::object();
        if (!looksLikeApiGraph(api)) {
            return Domain::ToolResult::failure(
                "ui_graph_unsupported",
                "Only API-format prompts (class_type nodes) can be imported in this phase. Export API format from ComfyUI.");
        }
        const auto name = args.value("name", "imported");
        const auto record = store_.saveWorkflow(name, api.dump(), now);
        return okJson({
            {"workflow_id", record.id},
            {"name", record.name},
            {"api_hash", record.apiHash},
        });
    }
    if (action == "validate" || action == "prepare") {
        const auto id = args.value("workflow_id", "");
        const auto record = store_.getWorkflow(id);
        if (!record) {
            return Domain::ToolResult::failure("not_found", "unknown workflow_id");
        }
        const auto api = nlohmann::json::parse(record->apiJson);
        const auto info = client_.objectInfo();
        std::vector<std::string> missing;
        for (auto it = api.begin(); it != api.end(); ++it) {
            const auto classType = it.value().value("class_type", "");
            if (!info.contains(classType)) {
                missing.push_back(classType);
            }
        }
        nlohmann::json payload = {
            {"workflow_id", record->id},
            {"valid", missing.empty()},
            {"missing_nodes", missing},
        };
        if (action == "prepare") {
            payload["prompt"] = api;
            payload["submitted"] = false;
        }
        if (!missing.empty()) {
            auto result = okJson(payload);
            result.ok = true;
            return result;
        }
        return okJson(payload);
    }
    return Domain::ToolResult::failure("unknown_action", "unknown comfy_workflow action: " + action);
}

Domain::ToolResult ComfyControl::prepareVideo(const nlohmann::json& arguments) {
    if (settings_.executionPolicy != "prepare_only" && settings_.executionPolicy != "full") {
        return Domain::ToolResult::failure("bad_policy", "invalid execution_policy");
    }
    const bool startComfy = arguments.value("start_comfy", true);
    std::string error;
    if (!ensureReachable(startComfy, error)) {
        return Domain::ToolResult::failure("comfy_unavailable", error);
    }
    const auto caps = client_.discover();
    nlohmann::json apiPrompt = nullptr;
    if (arguments.contains("api_prompt") && arguments["api_prompt"].is_object()) {
        apiPrompt = arguments["api_prompt"];
    }
    std::vector<std::string> workflowIds;
    std::vector<std::string> missing;
    if (!apiPrompt.is_null()) {
        if (!looksLikeApiGraph(apiPrompt)) {
            return Domain::ToolResult::failure(
                "ui_graph_unsupported",
                "api_prompt must be an API-format graph with class_type on every node.");
        }
        const auto record = store_.saveWorkflow(
            arguments.value("name", "prepared-video"),
            apiPrompt.dump(),
            Domain::iso8601(clock_->now()));
        workflowIds.push_back(record.id);
        if (caps.objectInfoAvailable) {
            for (auto it = apiPrompt.begin(); it != apiPrompt.end(); ++it) {
                const auto classType = it.value().value("class_type", "");
                if (!caps.objectInfo.contains(classType)) {
                    missing.push_back(classType);
                }
            }
        }
    } else if (arguments.contains("workflow_id") && arguments["workflow_id"].is_string()) {
        const auto record = store_.getWorkflow(arguments["workflow_id"].get<std::string>());
        if (!record) {
            return Domain::ToolResult::failure("not_found", "unknown workflow_id");
        }
        workflowIds.push_back(record->id);
        apiPrompt = nlohmann::json::parse(record->apiJson);
    }

    const auto files = findVideoWorkflows();
    std::vector<std::string> fileNames;
    for (const auto& path : files) {
        fileNames.push_back(path.filename().string());
    }
    const auto hints = videoNodeHints(caps.objectInfo);
    const bool ready = caps.reachable && missing.empty() && (!fileNames.empty() || !workflowIds.empty() || !hints.empty());

    Domain::PrepareSession session;
    session.id = Domain::makeUuid();
    session.createdAt = Domain::iso8601(clock_->now());
    session.ready = ready;
    session.executionPolicy = settings_.executionPolicy;
    session.comfyUrl = settings_.baseUrl;
    session.workflowIds = workflowIds;
    session.workflowFiles = fileNames;
    session.missingNodes = missing;
    if (ready) {
        session.userMessage =
            "ComfyUI is ready to create video. This machine cannot render while LM Studio is using the GPU.";
    } else {
        session.userMessage =
            "ComfyUI setup is incomplete. See missing nodes or install a video workflow, then try again.";
    }
    session.nextSteps = {
        "Unload the LM Studio model (or leave this chat) so VRAM is free.",
        "Open ComfyUI at " + settings_.baseUrl + " and load a video workflow"
            + (fileNames.empty() ? "." : " such as " + fileNames.front() + "."),
        "Queue the workflow in ComfyUI. Forge will not submit GPU jobs while execution_policy is prepare_only.",
        "When the dedicated ComfyUI machine is online, Forge will submit this prepared plan over the network instead.",
    };
    if (!arguments.value("prompt", arguments.value("concept", "")).empty()) {
        session.nextSteps.insert(
            session.nextSteps.begin() + 1,
            "Use this intent when you queue: " + arguments.value("prompt", arguments.value("concept", "")));
    }
    store_.savePrepareSession(session);

    nlohmann::json payload = {
        {"ready", session.ready},
        {"execution_policy", session.executionPolicy},
        {"comfy_url", session.comfyUrl},
        {"workflow_ids", session.workflowIds},
        {"workflow_files", session.workflowFiles},
        {"video_nodes", hints},
        {"missing_nodes", session.missingNodes},
        {"object_info_available", caps.objectInfoAvailable},
        {"user_message", session.userMessage},
        {"next_steps", session.nextSteps},
        {"submitted", false},
    };
    if (!apiPrompt.is_null()) {
        payload["prompt"] = apiPrompt;
    }
    return okJson(payload);
}

} // namespace Forge::Comfy
