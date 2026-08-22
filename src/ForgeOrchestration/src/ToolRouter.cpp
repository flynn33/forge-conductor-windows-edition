// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/ToolRouter.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <set>

namespace Forge::Orchestration {

std::string arg(const std::map<std::string, std::string>& arguments, const std::string& key) {
    const auto it = arguments.find(key);
    return it == arguments.end() ? std::string{} : it->second;
}

std::filesystem::path resolvePath(const std::string& path) {
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec) {
        resolved = std::filesystem::absolute(path);
    }
    return resolved;
}

ToolRouter::ToolRouter(ForgeServices& services) : services_(services) {
    packs_.push_back(makeAgentToolPack(services_));
    packs_.push_back(makeMemoryToolPack(services_));
    packs_.push_back(makeContinuityToolPack(services_));
    packs_.push_back(makeFilesystemToolPack());
    packs_.push_back(makeGitToolPack());
    packs_.push_back(makeShellToolPack());
    packs_.push_back(makeDocsToolPack());
    packs_.push_back(makeSearchToolPack());
}

std::vector<std::string> ToolRouter::toolNames() const {
    std::set<std::string> names;
    for (const auto& pack : packs_) {
        for (const auto& name : pack->toolNames()) {
            names.insert(name);
        }
    }
    return {names.begin(), names.end()};
}

Domain::ToolResult ToolRouter::call(
    const std::string& name,
    const std::map<std::string, std::string>& arguments,
    const Domain::ClientID& clientID) {
    const auto started = std::chrono::steady_clock::now();
    services_.sessions().touchIfActive(clientID);

    static const std::set<std::string> continuity{
        "session_checkpoint", "session_handoff", "context_get", "context_list"};
    static const std::set<std::string> progress{
        "fs_read", "fs_write", "fs_edit", "fs_list", "fs_glob", "fs_mkdir", "fs_delete", "fs_move",
        "shell_exec", "git_status", "git_diff", "git_log", "git_add", "git_commit",
        "memory_set", "search_text", "pdf_write", "pdf_from_file",
        "agent_run_start", "agent_run_complete"};

    if (!continuity.contains(name) && services_.continuityAutomation().isBlocked(clientID)) {
        auto blocked = Domain::ToolResult::failure(
            "context_budget_exceeded",
            "This chat was handed off. Start a new chat and call context_get.");
        return blocked;
    }

    std::string fingerprint = name;
    for (const auto& [k, v] : arguments) {
        fingerprint += "|" + k + "=" + v;
    }
    int loopCount = 0;
    {
        std::lock_guard lock(mutex_);
        auto& last = lastCall_[clientID.rawValue];
        if (last.first == fingerprint) {
            ++last.second;
        } else {
            last = {fingerprint, 1};
        }
        loopCount = last.second;
    }
    if (!continuity.contains(name) && loopCount >= 9) {
        services_.continuityAutomation().block(clientID);
        return Domain::ToolResult::failure("identical_call_loop", "Same tool called 9 times");
    }

    const auto binding = services_.sessions().rehydrate(clientID);
    ToolAuthorizationService authorizer(services_.paths(), services_.config(), services_.continuityAutomation());
    const auto decision = authorizer.authorize(name, arguments, clientID, binding);
    if (decision.kind == ToolAuthorizationService::DecisionKind::Denied) {
        return Domain::ToolResult::failure(decision.code, decision.message);
    }

    Domain::ToolResult result = Domain::ToolResult::failure("unknown_tool", "No pack handled " + name);
    for (auto& pack : packs_) {
        if (auto handled = pack->handle(name, decision.arguments, clientID)) {
            result = *handled;
            break;
        }
    }

    if (progress.contains(name)) {
        services_.continuityAutomation().noteProgress(clientID, name, arg(decision.arguments, "path"));
        if (services_.continuityAutomation().shouldHandoff(clientID)) {
            services_.continuity().checkpoint(clientID, "", "", {}, true);
            services_.continuityAutomation().block(clientID);
            result.payload["handoff_required"] = "true";
        } else if (services_.continuityAutomation().shouldCheckpoint(clientID)) {
            services_.continuity().checkpoint(clientID, "", "", {}, false);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    nlohmann::json args = nlohmann::json::object();
    for (const auto& [k, v] : decision.arguments) {
        args[k] = v;
    }
    services_.audit().record(
        name,
        args.dump(),
        result.ok ? "ok" : "error",
        elapsed.count(),
        result.ok ? std::nullopt : std::optional<std::string>(result.code),
        clientID.rawValue);
    return result;
}

} // namespace Forge::Orchestration
