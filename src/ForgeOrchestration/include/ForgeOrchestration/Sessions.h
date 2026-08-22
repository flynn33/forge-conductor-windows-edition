// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Clock.h"
#include "ForgeOrchestration/AgentCatalog.h"
#include "ForgePersistence/AuditService.h"
#include "ForgePersistence/ConfigStore.h"
#include "ForgePersistence/SQLiteStore.h"

#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace Forge::Orchestration {

class AgentSessionService final {
public:
    AgentSessionService(
        Persistence::SQLiteStore& store,
        AgentCatalog& catalog,
        Persistence::AuditService& audit,
        Persistence::DiagnosticLog& diagnostics,
        std::shared_ptr<Domain::IClock> clock,
        int idleTtlSec);

    Domain::AgentSession start(
        const std::string& agentID,
        const std::string& goal,
        const std::string& cwd,
        const Domain::ClientID& clientID);
    std::optional<Domain::AgentSession> status(const std::string& sessionID);
    Domain::AgentSession complete(const std::string& sessionID, const std::string& report);
    std::optional<Domain::ActiveBinding> rehydrate(const Domain::ClientID& clientID);
    std::optional<Domain::ActiveBinding> binding(const Domain::ClientID& clientID);
    void touchIfActive(const Domain::ClientID& clientID);
    void adoptWorkspace(const Domain::ClientID& clientID, const std::string& cwd);

private:
    Persistence::SQLiteStore& store_;
    AgentCatalog& catalog_;
    Persistence::AuditService& audit_;
    Persistence::DiagnosticLog& diagnostics_;
    std::shared_ptr<Domain::IClock> clock_;
    int idleTtlSec_;
    std::mutex mutex_;
    std::unordered_map<std::string, Domain::ActiveBinding> bindings_;
};

class ContextContinuityService final {
public:
    ContextContinuityService(
        Persistence::AppPaths paths,
        Persistence::SQLiteStore& store,
        AgentSessionService& sessions,
        Persistence::DiagnosticLog& diagnostics,
        std::shared_ptr<Domain::IClock> clock);

    Domain::HandoffPacket checkpoint(
        const Domain::ClientID& clientID,
        const std::string& goal,
        const std::string& narrative,
        const std::vector<std::string>& nextActions,
        bool finalize);
    std::optional<Domain::HandoffPacket> latest(bool resumeReadyOnly = false);
    std::optional<Domain::HandoffPacket> get(const std::string& id);
    std::vector<Domain::HandoffPacket> list(int limit);
    void projectFiles(const Domain::HandoffPacket& packet);

private:
    Persistence::AppPaths paths_;
    Persistence::SQLiteStore& store_;
    AgentSessionService& sessions_;
    Persistence::DiagnosticLog& diagnostics_;
    std::shared_ptr<Domain::IClock> clock_;
};

class ContinuityAutomation final {
public:
    static constexpr int checkpointEveryTools = 5;
    static constexpr int handoffEveryTools = 20;

    ContinuityAutomation(
        Persistence::SQLiteStore& store,
        AgentSessionService& sessions,
        ContextContinuityService& continuity,
        Persistence::DiagnosticLog& diagnostics,
        std::shared_ptr<Domain::IClock> clock);

    std::vector<std::filesystem::path> additionalRoots(const Domain::ClientID& clientID);
    bool isBlocked(const Domain::ClientID& clientID);
    void block(const Domain::ClientID& clientID);
    void clearBlock(const Domain::ClientID& clientID);
    void noteProgress(const Domain::ClientID& clientID, const std::string& tool, const std::string& path);
    bool shouldCheckpoint(const Domain::ClientID& clientID);
    bool shouldHandoff(const Domain::ClientID& clientID);

private:
    struct ClientState {
        int progressCount{0};
        bool blocked{false};
        std::vector<std::filesystem::path> implicitRoots;
    };

    Persistence::SQLiteStore& store_;
    AgentSessionService& sessions_;
    ContextContinuityService& continuity_;
    Persistence::DiagnosticLog& diagnostics_;
    std::shared_ptr<Domain::IClock> clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, ClientState> state_;
};

class ToolAuthorizationService final {
public:
    ToolAuthorizationService(
        Persistence::AppPaths paths,
        Persistence::ConfigStore& config,
        ContinuityAutomation& workspace);

    enum class DecisionKind { Allowed, Denied };
    struct Decision {
        DecisionKind kind{DecisionKind::Allowed};
        std::map<std::string, std::string> arguments;
        std::string code;
        std::string message;
    };

    Decision authorize(
        const std::string& tool,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID& clientID,
        const std::optional<Domain::ActiveBinding>& binding);

private:
    Persistence::AppPaths paths_;
    Persistence::ConfigStore& config_;
    ContinuityAutomation& workspace_;
};

} // namespace Forge::Orchestration
