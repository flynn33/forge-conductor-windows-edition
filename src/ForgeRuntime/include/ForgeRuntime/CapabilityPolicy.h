// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/Protocols.h"

namespace Forge::Runtime {

class DefaultCapabilityPolicy final : public ICapabilityPolicy {
public:
    bool allows(const ModuleManifest& manifest, Capability capability) const override;
};

class AlwaysUnlockedEntitlements final : public IEntitlementProvider {
public:
    bool isUnlocked(const std::string&) const override { return true; }
    void refreshEntitlements() override {}
    void onEntitlementsChanged(std::function<void()>) override {}
};

class DefaultCommunicationGuard final : public IModuleCommunicationGuard {
public:
    bool allow(const std::string& sourceID, const std::string& destinationID) const override;
};

class MemoryActivationStore final : public IActivationStore {
public:
    std::vector<std::string> loadActiveModuleIDs() const override { return active_; }
    void saveActiveModuleIDs(const std::vector<std::string>& moduleIDs) override { active_ = moduleIDs; }

private:
    std::vector<std::string> active_;
};

} // namespace Forge::Runtime
