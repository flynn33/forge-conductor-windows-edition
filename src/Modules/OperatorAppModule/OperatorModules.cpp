// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "OperatorModules.h"

#include "ForgeHost/LaunchOptions.h"
#include "ForgePersistence/AppPaths.h"
#include "ForgeRuntime/Logger.h"
#include "ForgeRuntime/ServiceContainer.h"
#include "ForgeComfy/ComfyControl.h"
#include "ForgeLmStudio/LmStudioDeploy.h"
#include "ForgeManager/ManagerController.h"
#include "ForgeOrchestration/ForgeServices.h"
#include "ForgeOrchestration/ToolRouter.h"
#include "ForgeTelemetry/TelemetryService.h"

#include <memory>
#include <stdexcept>

namespace Forge::Modules {
namespace {

Runtime::ModuleManifest makeManifest(
    const char* id,
    const char* name,
    Runtime::ModuleType type,
    const char* entry,
    std::vector<Runtime::Capability> capabilities) {
    Runtime::ModuleManifest manifest;
    manifest.schemaVersion = "1.1";
    manifest.manifestTemplateVersion = Runtime::ManifestTemplateVersion::V1_1;
    manifest.moduleID = id;
    manifest.displayName = name;
    manifest.moduleVersion = {0, 8, 0, std::nullopt};
    manifest.moduleType = type;
    manifest.entryPoint = entry;
    manifest.capabilitiesRequested = std::move(capabilities);
    return manifest;
}

Host::LaunchOptions& requireLaunch(Runtime::IForgeModuleContext& context) {
    auto options = context.services().get<Host::LaunchOptions>();
    if (!options) {
        throw std::runtime_error("LaunchOptions were not registered before module activation");
    }
    return *options;
}

} // namespace

Runtime::ModuleDescriptor PersistenceModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest PersistenceModule::manifest() const {
    return makeManifest("com.forge.module.persistence", "Persistence", Runtime::ModuleType::Service,
        "PersistenceModule", {Runtime::Capability::Storage, Runtime::Capability::Diagnostics});
}
void PersistenceModule::start(Runtime::IForgeModuleContext& context) {
    auto& options = requireLaunch(context);
    Persistence::AppPaths paths(options.home);
    paths.ensureLayout();
    context.services().add(std::make_shared<Persistence::AppPaths>(paths));
    context.logger().log(Runtime::LogLevel::Info, "persistence_ready", {
        {"home", paths.home().string()},
    });
}
void PersistenceModule::stop(Runtime::IForgeModuleContext&) {}

Runtime::ModuleDescriptor OrchestrationModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest OrchestrationModule::manifest() const {
    return makeManifest("com.forge.module.orchestration", "Orchestration", Runtime::ModuleType::Service,
        "OrchestrationModule", {Runtime::Capability::Storage, Runtime::Capability::Diagnostics});
}
void OrchestrationModule::start(Runtime::IForgeModuleContext& context) {
    auto& options = requireLaunch(context);
    auto app = Orchestration::ForgeServices::bootstrap(options.home, options.bundledAgents);
    context.services().add(app);
    context.logger().log(Runtime::LogLevel::Info, "orchestration_ready", {
        {"agents", std::to_string(app->catalog().all().size())},
        {"tools", std::to_string(app->tools().toolNames().size())},
    });
}
void OrchestrationModule::stop(Runtime::IForgeModuleContext& context) {
    if (auto app = context.services().get<Orchestration::ForgeServices>()) {
        app->shutdown();
    }
}

Runtime::ModuleDescriptor McpModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest McpModule::manifest() const {
    return makeManifest("com.forge.module.mcp", "MCP", Runtime::ModuleType::Service,
        "McpModule", {Runtime::Capability::Diagnostics});
}
void McpModule::start(Runtime::IForgeModuleContext& context) {
    if (!context.services().get<Orchestration::ForgeServices>()) {
        throw std::runtime_error("MCP module requires orchestration");
    }
}
void McpModule::stop(Runtime::IForgeModuleContext&) {}

Runtime::ModuleDescriptor TelemetryModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest TelemetryModule::manifest() const {
    return makeManifest("com.forge.module.telemetry", "Telemetry", Runtime::ModuleType::Service,
        "TelemetryModule", {Runtime::Capability::Telemetry});
}
void TelemetryModule::start(Runtime::IForgeModuleContext& context) {
    auto app = context.services().get<Orchestration::ForgeServices>();
    if (!app) {
        throw std::runtime_error("Telemetry module requires orchestration");
    }
    auto telemetry = std::make_shared<Telemetry::TelemetryService>(
        app->store(),
        [app] { return app->tools().toolNames(); });
    auto& options = requireLaunch(context);
    telemetry->start(options.headless ? 0.5 : (1.0 / 30.0));
    context.services().add(telemetry);
}
void TelemetryModule::stop(Runtime::IForgeModuleContext& context) {
    if (auto telemetry = context.services().get<Telemetry::TelemetryService>()) {
        telemetry->stop();
    }
}

Runtime::ModuleDescriptor LmStudioModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest LmStudioModule::manifest() const {
    return makeManifest("com.forge.module.lmstudio", "LM Studio", Runtime::ModuleType::Service,
        "LmStudioModule", {Runtime::Capability::FileExport, Runtime::Capability::Diagnostics});
}
void LmStudioModule::start(Runtime::IForgeModuleContext& context) {
    auto app = context.services().get<Orchestration::ForgeServices>();
    auto& options = requireLaunch(context);
    if (!app) {
        throw std::runtime_error("LM Studio module requires orchestration");
    }
    context.services().add(std::make_shared<LmStudio::LmStudioDeployService>(
        app->paths(), options.executable));
}
void LmStudioModule::stop(Runtime::IForgeModuleContext&) {}

Runtime::ModuleDescriptor ComfyModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest ComfyModule::manifest() const {
    return makeManifest("com.forge.module.comfy", "ComfyUI", Runtime::ModuleType::Service,
        "ComfyModule", {Runtime::Capability::Diagnostics});
}
void ComfyModule::start(Runtime::IForgeModuleContext& context) {
    auto app = context.services().get<Orchestration::ForgeServices>();
    if (!app) {
        throw std::runtime_error("Comfy module requires orchestration");
    }
    context.services().add(Comfy::ComfyControl::create(app->paths()));
}
void ComfyModule::stop(Runtime::IForgeModuleContext&) {}

Runtime::ModuleDescriptor ManagerModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest ManagerModule::manifest() const {
    return makeManifest("com.forge.module.manager", "Manager", Runtime::ModuleType::Service,
        "ManagerModule", {Runtime::Capability::Diagnostics});
}
void ManagerModule::start(Runtime::IForgeModuleContext& context) {
    auto app = context.services().get<Orchestration::ForgeServices>();
    auto& options = requireLaunch(context);
    if (!app) {
        throw std::runtime_error("Manager module requires orchestration");
    }
    context.services().add(std::make_shared<Manager::ManagerController>(
        app->paths(), options.executable));
}
void ManagerModule::stop(Runtime::IForgeModuleContext&) {}

Runtime::ModuleDescriptor OperatorAppModule::descriptor() const { return manifest().descriptor(); }
Runtime::ModuleManifest OperatorAppModule::manifest() const {
    auto manifest = makeManifest("com.forge.module.app.operator", "Operator", Runtime::ModuleType::App,
        "OperatorAppModule",
        {Runtime::Capability::ToolbarItems, Runtime::Capability::ViewInjection, Runtime::Capability::EventPublishing});
    Runtime::ModuleUIRequirements ui;
    ui.layoutID = "operator.layout";
    ui.viewIDs = {"Rig", "Mcp", "Agents", "Tools", "Feed", "Diagnostics", "Manager"};
    ui.slotIDs = {"appShell"};
    manifest.runtimeRequirements.ui = ui;
    return manifest;
}
Runtime::UIContributions OperatorAppModule::uiContributions() const {
    Runtime::UIContributions contributions;
    contributions.viewInjections.push_back({"OperatorShell", "appShell"});
    contributions.toolbarItems.push_back({"deploy", "Deploy to LM Studio", Runtime::ToolbarAction::PublishEvent, "lmstudio.deploy"});
    return contributions;
}
void OperatorAppModule::start(Runtime::IForgeModuleContext& context) {
    if (!context.services().get<Orchestration::ForgeServices>() ||
        !context.services().get<Telemetry::TelemetryService>()) {
        throw std::runtime_error("Operator app requires orchestration and telemetry");
    }
}
void OperatorAppModule::stop(Runtime::IForgeModuleContext&) {}

void registerAllModules(Runtime::ModuleRegistry& registry) {
    registry.add("PersistenceModule", [] { return std::make_shared<PersistenceModule>(); });
    registry.add("OrchestrationModule", [] { return std::make_shared<OrchestrationModule>(); });
    registry.add("McpModule", [] { return std::make_shared<McpModule>(); });
    registry.add("TelemetryModule", [] { return std::make_shared<TelemetryModule>(); });
    registry.add("LmStudioModule", [] { return std::make_shared<LmStudioModule>(); });
    registry.add("ComfyModule", [] { return std::make_shared<ComfyModule>(); });
    registry.add("ManagerModule", [] { return std::make_shared<ManagerModule>(); });
    registry.add("OperatorAppModule", [] { return std::make_shared<OperatorAppModule>(); });
}

} // namespace Forge::Modules
