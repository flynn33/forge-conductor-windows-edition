#pragma once

#include "ForgeConductor/Contracts/IForgeApplicationLifecycle.h"

#include <ForsettiCore/ForsettiProtocols.h>
#include <ForsettiCore/ModuleRegistry.h>

#include <memory>
#include <mutex>

namespace ForgeConductor::ForsettiModule {

class ForgeConductorAppModule final : public Forsetti::IForsettiAppModule {
public:
    static constexpr const char* ModuleID = "com.forsetti.app.forge-conductor-windows";
    static constexpr const char* EntryPoint = "ForgeConductorAppModule";

    explicit ForgeConductorAppModule(
        std::shared_ptr<Contracts::IForgeApplicationLifecycle> lifecycle);

    [[nodiscard]] Forsetti::ModuleDescriptor descriptor() const override;
    [[nodiscard]] Forsetti::ModuleManifest manifest() const override;
    [[nodiscard]] Forsetti::UIContributions uiContributions() const override;

    void start(Forsetti::IForsettiModuleContext& context) override;
    void stop(Forsetti::IForsettiModuleContext& context) override;

private:
    enum class LifecycleState {
        Stopped,
        Starting,
        Started,
        Stopping
    };

    void recordLifecycleFailure(
        Forsetti::IForsettiModuleContext& context,
        const char* operation,
        const char* detail) const noexcept;

    std::shared_ptr<Contracts::IForgeApplicationLifecycle> lifecycle_;
    mutable std::mutex lifecycleMutex_;
    LifecycleState lifecycleState_{LifecycleState::Stopped};
};

void registerForgeConductorAppModule(
    Forsetti::ModuleRegistry& registry,
    std::shared_ptr<Contracts::IForgeApplicationLifecycle> lifecycle);

} // namespace ForgeConductor::ForsettiModule
