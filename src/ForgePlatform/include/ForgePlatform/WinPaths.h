// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <string>

namespace Forge::Platform {

[[nodiscard]] std::filesystem::path userProfile();
[[nodiscard]] std::filesystem::path localAppData();
[[nodiscard]] std::filesystem::path currentExecutable();
[[nodiscard]] std::filesystem::path findUpwards(const std::filesystem::path& start, const std::filesystem::path& relative, int maxHops = 6);

} // namespace Forge::Platform
