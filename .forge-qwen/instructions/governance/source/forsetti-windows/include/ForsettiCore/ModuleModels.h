// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once

#include "ForsettiCore/ModuleRequirements.h"
#include "ForsettiCore/SemVer.h"
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Forsetti {

// MARK: - Platform

enum class Platform {
    Windows
};

/// Returns the platform this binary was compiled for.
static constexpr Platform currentPlatform() noexcept {
    return Platform::Windows;
}

std::string to_string(Platform platform);
Platform platformFromString(const std::string& str);

void to_json(nlohmann::json& j, Platform platform);
void from_json(const nlohmann::json& j, Platform& platform);

// MARK: - ModuleType

enum class ModuleType {
    Service,
    UI,
    App
};

std::string to_string(ModuleType type);
ModuleType moduleTypeFromString(const std::string& str);

void to_json(nlohmann::json& j, ModuleType type);
void from_json(const nlohmann::json& j, ModuleType& type);

// MARK: - Capability

enum class Capability {
    Networking,
    Storage,
    SecureStorage,
    FileExport,
    CryptoUtilities,
    Telemetry,
    RoutingOverlay,
    ToolbarItems,
    ViewInjection,
    UIThemeMask,
    EventPublishing,
    SharedDatabase,
    Authentication,
    Diagnostics,
    API,
    Security
};

std::string to_string(Capability capability);
Capability capabilityFromString(const std::string& str);

void to_json(nlohmann::json& j, Capability capability);
void from_json(const nlohmann::json& j, Capability& capability);

// MARK: - ModuleDescriptor

struct ModuleDescriptor final {
    std::string moduleID;
    std::string displayName;
    SemVer version;
    ModuleType type;

    bool operator==(const ModuleDescriptor&) const = default;
};

void to_json(nlohmann::json& j, const ModuleDescriptor& descriptor);
void from_json(const nlohmann::json& j, ModuleDescriptor& descriptor);

// MARK: - ModuleManifest

struct ModuleManifest final {
    std::string schemaVersion;
    std::string moduleID;
    std::string displayName;
    SemVer moduleVersion;
    ModuleType moduleType;
    std::vector<Platform> supportedPlatforms;
    SemVer minForsettiVersion;
    std::optional<SemVer> maxForsettiVersion;
    std::vector<Capability> capabilitiesRequested;
    std::optional<std::string> iapProductID;
    std::string entryPoint;
    ManifestTemplateVersion manifestTemplateVersion{ManifestTemplateVersion::V1_0};
    std::optional<DefaultModuleRole> defaultModuleRole;
    ModuleRuntimeRequirements runtimeRequirements{};

    /// Validates that the schemaVersion is supported by this runtime.
    [[nodiscard]] bool isSchemaValid() const noexcept {
        return schemaVersion == "1.0" || schemaVersion == "1.1";
    }

    bool operator==(const ModuleManifest&) const = default;
};

void to_json(nlohmann::json& j, const ModuleManifest& manifest);
void from_json(const nlohmann::json& j, ModuleManifest& manifest);

} // namespace Forsetti
