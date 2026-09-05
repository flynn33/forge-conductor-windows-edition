// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Forsetti {

enum class Capability;

enum class ManifestTemplateVersion {
    V1_0,
    V1_1
};

enum class DefaultModuleRole {
    UI,
    SharedDatabase,
    Authentication,
    Diagnostics,
    API,
    Security
};

enum class ModuleIOKind {
    Networking,
    Storage,
    SecureStorage,
    FileExport,
    CryptoUtilities,
    Telemetry,
    SharedDatabase,
    Authentication,
    Diagnostics,
    API,
    Security
};

enum class ModuleIOAccess {
    Read,
    Write,
    ReadWrite,
    Execute,
    Emit,
    Consume
};

enum class ModuleDataIsolationMode {
    PrivateToModule,
    FrameworkMediatedShared
};

std::string to_string(ManifestTemplateVersion version);
ManifestTemplateVersion manifestTemplateVersionFromString(const std::string& str);
void to_json(nlohmann::json& j, ManifestTemplateVersion version);
void from_json(const nlohmann::json& j, ManifestTemplateVersion& version);

std::string to_string(DefaultModuleRole role);
DefaultModuleRole defaultModuleRoleFromString(const std::string& str);
void to_json(nlohmann::json& j, DefaultModuleRole role);
void from_json(const nlohmann::json& j, DefaultModuleRole& role);

std::string to_string(ModuleIOKind kind);
ModuleIOKind moduleIOKindFromString(const std::string& str);
void to_json(nlohmann::json& j, ModuleIOKind kind);
void from_json(const nlohmann::json& j, ModuleIOKind& kind);

std::string to_string(ModuleIOAccess access);
ModuleIOAccess moduleIOAccessFromString(const std::string& str);
void to_json(nlohmann::json& j, ModuleIOAccess access);
void from_json(const nlohmann::json& j, ModuleIOAccess& access);

std::string to_string(ModuleDataIsolationMode mode);
ModuleDataIsolationMode moduleDataIsolationModeFromString(const std::string& str);
void to_json(nlohmann::json& j, ModuleDataIsolationMode mode);
void from_json(const nlohmann::json& j, ModuleDataIsolationMode& mode);

Capability capabilityForIOKind(ModuleIOKind kind);

struct ModuleIORequirement final {
    std::string requirementID;
    ModuleIOKind kind;
    ModuleIOAccess access;
    bool required = false;
    std::optional<std::string> description;

    bool operator==(const ModuleIORequirement&) const = default;
};

struct ModuleUIRequirements final {
    std::optional<std::string> controlSchemeID;
    std::optional<std::string> layoutID;
    std::vector<std::string> themeIDs;
    std::vector<std::string> viewIDs;
    std::vector<std::string> slotIDs;
    std::vector<std::string> toolbarItemIDs;
    std::vector<std::string> routeIDs;
    std::vector<std::string> pointerIDs;

    bool operator==(const ModuleUIRequirements&) const = default;
};

struct ModuleDataIsolation final {
    ModuleDataIsolationMode mode{ModuleDataIsolationMode::PrivateToModule};
    std::vector<std::string> ownedStoreIDs;
    std::vector<DefaultModuleRole> requiredDefaultRoles;

    bool operator==(const ModuleDataIsolation&) const = default;
};

struct ModuleRuntimeRequirements final {
    std::vector<ModuleIORequirement> io;
    std::optional<ModuleUIRequirements> ui;
    ModuleDataIsolation dataIsolation;

    bool operator==(const ModuleRuntimeRequirements&) const = default;
};

void to_json(nlohmann::json& j, const ModuleIORequirement& requirement);
void from_json(const nlohmann::json& j, ModuleIORequirement& requirement);

void to_json(nlohmann::json& j, const ModuleUIRequirements& requirements);
void from_json(const nlohmann::json& j, ModuleUIRequirements& requirements);

void to_json(nlohmann::json& j, const ModuleDataIsolation& isolation);
void from_json(const nlohmann::json& j, ModuleDataIsolation& isolation);

void to_json(nlohmann::json& j, const ModuleRuntimeRequirements& requirements);
void from_json(const nlohmann::json& j, ModuleRuntimeRequirements& requirements);

} // namespace Forsetti
