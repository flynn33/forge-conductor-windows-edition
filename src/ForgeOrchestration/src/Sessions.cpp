// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/Sessions.h"

#include "ForgeDomain/Clock.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace Forge::Orchestration {

AgentSessionService::AgentSessionService(
    Persistence::SQLiteStore& store,
    AgentCatalog& catalog,
    Persistence::AuditService& audit,
    Persistence::DiagnosticLog& diagnostics,
    std::shared_ptr<Domain::IClock> clock,
    int idleTtlSec)
    : store_(store)
    , catalog_(catalog)
    , audit_(audit)
    , diagnostics_(diagnostics)
    , clock_(std::move(clock))
    , idleTtlSec_(idleTtlSec) {}

Domain::AgentSession AgentSessionService::start(
    const std::string& agentID,
    const std::string& goal,
    const std::string& cwd,
    const Domain::ClientID& clientID) {
    const auto spec = catalog_.get(agentID);
    if (!spec) {
        throw std::runtime_error("Unknown agent: " + agentID);
    }
    Domain::AgentSession session;
    session.id = Domain::makeUuid();
    session.agentID = agentID;
    session.clientID = clientID.rawValue;
    session.status = Domain::SessionStatus::Open;
    session.summary = goal;
    session.cwd = cwd;
    session.createdAt = Domain::iso8601(clock_->now());
    session.updatedAt = session.createdAt;
    store_.sessionUpsert(session);

    Domain::ActiveBinding binding;
    binding.sessionID = session.id;
    binding.agentID = agentID;
    binding.cwd = cwd;
    binding.toolsPrimary = spec->tools;
    binding.toolsForbidden = spec->toolsForbidden;
    std::lock_guard lock(mutex_);
    bindings_[clientID.rawValue] = binding;
    diagnostics_.info("agent_run_start", {{"session", session.id}, {"agent", agentID}});
    return session;
}

std::optional<Domain::AgentSession> AgentSessionService::status(const std::string& sessionID) {
    for (const auto& session : store_.sessionList()) {
        if (session.id == sessionID) {
            return session;
        }
    }
    return std::nullopt;
}

Domain::AgentSession AgentSessionService::complete(const std::string& sessionID, const std::string& report) {
    auto session = status(sessionID);
    if (!session) {
        throw std::runtime_error("Unknown session");
    }
    session->status = Domain::SessionStatus::Completed;
    session->summary = report;
    session->updatedAt = Domain::iso8601(clock_->now());
    store_.sessionUpsert(*session);
    return *session;
}

