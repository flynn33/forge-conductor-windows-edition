// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/ForgeServices.h"

#include "ForgeOrchestration/Sessions.h"
#include "ForgeOrchestration/ToolRouter.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Forge::Orchestration {

std::shared_ptr<ForgeServices> ForgeServices::bootstrap(
    std::optional<std::filesystem::path> home,
    std::filesystem::path bundledAgents,
    std::shared_ptr<Domain::IClock> clock) {
    auto app = std::shared_ptr<ForgeServices>(new ForgeServices());
    app->paths_ = Persistence::AppPaths(std::move(home));
    app->paths_.ensureLayout();
    app->clock_ = std::move(clock);
    app->config_ = std::make_unique<Persistence::ConfigStore>(app->paths_);
    app->store_ = std::make_unique<Persistence::SQLiteStore>(app->paths_.storeSQLite(), app->clock_);
    app->audit_ = std::make_unique<Persistence::AuditService>(*app->store_, app->paths_);
    const auto role = app->config_->stringAt("mcp.role", "primary");
    app->diagnostics_ = std::make_unique<Persistence::DiagnosticLog>(app->paths_, role);
    app->catalog_ = std::make_unique<AgentCatalog>(app->paths_, std::move(bundledAgents));
    app->sessions_ = std::make_unique<AgentSessionService>(
        *app->store_,
        *app->catalog_,
        *app->audit_,
        *app->diagnostics_,
        app->clock_,
        app->config_->intAt("sessions.idle_ttl_sec", 14400));
    app->continuity_ = std::make_unique<ContextContinuityService>(
        app->paths_, *app->store_, *app->sessions_, *app->diagnostics_, app->clock_);
    app->automation_ = std::make_unique<ContinuityAutomation>(
        *app->store_, *app->sessions_, *app->continuity_, *app->diagnostics_, app->clock_);
    app->authorization_ = std::make_unique<ToolAuthorizationService>(
        app->paths_, *app->config_, *app->automation_);
    app->tools_ = std::make_unique<ToolRouter>(*app);
    app->diagnostics_->info("app_bootstrap", {
        {"version", Domain::kVersion},
        {"home", app->paths_.home().string()},
        {"agents", std::to_string(app->catalog_->all().size())},
        {"tools", std::to_string(app->tools_->toolNames().size())},
    });
    return app;
}

ForgeServices::~ForgeServices() { shutdown(); }

ToolRouter& ForgeServices::tools() { return *tools_; }

void ForgeServices::shutdown() {
    if (store_) {
        store_->close();
    }
}

Domain::DoctorReport ForgeServices::doctor() const {
    Domain::DoctorReport report;
    auto check = [&](const std::string& name, bool ok, const std::string& detail, bool hard = true) {
        if (hard && !ok) {
            report.ok = false;
        }
        report.checks.push_back(Domain::DoctorCheck{name, ok, detail, hard});
    };
    check("home_layout", std::filesystem::exists(paths_.home()), paths_.home().string());
    check("sqlite_store", std::filesystem::exists(paths_.storeSQLite()), paths_.storeSQLite().string());
    check("agent_catalog", catalog_->all().size() >= 5, std::to_string(catalog_->all().size()) + " agents loaded");
    check("config", std::filesystem::exists(paths_.configJSON()), paths_.configJSON().string());
#ifdef _WIN32
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    check("executable", modulePath[0] != 0, std::filesystem::path(modulePath).string());
#endif
    return report;
}

} // namespace Forge::Orchestration
