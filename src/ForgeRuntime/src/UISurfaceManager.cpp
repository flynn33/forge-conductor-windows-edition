// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/UISurfaceManager.h"

namespace Forge::Runtime {

void UISurfaceManager::setActive(const std::string& moduleID, const UIContributions& contributions) {
    state_.activeModuleID = moduleID;
    state_.contributions = contributions;
}

void UISurfaceManager::clear() {
    state_ = {};
}

SurfaceStateSnapshot UISurfaceManager::snapshot() const {
    return state_;
}

} // namespace Forge::Runtime