std::optional<Domain::ActiveBinding> AgentSessionService::rehydrate(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(clientID.rawValue);
    if (it == bindings_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Domain::ActiveBinding> AgentSessionService::binding(const Domain::ClientID& clientID) {
    return rehydrate(clientID);
}

void AgentSessionService::touchIfActive(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(clientID.rawValue);
    if (it == bindings_.end()) {
        return;
    }
    if (auto session = status(it->second.sessionID)) {
        session->updatedAt = Domain::iso8601(clock_->now());
        store_.sessionUpsert(*session);
    }
}

void AgentSessionService::adoptWorkspace(const Domain::ClientID& clientID, const std::string& cwd) {
    std::lock_guard lock(mutex_);
    auto& binding = bindings_[clientID.rawValue];
    binding.cwd = cwd;
}

ContextContinuityService::ContextContinuityService(
    Persistence::AppPaths paths,
    Persistence::SQLiteStore& store,
    AgentSessionService& sessions,
    Persistence::DiagnosticLog& diagnostics,
    std::shared_ptr<Domain::IClock> clock)
    : paths_(std::move(paths))
    , store_(store)
    , sessions_(sessions)
    , diagnostics_(diagnostics)
    , clock_(std::move(clock)) {}

Domain::HandoffPacket ContextContinuityService::checkpoint(
    const Domain::ClientID& clientID,
    const std::string& goal,
    const std::string& narrative,
    const std::vector<std::string>& nextActions,
    bool finalize) {
    Domain::HandoffPacket packet;
    packet.id = Domain::makeUuid();
    packet.createdAt = Domain::iso8601(clock_->now());
    packet.updatedAt = packet.createdAt;
    packet.source = finalize ? "handoff" : "checkpoint";
    packet.resumeReady = finalize;
    packet.clientID = clientID.rawValue;
    packet.writeSequence = static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(clock_->now()));
    packet.goal = goal;
    packet.narrative = narrative;
    packet.nextActions = nextActions;
    packet.resumeSeed = finalize ? ("Resume from packet " + packet.id) : "";
    if (const auto binding = sessions_.binding(clientID); binding && binding->cwd) {
        packet.cwd = binding->cwd;
    }
    nlohmann::json json;
    json["id"] = packet.id;
    json["goal"] = packet.goal;
    json["narrative"] = packet.narrative;
    json["next_actions"] = packet.nextActions;
    json["resume_seed"] = packet.resumeSeed;
    if (packet.cwd) json["cwd"] = *packet.cwd;
    packet.packetJson = json.dump();
    store_.handoffUpsert(packet);
    projectFiles(packet);
    return packet;
}

std::optional<Domain::HandoffPacket> ContextContinuityService::latest(bool resumeReadyOnly) {
    return store_.handoffLatest(resumeReadyOnly);
}

std::optional<Domain::HandoffPacket> ContextContinuityService::get(const std::string& id) {
    return store_.handoffGet(id);
}

std::vector<Domain::HandoffPacket> ContextContinuityService::list(int limit) {
    return store_.handoffList(limit);
}

void ContextContinuityService::projectFiles(const Domain::HandoffPacket& packet) {
    std::filesystem::create_directories(paths_.memoryHandoffsDir());
    std::ofstream current(paths_.memoryCurrentTask());
    current << "# Current task\n\n" << packet.goal << "\n\n" << packet.narrative << "\n";
    if (packet.resumeReady) {
        std::ofstream next(paths_.memoryNextChat());
        next << "# Next chat\n\nCall context_get then continue from:\n";
        for (const auto& action : packet.nextActions) {
            next << "- " << action << "\n";
        }
    }
    std::ofstream copy(paths_.memoryHandoffsDir() / (packet.id + ".json"));
    copy << packet.packetJson;
}

ContinuityAutomation::ContinuityAutomation(
    Persistence::SQLiteStore& store,
    AgentSessionService& sessions,
    ContextContinuityService& continuity,
    Persistence::DiagnosticLog& diagnostics,
    std::shared_ptr<Domain::IClock> clock)
    : store_(store)
    , sessions_(sessions)
    , continuity_(continuity)
    , diagnostics_(diagnostics)
    , clock_(std::move(clock)) {}

std::vector<std::filesystem::path> ContinuityAutomation::additionalRoots(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    auto roots = state_[clientID.rawValue].implicitRoots;
    if (const auto binding = sessions_.binding(clientID); binding && binding->cwd) {
        roots.push_back(*binding->cwd);
    }
    if (const auto packet = store_.handoffLatest(false); packet && packet->cwd) {
        roots.push_back(*packet->cwd);
    }
    return roots;
}

bool ContinuityAutomation::isBlocked(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    return state_[clientID.rawValue].blocked;
}

void ContinuityAutomation::block(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    state_[clientID.rawValue].blocked = true;
}

void ContinuityAutomation::clearBlock(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    state_[clientID.rawValue].blocked = false;
}

void ContinuityAutomation::noteProgress(
    const Domain::ClientID& clientID,
    const std::string& tool,
    const std::string& path) {
    std::lock_guard lock(mutex_);
    auto& client = state_[clientID.rawValue];
    ++client.progressCount;
    if (!path.empty()) {
        client.implicitRoots.push_back(path);
    }
    (void)tool;
}

bool ContinuityAutomation::shouldCheckpoint(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    return state_[clientID.rawValue].progressCount > 0 &&
           state_[clientID.rawValue].progressCount % checkpointEveryTools == 0;
}

bool ContinuityAutomation::shouldHandoff(const Domain::ClientID& clientID) {
    std::lock_guard lock(mutex_);
    return state_[clientID.rawValue].progressCount > 0 &&
           state_[clientID.rawValue].progressCount % handoffEveryTools == 0;
}

ToolAuthorizationService::ToolAuthorizationService(
    Persistence::AppPaths paths,
    Persistence::ConfigStore& config,
    ContinuityAutomation& workspace)
    : paths_(std::move(paths))
    , config_(config)
    , workspace_(workspace) {}

ToolAuthorizationService::Decision ToolAuthorizationService::authorize(
    const std::string& tool,
    const std::map<std::string, std::string>& arguments,
    const Domain::ClientID& clientID,
    const std::optional<Domain::ActiveBinding>& binding) {
    static const std::vector<std::string> sessionTools{
        "forge_status", "agent_list", "agent_get", "agent_context", "agent_recommend",
        "agent_run_start", "agent_run_status", "agent_run_complete",
        "session_checkpoint", "session_handoff", "context_get", "context_list",
        "memory_set", "memory_get", "memory_list", "memory_delete", "memory_search"};
    static const std::vector<std::string> requiresSession{"shell_exec", "git_add", "git_commit"};

    if (binding) {
        for (const auto& forbidden : binding->toolsForbidden) {
            if (forbidden == tool) {
                return Decision{DecisionKind::Denied, arguments, "tool_forbidden",
                    "Agent '" + binding->agentID + "' forbids tool '" + tool + "'"};
            }
        }
        if (!binding->toolsPrimary.empty()) {
            bool granted = false;
            for (const auto& allowed : binding->toolsPrimary) {
                if (allowed == tool) granted = true;
            }
            for (const auto& allowed : sessionTools) {
                if (allowed == tool) granted = true;
            }
            if (!granted) {
                return Decision{DecisionKind::Denied, arguments, "tool_not_granted",
                    "Tool '" + tool + "' is not granted"};
            }
        }
    } else {
        for (const auto& required : requiresSession) {
            if (required == tool && workspace_.additionalRoots(clientID).empty()) {
                return Decision{DecisionKind::Denied, arguments, "active_session_required",
                    "Tool requires agent_run_start with an explicit workspace cwd"};
            }
        }
    }

    auto pathIt = arguments.find("path");
    if (pathIt != arguments.end()) {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(pathIt->second), ec);
        if (ec) {
            canonical = std::filesystem::absolute(pathIt->second);
        }
        std::vector<std::filesystem::path> roots{paths_.home()};
        for (const auto& extra : config_.model().allowedRoots) {
            roots.emplace_back(extra);
        }
        for (const auto& extra : workspace_.additionalRoots(clientID)) {
            roots.push_back(extra);
        }
        if (binding && binding->cwd) {
            roots.emplace_back(*binding->cwd);
        }
        bool inside = false;
        for (const auto& root : roots) {
            std::error_code rec;
            const auto rootCanon = std::filesystem::weakly_canonical(root, rec);
            const auto rel = canonical.lexically_relative(rootCanon);
            if (!rel.empty() && rel.native().find(L"..") == std::wstring::npos &&
                rel.string().find("..") == std::string::npos) {
                inside = true;
                break;
            }
        }
        const bool writeTool = tool == "fs_write" || tool == "fs_edit" || tool == "fs_delete" ||
            tool == "fs_mkdir" || tool == "fs_move";
        if (!inside && writeTool) {
            return Decision{DecisionKind::Denied, arguments, "path_outside_allowed_roots",
                "Path is outside allowed roots"};
        }
        auto normalized = arguments;
        normalized["path"] = canonical.string();
        return Decision{DecisionKind::Allowed, normalized, {}, {}};
    }
    return Decision{DecisionKind::Allowed, arguments, {}, {}};
}

} // namespace Forge::Orchestration
