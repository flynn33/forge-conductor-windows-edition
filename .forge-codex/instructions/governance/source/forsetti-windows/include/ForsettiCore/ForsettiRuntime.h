// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once
#include "ForsettiCore/ModuleManager.h"
#include "ForsettiCore/ForsettiProtocols.h"
#include <memory>
#include <string>
#include <functional>
#include <atomic>

namespace Forsetti {

// ---------------------------------------------------------------------------
// ForsettiRuntime — top-level runtime orchestrator
// ---------------------------------------------------------------------------

class ForsettiRuntime final {
public:
    ForsettiRuntime(
        std::unique_ptr<ModuleManager> moduleManager,
        std::shared_ptr<IEntitlementProvider> entitlementProvider,
        std::shared_ptr<IForsettiEventBus> eventBus,
        std::string manifestDirectory
    );

    // Lifecycle
    void boot();
    void shutdown();

    // Entitlement reconciliation
    void reconcileActiveModulesWithEntitlements();

    // Module activation / deactivation (delegates to ModuleManager)
    void activateModule(const std::string& moduleID);
    void deactivateModule(const std::string& moduleID);

    // Getters
    [[nodiscard]] bool isBooted() const;
    [[nodiscard]] ModuleManager& moduleManager();
    [[nodiscard]] const ModuleManager& moduleManager() const;

private:
    std::unique_ptr<ModuleManager> moduleManager_;
    std::shared_ptr<IEntitlementProvider> entitlementProvider_;
    std::shared_ptr<IForsettiEventBus> eventBus_;
    std::string manifestDirectory_;
    std::atomic<bool> isBooted_{false};
};

} // namespace Forsetti
