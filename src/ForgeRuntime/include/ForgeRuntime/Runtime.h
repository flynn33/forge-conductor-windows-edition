// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/ModuleManager.h"

#include <memory>
#include <string>

namespace Forge::Runtime {

class ForgeRuntime final {
public:
    explicit ForgeRuntime(std::unique_ptr<ModuleManager> manager, std::string manifestDirectory);

    void boot();
    void shutdown();
    void activate(const std::string& moduleID);
    void deactivate(const std::string& moduleID);

    [[nodiscard]] bool isBooted() const noexcept { return booted_; }
    [[nodiscard]] ModuleManager& modules() { return *manager_; }

private:
    std::unique_ptr<ModuleManager> manager_;
    std::string manifestDirectory_;
    bool booted_{false};
};

} // namespace Forge::Runtime
