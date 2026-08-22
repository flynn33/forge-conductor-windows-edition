// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeComfy/ComfyClient.h"

#include "ForgePlatform/HttpUrl.h"

#include <stdexcept>

namespace Forge::Comfy {
namespace {

const std::map<std::string, std::string> kRoutes{
    {"system_stats", "/system_stats"},
    {"object_info", "/object_info"},
    {"queue", "/queue"},
    {"history", "/history"},
    {"prompt", "/prompt"},
    {"interrupt", "/interrupt"},
    {"free", "/free"},
};

} // namespace

ComfyClient::ComfyClient(std::string baseUrl, Platform::IHttpClient& http)
    : baseUrl_(std::move(baseUrl))
    , http_(http) {}

nlohmann::json ComfyClient::checked(const std::string& method, const std::string& path, const std::string& jsonBody) {
    const auto url = Platform::joinUrl(baseUrl_, path);
    const auto response = http_.request(method, url, jsonBody);
    if (!response.error.empty() && response.status == 0) {
        throw std::runtime_error(response.error);
    }
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("ComfyUI HTTP " + std::to_string(response.status) + " for " + method + " " + path);
    }
    if (response.body.empty()) {
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(response.body);
    } catch (...) {
        throw std::runtime_error("ComfyUI returned non-JSON for " + path);
    }
}

nlohmann::json ComfyClient::request(const std::string& operation, const std::string& method, const std::string& jsonBody) {
    const auto routeIt = kRoutes.find(operation);
    if (routeIt == kRoutes.end()) {
        throw std::invalid_argument("unknown ComfyUI operation: " + operation);
    }
    const auto selected = selectedRoutes_.find(operation);
    if (selected != selectedRoutes_.end()) {
        return checked(method, selected->second, jsonBody);
    }
    const auto preferred = "/api" + routeIt->second;
    const auto url = Platform::joinUrl(baseUrl_, preferred);
    const auto response = http_.request(method, url, jsonBody);
    if (response.status != 404) {
        if (!response.error.empty() && response.status == 0) {
            throw std::runtime_error(response.error);
        }
        if (response.status < 200 || response.status >= 300) {
            throw std::runtime_error("ComfyUI HTTP " + std::to_string(response.status) + " for " + method + " " + preferred);
        }
        selectedRoutes_[operation] = preferred;
        if (response.body.empty()) {
            return nlohmann::json::object();
        }
        return nlohmann::json::parse(response.body);
    }
    const auto body = checked(method, routeIt->second, jsonBody);
    selectedRoutes_[operation] = routeIt->second;
    return body;
}

bool ComfyClient::trySystemStats(nlohmann::json& out) {
    try {
        out = request("system_stats", "GET");
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json ComfyClient::objectInfo() {
    auto body = request("object_info", "GET");
    if (!body.is_object()) {
        throw std::runtime_error("ComfyUI object_info response must be a JSON object");
    }
    return body;
}

ComfyCapabilities ComfyClient::discover() {
    ComfyCapabilities caps;
    caps.baseUrl = baseUrl_;
    nlohmann::json stats;
    caps.reachable = trySystemStats(stats);
    if (caps.reachable) {
        caps.systemStats = stats.is_object() ? stats : nlohmann::json::object();
    }
    try {
        caps.objectInfo = objectInfo();
        caps.objectInfoAvailable = true;
    } catch (...) {
        caps.objectInfoAvailable = false;
    }
    for (const auto& [op, path] : selectedRoutes_) {
        caps.routeSelections.emplace_back(op, path);
    }
    return caps;
}

} // namespace Forge::Comfy
