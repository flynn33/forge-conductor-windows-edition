// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/Protocols.h"

#include <string>
#include <unordered_map>

namespace Forge::Runtime {

class ModuleRegistry final {
public:
    void add(const std::string& entryPoint, ModuleFactory factory);
    [[nodiscard]] std::shared_ptr<IForgeModule> create(const std::string& entryPoint) const;
    [[nodiscard]] bool contains(const std::string& entryPoint) const;

private:
    std::unordered_map<std::string, ModuleFactory> factories_;
};

} // namespace Forge::Runtime
