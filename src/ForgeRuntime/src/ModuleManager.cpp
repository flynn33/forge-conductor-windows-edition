// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/ModuleManager.h"

#include "ForgeRuntime/Logger.h"
#include "ForgeRuntime/ManifestLoader.h"

#include <stdexcept>

namespace Forge::Runtime {

ModuleManager::ModuleManager(
    ModuleRegistry registry,
    std::shared_ptr<IEntitlementProvider> entitlements,
    std::shared_ptr<ICapabilityPolicy> policy,
    std::shared_ptr<IActivationStore> activationStore,
    std::shared_ptr<IForgeEventBus> events,
    std::shared_ptr<IForgeLogger> logger,
    std::shared_ptr<IServiceProvider> services,
    std::shared_ptr<UISurfaceManager> surface,
    std::shared_ptr<IModuleCommunicationGuard> guard)
    : registry_(std::move(registry))
    , entitlements_(std::move(entitlements))
    , policy_(std::move(policy))
    , activationStore_(std::move(activationStore))
    , events_(std::move(events))
    , logger_(std::move(logger))
    , services_(std::move(services))
    , surface_(std::move(surface))
    , guard_(std::move(guard)) {}

void ModuleManager::discover(const std::string& manifestDirectory) {
    ManifestLoader loader;
    manifests_ = loader.loadDirectory(manifestDirectory);
    logger_->log(LogLevel::Info, "modules_discovered", {
        {"count", std::to_string(manifests_.size())},
        {"path", manifestDirectory},
    });
}

void ModuleManager::activate(const std::string& moduleID) {
    for (const auto& manifest : manifests_) {
        if (manifest.moduleID == moduleID) {
            activateLocked(manifest);
            persist();
            return;
        }
    }
    throw std::runtime_error("Unknown module: " + moduleID);
}

void ModuleManager::deactivate(const std::string& moduleID) {
    const auto it = active_.find(moduleID);
    if (it == active_.end()) {
        return;
    }
    it->second.instance->stop(*it->second.context);
    if (activeSurfaceModuleID_ == moduleID) {
        surface_->clear();
        activeSurfaceModuleID_.clear();
    }
    active_.erase(it);
    persist();
}

void ModuleManager::restore() {
    if (!activationStore_) {
        return;
    }
    for (const auto& moduleID : activationStore_->loadActiveModuleIDs()) {
        try {
            activate(moduleID);
        } catch (const std::exception& ex) {
            logger_->log(LogLevel::Warn, "restore_failed", {
                {"module", moduleID},
                {"error", ex.what()},
            });
        }
    }
}

void ModuleManager::shutdown() {
    const auto ids = [&] {
        std::vector<std::string> result;
        result.reserve(active_.size());
        for (const auto& [id, _] : active_) {
            result.push_back(id);
        }
        return result;
    }();
    for (const auto& id : ids) {
        deactivate(id);
    }
}

bool ModuleManager::isActive(const std::string& moduleID) const {
    return active_.contains(moduleID);
}

void ModuleManager::persist() const {
    if (!activationStore_) {
        return;
    }
    std::vector<std::string> ids;
    ids.reserve(active_.size());
    for (const auto& [id, _] : active_) {
        ids.push_back(id);
    }
    activationStore_->saveActiveModuleIDs(ids);
}

void ModuleManager::activateLocked(const ModuleManifest& manifest) {
    if (active_.contains(manifest.moduleID)) {
        return;
    }
    if (!manifest.isSchemaValid()) {
        throw std::runtime_error("Invalid manifest schema for " + manifest.moduleID);
    }
    if (entitlements_ && !entitlements_->isUnlocked(manifest.moduleID)) {
        throw std::runtime_error("Module is entitlement-locked: " + manifest.moduleID);
    }
    if (!registry_.contains(manifest.entryPoint)) {
        throw std::runtime_error("Unregistered entry point: " + manifest.entryPoint);
    }

    auto instance = registry_.create(manifest.entryPoint);
    if (instance->descriptor().moduleID != manifest.moduleID) {
        throw std::runtime_error("Factory identity mismatch for " + manifest.moduleID);
    }

    const bool isSurface =
        manifest.moduleType == ModuleType::UI || manifest.moduleType == ModuleType::App;
    if (isSurface && !activeSurfaceModuleID_.empty() && activeSurfaceModuleID_ != manifest.moduleID) {
        deactivate(activeSurfaceModuleID_);
    }

    auto context = std::make_unique<ModuleContext>(
        *logger_, *services_, *events_, manifest, *policy_);
    instance->start(*context);

    if (isSurface) {
        if (auto* ui = dynamic_cast<IForgeUIModule*>(instance.get())) {
            surface_->setActive(manifest.moduleID, ui->uiContributions());
        }
        activeSurfaceModuleID_ = manifest.moduleID;
    }

    ActiveModule active;
    active.instance = std::move(instance);
    active.context = std::move(context);
    active.manifest = manifest;
    active_.emplace(manifest.moduleID, std::move(active));
    logger_->log(LogLevel::Info, "module_activated", {{"module", manifest.moduleID}});
}

} // namespace Forge::Runtime
