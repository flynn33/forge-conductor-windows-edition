// Forsetti Framework for Windows
// Copyright (c) 2026 James Daley. All Rights Reserved.
// Proprietary and Confidential. Patent Pending.

#pragma once

#include "ForsettiCore/ModuleModels.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Forsetti {

struct ModuleRegistrationRecord final {
    std::string moduleID;
    std::string displayName;
    SemVer moduleVersion;
    ModuleType moduleType{ModuleType::Service};
    std::string entryPoint;
    std::string schemaVersion;
    ManifestTemplateVersion manifestTemplateVersion{ManifestTemplateVersion::V1_0};
    std::string canonicalManifestHash;
    std::vector<Platform> supportedPlatforms;
    std::vector<Capability> capabilitiesRequested;
    std::optional<DefaultModuleRole> defaultModuleRole;
    ModuleRuntimeRequirements runtimeRequirements;
    std::string registeredAt;
    std::string lastConfirmedAt;
    bool confirmed = false;

    bool operator==(const ModuleRegistrationRecord&) const = default;
};

class IModuleRegistrationStore {
public:
    virtual std::optional<ModuleRegistrationRecord> load(const std::string& moduleID) const = 0;
    virtual std::vector<ModuleRegistrationRecord> loadAll() const = 0;
    virtual void save(const ModuleRegistrationRecord& record) = 0;
    virtual void remove(const std::string& moduleID) = 0;

    virtual ~IModuleRegistrationStore() = default;
};

class IManifestDigestProvider {
public:
    virtual std::string canonicalManifestJSON(const ModuleManifest& manifest) const = 0;
    virtual std::string digestManifest(const ModuleManifest& manifest) const = 0;

    virtual ~IManifestDigestProvider() = default;
};

class IRegistrationClock {
public:
    virtual std::string now() const = 0;

    virtual ~IRegistrationClock() = default;
};

class InMemoryModuleRegistrationStore final : public IModuleRegistrationStore {
public:
    std::optional<ModuleRegistrationRecord> load(const std::string& moduleID) const override;
    std::vector<ModuleRegistrationRecord> loadAll() const override;
    void save(const ModuleRegistrationRecord& record) override;
    void remove(const std::string& moduleID) override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModuleRegistrationRecord> records_;
};

class SystemRegistrationClock final : public IRegistrationClock {
public:
    std::string now() const override;
};

class Sha256ManifestDigestProvider final : public IManifestDigestProvider {
public:
    std::string canonicalManifestJSON(const ModuleManifest& manifest) const override;
    std::string digestManifest(const ModuleManifest& manifest) const override;
};

class ModuleRegistrationService final {
public:
    ModuleRegistrationService(
        std::shared_ptr<IModuleRegistrationStore> store,
        std::shared_ptr<IManifestDigestProvider> digestProvider,
        std::shared_ptr<IRegistrationClock> clock);

    static std::shared_ptr<ModuleRegistrationService> makeInMemory();

    ModuleRegistrationRecord registerDiscoveredManifest(const ModuleManifest& manifest);
    ModuleRegistrationRecord confirmDiscoveredManifest(const ModuleManifest& manifest);
    std::vector<ModuleRegistrationRecord> confirmAllDiscoveredManifests(
        const std::vector<ModuleManifest>& manifests);
    void reconcileRemovedManifests(const std::vector<std::string>& discoveredModuleIDs);
    [[nodiscard]] std::vector<ModuleRegistrationRecord> registeredModules() const;
    [[nodiscard]] std::optional<ModuleRegistrationRecord> load(const std::string& moduleID) const;
    [[nodiscard]] bool isConfirmedMatch(const ModuleManifest& manifest) const;

private:
    [[nodiscard]] ModuleRegistrationRecord makeRecord(
        const ModuleManifest& manifest,
        const std::string& canonicalManifestHash,
        const std::string& registeredAt,
        const std::string& lastConfirmedAt,
        bool confirmed) const;
    [[nodiscard]] bool recordMatchesManifest(
        const ModuleRegistrationRecord& record,
        const ModuleManifest& manifest,
        const std::string& canonicalManifestHash) const;

    std::shared_ptr<IModuleRegistrationStore> store_;
    std::shared_ptr<IManifestDigestProvider> digestProvider_;
    std::shared_ptr<IRegistrationClock> clock_;
};

void to_json(nlohmann::json& j, const ModuleRegistrationRecord& record);
void from_json(const nlohmann::json& j, ModuleRegistrationRecord& record);

} // namespace Forsetti
