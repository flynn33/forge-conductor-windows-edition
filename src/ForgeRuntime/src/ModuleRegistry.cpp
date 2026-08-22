// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/ModuleRegistry.h"

#include <stdexcept>

namespace Forge::Runtime {

void ModuleRegistry::add(const std::string& entryPoint, ModuleFactory factory) {
    factories_[entryPoint] = std::move(factory);
}

std::shared_ptr<IForgeModule> ModuleRegistry::create(const std::string& entryPoint) const {
    const auto it = factories_.find(entryPoint);
    if (it == factories_.end()) {
        throw std::runtime_error("No factory registered for entry point: " + entryPoint);
    }
    return it->second();
}

bool ModuleRegistry::contains(const std::string& entryPoint) const {
    return factories_.contains(entryPoint);
}

} // namespace Forge::Runtime
