// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/Runtime.h"

namespace Forge::Runtime {

ForgeRuntime::ForgeRuntime(std::unique_ptr<ModuleManager> manager, std::string manifestDirectory)
    : manager_(std::move(manager))
    , manifestDirectory_(std::move(manifestDirectory)) {}

void ForgeRuntime::boot() {
    if (booted_) {
        return;
    }
    manager_->discover(manifestDirectory_);
    manager_->restore();
    booted_ = true;
}

void ForgeRuntime::shutdown() {
    if (!booted_) {
        return;
    }
    manager_->shutdown();
    booted_ = false;
}

void ForgeRuntime::activate(const std::string& moduleID) {
    manager_->activate(moduleID);
}

void ForgeRuntime::deactivate(const std::string& moduleID) {
    manager_->deactivate(moduleID);
}

} // namespace Forge::Runtime
