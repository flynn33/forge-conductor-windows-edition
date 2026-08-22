// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/ModuleContext.h"
#include "ForgeRuntime/ModuleRegistry.h"
#include "ForgeRuntime/UISurfaceManager.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Forge::Runtime {

class ModuleManager final {
public:
    ModuleManager(
        ModuleRegistry registry,
        std::shared_ptr<IEntitlementProvider> entitlements,
        std::shared_ptr<ICapabilityPolicy> policy,
        std::shared_ptr<IActivationStore> activationStore,
        std::shared_ptr<IForgeEventBus> events,
        std::shared_ptr<IForgeLogger> logger,
        std::shared_ptr<IServiceProvider> services,
        std::shared_ptr<UISurfaceManager> surface,
        std::shared_ptr<IModuleCommunicationGuard> guard);

    void discover(const std::string& manifestDirectory);
    void activate(const std::string& moduleID);
    void deactivate(const std::string& moduleID);
    void restore();
    void shutdown();

    [[nodiscard]] const std::vector<ModuleManifest>& manifests() const { return manifests_; }
    [[nodiscard]] bool isActive(const std::string& moduleID) const;
    [[nodiscard]] UISurfaceManager& surface() { return *surface_; }

private:
    struct ActiveModule {
        std::shared_ptr<IForgeModule> instance;
        std::unique_ptr<ModuleContext> context;
        ModuleManifest manifest;
    };

    void persist() const;
    void activateLocked(const ModuleManifest& manifest);

    ModuleRegistry registry_;
    std::shared_ptr<IEntitlementProvider> entitlements_;
    std::shared_ptr<ICapabilityPolicy> policy_;
    std::shared_ptr<IActivationStore> activationStore_;
    std::shared_ptr<IForgeEventBus> events_;
    std::shared_ptr<IForgeLogger> logger_;
    std::shared_ptr<IServiceProvider> services_;
    std::shared_ptr<UISurfaceManager> surface_;
    std::shared_ptr<IModuleCommunicationGuard> guard_;
    std::vector<ModuleManifest> manifests_;
    std::unordered_map<std::string, ActiveModule> active_;
    std::string activeSurfaceModuleID_;
};

} // namespace Forge::Runtime
