// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/SemVer.h"

#include <optional>
#include <string>
#include <vector>

namespace Forge::Runtime {

enum class Platform { Windows };

enum class ModuleType { Service, UI, App };

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

enum class ManifestTemplateVersion { V1_0, V1_1 };

enum class DefaultModuleRole { None, UI, Service };

enum class ModuleIOKind { Storage, Network, File, Process };
enum class ModuleIOAccess { Read, Write, ReadWrite };
enum class ModuleDataIsolationMode { PrivateToModule, Shared };

struct ModuleIORequirement final {
    std::string requirementID;
    ModuleIOKind kind{ModuleIOKind::Storage};
    ModuleIOAccess access{ModuleIOAccess::ReadWrite};
    bool required{true};
    std::string description;
};

struct ModuleUIRequirements final {
    std::string controlSchemeID;
    std::string layoutID;
    std::vector<std::string> themeIDs;
    std::vector<std::string> viewIDs;
    std::vector<std::string> slotIDs;
    std::vector<std::string> toolbarItemIDs;
    std::vector<std::string> routeIDs;
};

struct ModuleDataIsolation final {
    ModuleDataIsolationMode mode{ModuleDataIsolationMode::PrivateToModule};
    std::vector<std::string> ownedStoreIDs;
    std::vector<std::string> requiredDefaultRoles;
};

struct ModuleRuntimeRequirements final {
    std::vector<ModuleIORequirement> io;
    std::optional<ModuleUIRequirements> ui;
    ModuleDataIsolation dataIsolation{};
};

struct ModuleDescriptor final {
    std::string moduleID;
    std::string displayName;
    SemVer version;
    ModuleType type{ModuleType::Service};

    [[nodiscard]] bool operator==(const ModuleDescriptor&) const = default;
};

struct ModuleManifest final {
    std::string schemaVersion{"1.1"};
    ManifestTemplateVersion manifestTemplateVersion{ManifestTemplateVersion::V1_1};
    std::string moduleID;
    std::string displayName;
    SemVer moduleVersion;
    ModuleType moduleType{ModuleType::Service};
    std::vector<Platform> supportedPlatforms{Platform::Windows};
    SemVer minForsettiVersion{0, 2, 0, std::nullopt};
    std::optional<SemVer> maxForsettiVersion;
    std::vector<Capability> capabilitiesRequested;
    std::optional<std::string> iapProductID;
    std::string entryPoint;
    std::optional<DefaultModuleRole> defaultModuleRole;
    ModuleRuntimeRequirements runtimeRequirements{};

    [[nodiscard]] bool isSchemaValid() const noexcept {
        return schemaVersion == "1.0" || schemaVersion == "1.1";
    }

    [[nodiscard]] ModuleDescriptor descriptor() const {
        return ModuleDescriptor{moduleID, displayName, moduleVersion, moduleType};
    }
};

[[nodiscard]] std::string toString(Platform platform);
[[nodiscard]] std::string toString(ModuleType type);
[[nodiscard]] std::string toString(Capability capability);
[[nodiscard]] Capability capabilityFromString(const std::string& value);
[[nodiscard]] ModuleType moduleTypeFromString(const std::string& value);

} // namespace Forge::Runtime
