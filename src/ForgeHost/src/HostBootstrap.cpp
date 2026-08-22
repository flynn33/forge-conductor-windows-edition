// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeHost/HostBootstrap.h"

#include "ForgeRuntime/CapabilityPolicy.h"
#include "ForgeRuntime/EventBus.h"
#include "ForgeRuntime/Logger.h"
#include "ForgeRuntime/ServiceContainer.h"
#include "ForgeRuntime/UISurfaceManager.h"

namespace Forge::Host {

HostBundle HostBootstrap::boot(
    const std::filesystem::path& manifestDirectory,
    Runtime::ModuleRegistry registry) const {
    HostBundle bundle;
    bundle.services = std::make_shared<Runtime::ServiceContainer>();
    auto logger = std::make_shared<Runtime::ConsoleLogger>();
    auto events = std::make_shared<Runtime::InMemoryEventBus>();
    auto entitlements = std::make_shared<Runtime::AlwaysUnlockedEntitlements>();
    auto policy = std::make_shared<Runtime::DefaultCapabilityPolicy>();
    auto activation = std::make_shared<Runtime::MemoryActivationStore>();
    auto surface = std::make_shared<Runtime::UISurfaceManager>();
    auto guard = std::make_shared<Runtime::DefaultCommunicationGuard>();

    auto manager = std::make_unique<Runtime::ModuleManager>(
        std::move(registry),
        entitlements,
        policy,
        activation,
        events,
        logger,
        bundle.services,
        surface,
        guard);
    bundle.runtime = std::make_unique<Runtime::ForgeRuntime>(
        std::move(manager),
        manifestDirectory.string());
    bundle.runtime->boot();
    return bundle;
}

} // namespace Forge::Host
