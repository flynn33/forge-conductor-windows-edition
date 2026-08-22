// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePersistence/AppPaths.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ShlObj.h>
#include <windows.h>
#endif

namespace Forge::Persistence {
namespace {

std::filesystem::path defaultHome() {
    if (const char* env = std::getenv("FORGE_CONDUCTOR_HOME"); env && *env) {
        return std::filesystem::path(env);
    }
#ifdef _WIN32
    PWSTR profile = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) {
        std::filesystem::path home = std::filesystem::path(profile) / ".forge-conductor";
        CoTaskMemFree(profile);
        return home;
    }
#endif
    if (const char* user = std::getenv("USERPROFILE"); user && *user) {
        return std::filesystem::path(user) / ".forge-conductor";
    }
    throw std::runtime_error("Cannot resolve user profile for Forge home");
}

} // namespace

AppPaths::AppPaths(std::optional<std::filesystem::path> home)
    : home_(home ? std::filesystem::weakly_canonical(*home) : defaultHome()) {}

void AppPaths::ensureLayout() const {
    const std::filesystem::path dirs[] = {
        home_,
        agentsDir(),
        logsDir(),
        memoryDir(),
        memoryHandoffsDir(),
        exportsDir(),
        home_ / "cache",
    };
    for (const auto& dir : dirs) {
        std::filesystem::create_directories(dir);
    }
    if (!std::filesystem::exists(configJSON())) {
        constexpr const char* kDefault = R"({
  "log_level": "info",
  "allowed_roots": [],
  "shell": { "default_timeout_sec": 30 },
  "dashboard": { "host": "127.0.0.1", "port": 7788, "refresh_interval_sec": 8 },
  "manager": { "auto_restart": true, "watchdog_interval_sec": 3, "open_browser_on_start": false },
  "mcp": { "role": "primary" },
  "sessions": { "idle_ttl_sec": 14400 },
  "comfy": {
    "enabled": true,
    "execution_policy": "prepare_only",
    "transport": "loopback",
    "base_url": "http://127.0.0.1:8188",
    "comfy_root": "A:\\ComfyUI\\ComfyUI_windows_portable\\ComfyUI",
    "comfy_python": "A:\\ComfyUI\\ComfyUI_windows_portable\\python_embeded\\python.exe",
    "comfy_output": "A:\\ComfyUI\\ComfyUI_windows_portable\\ComfyUI\\output"
  }
}
)";
        std::ofstream stream(configJSON(), std::ios::binary);
        stream << kDefault;
    }
}

} // namespace Forge::Persistence
