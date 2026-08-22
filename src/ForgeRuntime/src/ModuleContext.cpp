// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/ModuleContext.h"

namespace Forge::Runtime {

ModuleContext::ModuleContext(
    IForgeLogger& logger,
    IServiceProvider& services,
    IForgeEventBus& events,
    ModuleManifest manifest,
    ICapabilityPolicy& policy)
    : logger_(logger)
    , services_(services)
    , events_(events)
    , manifest_(std::move(manifest))
    , policy_(policy) {}

IForgeLogger& ModuleContext::logger() { return logger_; }
IServiceProvider& ModuleContext::services() { return services_; }
IForgeEventBus& ModuleContext::events() { return events_; }
const ModuleManifest& ModuleContext::manifest() const { return manifest_; }

bool ModuleContext::hasCapability(Capability capability) const {
    return policy_.allows(manifest_, capability);
}

} // namespace Forge::Runtime
