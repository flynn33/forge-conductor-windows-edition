// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <optional>

namespace Forge::Host {

struct LaunchOptions final {
    std::optional<std::filesystem::path> home;
    std::filesystem::path bundledAgents;
    std::filesystem::path executable;
    std::filesystem::path manifestDirectory;
    bool headless{false};
};

} // namespace Forge::Host
