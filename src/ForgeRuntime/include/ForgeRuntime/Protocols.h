// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/ModuleModels.h"
#include "ForgeRuntime/UIModels.h"

#include <functional>
#include <memory>
#include <string>

namespace Forge::Runtime {

class IForgeModuleContext;
class IForgeEventBus;
class IForgeLogger;
class IServiceProvider;

class IForgeModule {
public:
    virtual ModuleDescriptor descriptor() const = 0;
    virtual ModuleManifest manifest() const = 0;
    virtual void start(IForgeModuleContext& context) = 0;
    virtual void stop(IForgeModuleContext& context) = 0;
    virtual ~IForgeModule() = default;

protected:
    IForgeModule() = default;
    IForgeModule(const IForgeModule&) = delete;
    IForgeModule& operator=(const IForgeModule&) = delete;
};

class IForgeUIModule : public IForgeModule {
public:
    virtual UIContributions uiContributions() const = 0;
};

class IForgeAppModule : public IForgeUIModule {
};

class IEntitlementProvider {
public:
    virtual bool isUnlocked(const std::string& moduleIDOrProductID) const = 0;
    virtual void refreshEntitlements() = 0;
    virtual void onEntitlementsChanged(std::function<void()> callback) = 0;
    virtual ~IEntitlementProvider() = default;
};

class ICapabilityPolicy {
public:
    virtual bool allows(const ModuleManifest& manifest, Capability capability) const = 0;
    virtual ~ICapabilityPolicy() = default;
};

class IActivationStore {
public:
    virtual std::vector<std::string> loadActiveModuleIDs() const = 0;
    virtual void saveActiveModuleIDs(const std::vector<std::string>& moduleIDs) = 0;
    virtual ~IActivationStore() = default;
};

class IModuleCommunicationGuard {
public:
    virtual bool allow(const std::string& sourceID, const std::string& destinationID) const = 0;
    virtual ~IModuleCommunicationGuard() = default;
};

class IForgeModuleContext {
public:
    virtual IForgeLogger& logger() = 0;
    virtual IServiceProvider& services() = 0;
    virtual IForgeEventBus& events() = 0;
    virtual const ModuleManifest& manifest() const = 0;
    virtual bool hasCapability(Capability capability) const = 0;
    virtual ~IForgeModuleContext() = default;
};

using ModuleFactory = std::function<std::shared_ptr<IForgeModule>()>;

} // namespace Forge::Runtime
