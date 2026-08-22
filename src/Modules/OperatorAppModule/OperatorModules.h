// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/ModuleRegistry.h"
#include "ForgeRuntime/Protocols.h"

namespace Forge::Modules {

class PersistenceModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class OrchestrationModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class McpModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class TelemetryModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class LmStudioModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class ComfyModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class ManagerModule final : public Runtime::IForgeModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

class OperatorAppModule final : public Runtime::IForgeAppModule {
public:
    Runtime::ModuleDescriptor descriptor() const override;
    Runtime::ModuleManifest manifest() const override;
    Runtime::UIContributions uiContributions() const override;
    void start(Runtime::IForgeModuleContext& context) override;
    void stop(Runtime::IForgeModuleContext& context) override;
};

void registerAllModules(Runtime::ModuleRegistry& registry);

} // namespace Forge::Modules
