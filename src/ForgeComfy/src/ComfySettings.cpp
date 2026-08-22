// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeComfy/ComfySettings.h"

#include "ForgePersistence/AppPaths.h"
#include "ForgePlatform/HttpUrl.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace Forge::Comfy {

ComfySettings loadComfySettings(const Persistence::AppPaths& paths) {
    ComfySettings settings;
    std::ifstream stream(paths.configJSON(), std::ios::binary);
    if (!stream) {
        settings.baseUrl = Platform::validateLoopbackHttpBaseUrl(settings.baseUrl, true);
        return settings;
    }
    nlohmann::json json;
    try {
        stream >> json;
    } catch (...) {
        throw std::invalid_argument("config.json is not valid JSON");
    }
    if (!json.contains("comfy") || !json["comfy"].is_object()) {
        settings.baseUrl = Platform::validateLoopbackHttpBaseUrl(settings.baseUrl, true);
        return settings;
    }
    const auto& comfy = json["comfy"];
    settings.enabled = comfy.value("enabled", true);
    settings.executionPolicy = comfy.value("execution_policy", settings.executionPolicy);
    settings.transport = comfy.value("transport", settings.transport);
    settings.baseUrl = comfy.value("base_url", settings.baseUrl);
    if (comfy.contains("comfy_root")) {
        settings.comfyRoot = std::filesystem::path(comfy["comfy_root"].get<std::string>());
    }
    if (comfy.contains("comfy_python")) {
        settings.comfyPython = std::filesystem::path(comfy["comfy_python"].get<std::string>());
    }
    if (comfy.contains("comfy_output")) {
        settings.comfyOutput = std::filesystem::path(comfy["comfy_output"].get<std::string>());
    }
    if (settings.executionPolicy != "prepare_only" && settings.executionPolicy != "full") {
        throw std::invalid_argument("comfy.execution_policy must be prepare_only or full");
    }
    if (settings.transport != "loopback" && settings.transport != "remote") {
        throw std::invalid_argument("comfy.transport must be loopback or remote");
    }
    if (settings.transport == "remote") {
        throw std::invalid_argument("comfy.transport=remote is not implemented yet");
    }
    settings.baseUrl = Platform::validateLoopbackHttpBaseUrl(settings.baseUrl, true);
    return settings;
}

} // namespace Forge::Comfy
