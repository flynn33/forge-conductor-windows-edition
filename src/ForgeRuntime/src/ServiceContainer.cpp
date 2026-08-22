// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/ServiceContainer.h"

namespace Forge::Runtime {

void ServiceContainer::registerService(const std::type_index& type, std::shared_ptr<void> instance) {
    std::lock_guard lock(mutex_);
    services_[type] = std::move(instance);
}

std::shared_ptr<void> ServiceContainer::resolve(const std::type_index& type) const {
    std::lock_guard lock(mutex_);
    const auto it = services_.find(type);
    return it == services_.end() ? nullptr : it->second;
}

} // namespace Forge::Runtime
