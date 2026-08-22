// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/Runtime.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Forge::Host {

struct HostBundle final {
    std::unique_ptr<Runtime::ForgeRuntime> runtime;
    std::shared_ptr<Runtime::IServiceProvider> services;
};

class HostBootstrap final {
public:
    HostBundle boot(
        const std::filesystem::path& manifestDirectory,
        Runtime::ModuleRegistry registry) const;
};

} // namespace Forge::Host
