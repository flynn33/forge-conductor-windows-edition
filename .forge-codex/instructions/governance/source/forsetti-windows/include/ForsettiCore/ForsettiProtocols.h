// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once
#include "ForsettiCore/ModuleModels.h"
#include "ForsettiCore/UIModels.h"
#include <memory>
#include <string>
#include <functional>

namespace Forsetti {

class IForsettiModuleContext;

// ---------------------------------------------------------------------------
// IForsettiModule – base interface for every module in the framework.
// ---------------------------------------------------------------------------
class IForsettiModule {
public:
    virtual ModuleDescriptor descriptor() const = 0;
    virtual ModuleManifest   manifest()   const = 0;

    virtual void start(IForsettiModuleContext& context) = 0;
    virtual void stop(IForsettiModuleContext& context)  = 0;

    virtual ~IForsettiModule() = default;
};

// ---------------------------------------------------------------------------
// IForsettiUIModule – a module that contributes UI elements.
// ---------------------------------------------------------------------------
class IForsettiUIModule : public IForsettiModule {
public:
    virtual UIContributions uiContributions() const = 0;

    ~IForsettiUIModule() override = default;
};

// ---------------------------------------------------------------------------
// IForsettiAppModule – marker interface for application-level modules.
// ---------------------------------------------------------------------------
class IForsettiAppModule : public IForsettiUIModule {
public:
    ~IForsettiAppModule() override = default;
};

// ---------------------------------------------------------------------------
// IEntitlementProvider – abstraction over purchase / licensing validation.
// ---------------------------------------------------------------------------
class IEntitlementProvider {
public:
    virtual bool isUnlocked(const std::string& moduleIDOrProductID) const = 0;
    virtual void refreshEntitlements() = 0;
    virtual void onEntitlementsChanged(std::function<void()> callback) = 0;
    virtual void restorePurchases() = 0;

    virtual ~IEntitlementProvider() = default;
};

} // namespace Forsetti
