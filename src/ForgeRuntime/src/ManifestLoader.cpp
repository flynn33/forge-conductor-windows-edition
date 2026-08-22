// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeRuntime/ManifestLoader.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Forge::Runtime {
namespace {

SemVer parseVersion(const nlohmann::json& value) {
    if (value.is_string()) {
        return SemVer::parse(value.get<std::string>());
    }
    SemVer version;
    version.major = value.value("major", 0u);
    version.minor = value.value("minor", 0u);
    version.patch = value.value("patch", 0u);
    if (value.contains("prerelease") && !value["prerelease"].is_null()) {
        version.prerelease = value["prerelease"].get<std::string>();
    }
    return version;
}

std::string readAll(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read manifest: " + path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

ModuleManifest ManifestLoader::parseJson(const std::string& jsonText) const {
    const auto json = nlohmann::json::parse(jsonText);
    ModuleManifest manifest;
    manifest.schemaVersion = json.value("schemaVersion", "1.0");
    manifest.moduleID = json.at("moduleID").get<std::string>();
    manifest.displayName = json.at("displayName").get<std::string>();
    manifest.moduleVersion = parseVersion(json.at("moduleVersion"));
    manifest.moduleType = moduleTypeFromString(json.at("moduleType").get<std::string>());
    manifest.entryPoint = json.at("entryPoint").get<std::string>();
    if (json.contains("minForsettiVersion")) {
        manifest.minForsettiVersion = parseVersion(json["minForsettiVersion"]);
    }
    if (json.contains("maxForsettiVersion") && !json["maxForsettiVersion"].is_null()) {
        manifest.maxForsettiVersion = parseVersion(json["maxForsettiVersion"]);
    }
    if (json.contains("capabilitiesRequested")) {
        for (const auto& item : json["capabilitiesRequested"]) {
            manifest.capabilitiesRequested.push_back(capabilityFromString(item.get<std::string>()));
        }
    }
    if (json.contains("iapProductID") && !json["iapProductID"].is_null()) {
        manifest.iapProductID = json["iapProductID"].get<std::string>();
    }
    if (json.value("manifestTemplateVersion", "1.0") == "1.1") {
        manifest.manifestTemplateVersion = ManifestTemplateVersion::V1_1;
    }
    if (json.contains("defaultModuleRole") && !json["defaultModuleRole"].is_null()) {
        const auto role = json["defaultModuleRole"].get<std::string>();
        if (role == "ui") {
            manifest.defaultModuleRole = DefaultModuleRole::UI;
        } else if (role == "service") {
            manifest.defaultModuleRole = DefaultModuleRole::Service;
        }
    }
    if (json.contains("runtimeRequirements") && json["runtimeRequirements"].is_object()) {
        const auto& req = json["runtimeRequirements"];
        if (req.contains("io") && req["io"].is_array()) {
            for (const auto& item : req["io"]) {
                ModuleIORequirement io;
                io.requirementID = item.value("requirementID", "");
                io.description = item.value("description", "");
                io.required = item.value("required", true);
                manifest.runtimeRequirements.io.push_back(std::move(io));
            }
        }
        if (req.contains("ui") && req["ui"].is_object()) {
            ModuleUIRequirements ui;
            ui.controlSchemeID = req["ui"].value("controlSchemeID", "");
            ui.layoutID = req["ui"].value("layoutID", "");
            if (req["ui"].contains("viewIDs")) {
                ui.viewIDs = req["ui"]["viewIDs"].get<std::vector<std::string>>();
            }
            if (req["ui"].contains("slotIDs")) {
                ui.slotIDs = req["ui"]["slotIDs"].get<std::vector<std::string>>();
            }
            if (req["ui"].contains("toolbarItemIDs")) {
                ui.toolbarItemIDs = req["ui"]["toolbarItemIDs"].get<std::vector<std::string>>();
            }
            manifest.runtimeRequirements.ui = std::move(ui);
        }
        if (req.contains("dataIsolation") && req["dataIsolation"].is_object()) {
            const auto mode = req["dataIsolation"].value("mode", "private_to_module");
            manifest.runtimeRequirements.dataIsolation.mode =
                mode == "shared" ? ModuleDataIsolationMode::Shared
                                 : ModuleDataIsolationMode::PrivateToModule;
        }
    }
    if (!manifest.isSchemaValid()) {
        throw std::runtime_error("Unsupported manifest schema: " + manifest.schemaVersion);
    }
    return manifest;
}

ModuleManifest ManifestLoader::loadFile(const std::string& path) const {
    return parseJson(readAll(path));
}

std::vector<ModuleManifest> ManifestLoader::loadDirectory(const std::string& directory) const {
    std::vector<ModuleManifest> manifests;
    if (!std::filesystem::exists(directory)) {
        return manifests;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        manifests.push_back(loadFile(entry.path().string()));
    }
    return manifests;
}

} // namespace Forge::Runtime
