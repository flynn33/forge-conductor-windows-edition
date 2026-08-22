// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Clock.h"
#include "ForgeDomain/Ports.h"
#include "ForgeDomain/Version.h"
#include "ForgeOrchestration/AgentCatalog.h"
#include "ForgePersistence/AppPaths.h"
#include "ForgePersistence/AuditService.h"
#include "ForgePersistence/ConfigStore.h"
#include "ForgePersistence/SQLiteStore.h"

#include <memory>
#include <string>

namespace Forge::Orchestration {

class ToolRouter;
class AgentSessionService;
class ContextContinuityService;
class ContinuityAutomation;
class ToolAuthorizationService;

class ForgeServices final {
public:
    static constexpr const char* version() { return Domain::kVersion; }

    static std::shared_ptr<ForgeServices> bootstrap(
        std::optional<std::filesystem::path> home,
        std::filesystem::path bundledAgents,
        std::shared_ptr<Domain::IClock> clock = std::make_shared<Domain::SystemClock>());

    ~ForgeServices();

    Persistence::AppPaths& paths() { return paths_; }
    Persistence::ConfigStore& config() { return *config_; }
    Persistence::SQLiteStore& store() { return *store_; }
    Persistence::AuditService& audit() { return *audit_; }
    Persistence::DiagnosticLog& diagnostics() { return *diagnostics_; }
    AgentCatalog& catalog() { return *catalog_; }
    ToolRouter& tools();
    Domain::DoctorReport doctor() const;
    void shutdown();

    AgentSessionService& sessions() { return *sessions_; }
    ContextContinuityService& continuity() { return *continuity_; }
    ContinuityAutomation& continuityAutomation() { return *automation_; }

private:
    ForgeServices() = default;

    Persistence::AppPaths paths_{std::nullopt};
    std::shared_ptr<Domain::IClock> clock_;
    std::unique_ptr<Persistence::ConfigStore> config_;
    std::unique_ptr<Persistence::SQLiteStore> store_;
    std::unique_ptr<Persistence::AuditService> audit_;
    std::unique_ptr<Persistence::DiagnosticLog> diagnostics_;
    std::unique_ptr<AgentCatalog> catalog_;
    std::unique_ptr<AgentSessionService> sessions_;
    std::unique_ptr<ContextContinuityService> continuity_;
    std::unique_ptr<ContinuityAutomation> automation_;
    std::unique_ptr<ToolAuthorizationService> authorization_;
    std::unique_ptr<ToolRouter> tools_;
};

} // namespace Forge::Orchestration
