// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePersistence/ConfigStore.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace Forge::Persistence {
namespace {

nlohmann::json loadJson(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return nlohmann::json::object();
    }
    nlohmann::json json;
    stream >> json;
    return json;
}

Domain::AppConfig fromJson(const nlohmann::json& json) {
    Domain::AppConfig config;
    config.logLevel = json.value("log_level", "info");
    if (json.contains("allowed_roots") && json["allowed_roots"].is_array()) {
        config.allowedRoots = json["allowed_roots"].get<std::vector<std::string>>();
    }
    if (json.contains("shell")) {
        config.shellTimeoutSec = json["shell"].value("default_timeout_sec", 30);
    }
    if (json.contains("dashboard")) {
        config.dashboardHost = json["dashboard"].value("host", "127.0.0.1");
        config.dashboardPort = json["dashboard"].value("port", 7788);
    }
    if (json.contains("manager")) {
        config.managerAutoRestart = json["manager"].value("auto_restart", true);
        config.managerWatchdogSec = json["manager"].value("watchdog_interval_sec", 3);
        config.openOnStart = json["manager"].value("open_browser_on_start", false);
    }
    if (json.contains("mcp")) {
        config.mcpRole = json["mcp"].value("role", "primary");
    }
    if (json.contains("sessions")) {
        config.sessionIdleTtlSec = json["sessions"].value("idle_ttl_sec", 14400);
    }
    return config;
}

} // namespace

ConfigStore::ConfigStore(AppPaths paths) : paths_(std::move(paths)) {
    reload();
}

void ConfigStore::reload() {
    std::lock_guard lock(mutex_);
    config_ = fromJson(loadJson(paths_.configJSON()));
}

Domain::AppConfig ConfigStore::model() const {
    std::lock_guard lock(mutex_);
    return config_;
}

std::string ConfigStore::stringAt(const std::string& key, const std::string& fallback) const {
    const auto config = model();
    if (key == "log_level") return config.logLevel;
    if (key == "mcp.role") return config.mcpRole;
    if (key == "dashboard.host") return config.dashboardHost;
    return fallback;
}

int ConfigStore::intAt(const std::string& key, int fallback) const {
    const auto config = model();
    if (key == "shell.timeout") return config.shellTimeoutSec;
    if (key == "dashboard.port") return config.dashboardPort;
    if (key == "sessions.idle_ttl_sec") return config.sessionIdleTtlSec;
    return fallback;
}

bool ConfigStore::boolAt(const std::string& key, bool fallback) const {
    const auto config = model();
    if (key == "manager.auto_restart") return config.managerAutoRestart;
    if (key == "manager.open_on_start") return config.openOnStart;
    return fallback;
}

void ConfigStore::save(const Domain::AppConfig& config) {
    nlohmann::json json;
    json["log_level"] = config.logLevel;
    json["allowed_roots"] = config.allowedRoots;
    json["shell"]["default_timeout_sec"] = config.shellTimeoutSec;
    json["dashboard"]["host"] = config.dashboardHost;
    json["dashboard"]["port"] = config.dashboardPort;
    json["manager"]["auto_restart"] = config.managerAutoRestart;
    json["manager"]["watchdog_interval_sec"] = config.managerWatchdogSec;
    json["manager"]["open_browser_on_start"] = config.openOnStart;
    json["mcp"]["role"] = config.mcpRole;
    json["sessions"]["idle_ttl_sec"] = config.sessionIdleTtlSec;
    std::ofstream stream(paths_.configJSON(), std::ios::binary);
    stream << json.dump(2);
    std::lock_guard lock(mutex_);
    config_ = config;
}

} // namespace Forge::Persistence
