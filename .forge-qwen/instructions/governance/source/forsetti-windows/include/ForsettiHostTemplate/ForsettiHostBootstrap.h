// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once

#include "ForsettiCore/ActivationStore.h"
#include "ForsettiCore/CapabilityPolicy.h"
#include "ForsettiCore/EntitlementProviders.h"
#include "ForsettiCore/ForsettiContext.h"
#include "ForsettiCore/ForsettiEventBus.h"
#include "ForsettiCore/ForsettiLogger.h"
#include "ForsettiCore/ForsettiRuntime.h"
#include "ForsettiCore/ForsettiServiceContainer.h"
#include "ForsettiCore/ModuleRegistry.h"
#include "ForsettiCore/ModuleRegistration.h"
#include "ForsettiCore/ModuleRequirementValidator.h"
#include "ForsettiCore/UISurfaceManager.h"
#include "ForsettiHostTemplate/ForsettiHostController.h"

#include <memory>
#include <string>

namespace Forsetti {

struct ForsettiHostBootstrapConfiguration final {
    std::string manifestDirectory;
    ModuleRegistry moduleRegistry;
    std::shared_ptr<IActivationStore> activationStore;
    std::shared_ptr<IEntitlementProvider> entitlementProvider;
    std::shared_ptr<ICapabilityPolicy> capabilityPolicy;
    std::shared_ptr<IServiceProvider> services;
    std::shared_ptr<IForsettiEventBus> eventBus;
    std::shared_ptr<IForsettiLogger> logger;
    std::shared_ptr<IOverlayRouter> overlayRouter;
    std::shared_ptr<IModuleCommunicationGuard> communicationGuard;
    std::shared_ptr<UISurfaceManager> surfaceManager;
    std::shared_ptr<ModuleRegistrationService> registrationService;
    std::shared_ptr<ModuleRequirementValidator> requirementValidator;
};

class IForsettiHostBootstrap {
public:
    virtual std::shared_ptr<IForsettiHostController> makeController(
        ForsettiHostBootstrapConfiguration configuration) const = 0;

    virtual ~IForsettiHostBootstrap() = default;
};

class ForsettiHostBootstrap final : public IForsettiHostBootstrap {
public:
    std::shared_ptr<IForsettiHostController> makeController(
        ForsettiHostBootstrapConfiguration configuration) const override;
};

} // namespace Forsetti
