// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgePlatform/HttpClient.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Forge::Comfy {

struct ComfyCapabilities final {
    bool reachable{false};
    std::string baseUrl;
    nlohmann::json systemStats = nlohmann::json::object();
    nlohmann::json objectInfo = nlohmann::json::object();
    bool objectInfoAvailable{false};
    std::vector<std::pair<std::string, std::string>> routeSelections;
};

class ComfyClient final {
public:
    ComfyClient(std::string baseUrl, Platform::IHttpClient& http);

    nlohmann::json request(const std::string& operation, const std::string& method, const std::string& jsonBody = {});
    ComfyCapabilities discover();
    nlohmann::json objectInfo();
    bool trySystemStats(nlohmann::json& out);

private:
    nlohmann::json checked(const std::string& method, const std::string& path, const std::string& jsonBody);
    std::string baseUrl_;
    Platform::IHttpClient& http_;
    std::map<std::string, std::string> selectedRoutes_;
};

} // namespace Forge::Comfy
