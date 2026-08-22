// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeLmStudio/LmStudioDeploy.h"

#include "ForgeDomain/Clock.h"

#include <nlohmann/json.hpp>

#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ShlObj.h>
#include <windows.h>
#endif

namespace Forge::LmStudio {
namespace {

std::filesystem::path userProfile() {
#ifdef _WIN32
    PWSTR profile = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) {
        std::filesystem::path path(profile);
        CoTaskMemFree(profile);
        return path;
    }
#endif
    if (const char* env = std::getenv("USERPROFILE")) {
        return env;
    }
    return {};
}

nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return nlohmann::json::object();
    }
    nlohmann::json json;
    try {
        stream >> json;
    } catch (...) {
        json = nlohmann::json::object();
    }
    return json;
}

void writeJson(const std::filesystem::path& path, const nlohmann::json& json) {
    std::filesystem::create_directories(path.parent_path());
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream stream(tmp, std::ios::binary);
        stream << json.dump(2);
    }
    std::filesystem::rename(tmp, path);
}

nlohmann::json serverEntry(const std::filesystem::path& exe, const std::string& role, const std::string& home, const std::string& deployment) {
    nlohmann::json env;
    env["FORGE_MCP_ROLE"] = role;
    env["FORGE_CONDUCTOR_HOME"] = home;
    env["FORGE_DEPLOYMENT_ID"] = deployment;
    return {
        {"command", exe.string()},
        {"args", nlohmann::json::array({"serve"})},
        {"env", env},
    };
}

void writePlugin(
    const std::filesystem::path& dir,
    const std::string& name,
    const std::filesystem::path& exe,
    const std::string& role,
    const std::string& home,
    const std::string& deployment) {
    std::filesystem::create_directories(dir);
    nlohmann::json manifest{
        {"name", name},
        {"displayName", name},
        {"runner", "mcpBridge"},
        {"version", "0.8.0"},
    };
    writeJson(dir / "manifest.json", manifest);
    writeJson(dir / "mcp-bridge-config.json", serverEntry(exe, role, home, deployment));
    writeJson(dir / "install-state.json", {{"ok", true}, {"deploymentID", deployment}});
}

} // namespace

LmStudioDeployService::LmStudioDeployService(
    Persistence::AppPaths paths,
    std::filesystem::path executable,
    std::optional<std::filesystem::path> lmStudioHome)
    : paths_(std::move(paths))
    , executable_(std::move(executable))
    , lmStudioHomeOverride_(std::move(lmStudioHome)) {}

std::filesystem::path LmStudioDeployService::lmStudioHome() const {
    if (lmStudioHomeOverride_) {
        return *lmStudioHomeOverride_;
    }
    return userProfile() / ".lmstudio";
}

std::filesystem::path LmStudioDeployService::mcpJsonPath() const {
    return lmStudioHome() / "mcp.json";
}

Domain::DoctorReport LmStudioDeployService::status() {
    Domain::DoctorReport report;
    auto check = [&](const std::string& name, bool ok, const std::string& detail) {
        if (!ok) report.ok = false;
        report.checks.push_back({name, ok, detail, true});
    };
    const auto home = lmStudioHome();
    const auto mcp = mcpJsonPath();
    check("lmstudio_home", std::filesystem::exists(home), home.string());
    check("executable", std::filesystem::exists(executable_), executable_.string());
    const auto json = readJson(mcp);
    const bool registered = json.contains("mcpServers") &&
        json["mcpServers"].contains("forge-conductor") &&
        json["mcpServers"].contains("forge-conductor-fallback") &&
        json["mcpServers"].contains("comfy-control");
    check("mcp_json", registered, mcp.string());
    const auto plugins = home / "extensions" / "plugins" / "mcp";
    check("primary_plugin", std::filesystem::exists(plugins / "forge-conductor" / "manifest.json"),
        (plugins / "forge-conductor").string());
    check("fallback_plugin", std::filesystem::exists(plugins / "forge-conductor-fallback" / "manifest.json"),
        (plugins / "forge-conductor-fallback").string());
    check("comfy_plugin", std::filesystem::exists(plugins / "comfy-control" / "manifest.json"),
        (plugins / "comfy-control").string());
    return report;
}

Domain::DoctorReport LmStudioDeployService::deploy() {
    Domain::DoctorReport report;
    if (!std::filesystem::exists(executable_)) {
        report.ok = false;
        report.checks.push_back({"executable", false, "Forge executable not found", true});
        return report;
    }
    const auto home = lmStudioHome();
    std::filesystem::create_directories(home);
    const auto deployment = Domain::makeUuid();
    const auto forgeHome = paths_.home().string();
    auto json = readJson(mcpJsonPath());
    if (!json.is_object()) {
        report.ok = false;
        report.checks.push_back({"mcp_json", false, "Existing mcp.json is malformed; aborting", true});
        return report;
    }
    if (!json.contains("mcpServers") || !json["mcpServers"].is_object()) {
        json["mcpServers"] = nlohmann::json::object();
    }
    json["mcpServers"]["forge-conductor"] = serverEntry(executable_, "primary", forgeHome, deployment);
    json["mcpServers"]["forge-conductor-fallback"] = serverEntry(executable_, "fallback", forgeHome, deployment);
    json["mcpServers"]["comfy-control"] = serverEntry(executable_, "comfy", forgeHome, deployment);

    const auto plugins = home / "extensions" / "plugins" / "mcp";
    writePlugin(plugins / "forge-conductor", "forge-conductor", executable_, "primary", forgeHome, deployment);
    writePlugin(plugins / "forge-conductor-fallback", "forge-conductor-fallback", executable_, "fallback", forgeHome, deployment);
    writePlugin(plugins / "comfy-control", "comfy-control", executable_, "comfy", forgeHome, deployment);
    writeJson(mcpJsonPath(), json);

    report.checks.push_back({"deploy", true, "Wrote primary + fallback + comfy-control (" + deployment + ")", true});
    report.checks.push_back({"mcp_json", true, mcpJsonPath().string(), true});
    return report;
}

} // namespace Forge::LmStudio
