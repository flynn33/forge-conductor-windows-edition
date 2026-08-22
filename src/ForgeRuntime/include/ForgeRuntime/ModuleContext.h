// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/Protocols.h"

namespace Forge::Runtime {

class ModuleContext final : public IForgeModuleContext {
public:
    ModuleContext(
        IForgeLogger& logger,
        IServiceProvider& services,
        IForgeEventBus& events,
        ModuleManifest manifest,
        ICapabilityPolicy& policy);

    IForgeLogger& logger() override;
    IServiceProvider& services() override;
    IForgeEventBus& events() override;
    const ModuleManifest& manifest() const override;
    bool hasCapability(Capability capability) const override;

private:
    IForgeLogger& logger_;
    IServiceProvider& services_;
    IForgeEventBus& events_;
    ModuleManifest manifest_;
    ICapabilityPolicy& policy_;
};

} // namespace Forge::Runtime
