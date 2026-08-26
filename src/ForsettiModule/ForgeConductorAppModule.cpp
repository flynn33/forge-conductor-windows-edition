#include "ForgeConductor/ForsettiModule/ForgeConductorAppModule.h"

#include <ForsettiCore/ForsettiContext.h>
#include <ForsettiCore/ForsettiLogger.h>
#include <ForsettiCore/ModuleRequirements.h>
#include <ForsettiCore/UIModels.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace ForgeConductor::ForsettiModule {
namespace {

constexpr const char* DisplayName = "Forge Conductor";

Forsetti::ModuleRuntimeRequirements makeRuntimeRequirements()
{
    using Forsetti::DefaultModuleRole;
    using Forsetti::ModuleDataIsolation;
    using Forsetti::ModuleDataIsolationMode;
    using Forsetti::ModuleIOAccess;
    using Forsetti::ModuleIOKind;
    using Forsetti::ModuleIORequirement;
    using Forsetti::ModuleRuntimeRequirements;
    using Forsetti::ModuleUIRequirements;

    return ModuleRuntimeRequirements{
        .io = {
            ModuleIORequirement{
                .requirementID = "forge.network.client",
                .kind = ModuleIOKind::Networking,
                .access = ModuleIOAccess::ReadWrite,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.settings.storage",
                .kind = ModuleIOKind::Storage,
                .access = ModuleIOAccess::ReadWrite,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.secrets.secure-storage",
                .kind = ModuleIOKind::SecureStorage,
                .access = ModuleIOAccess::ReadWrite,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.data.export",
                .kind = ModuleIOKind::FileExport,
                .access = ModuleIOAccess::Write,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.telemetry.events",
                .kind = ModuleIOKind::Telemetry,
                .access = ModuleIOAccess::Emit,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.database.access",
                .kind = ModuleIOKind::SharedDatabase,
                .access = ModuleIOAccess::ReadWrite,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.diagnostics.events",
                .kind = ModuleIOKind::Diagnostics,
                .access = ModuleIOAccess::Emit,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.api.invoke",
                .kind = ModuleIOKind::API,
                .access = ModuleIOAccess::Execute,
                .required = true,
                .description = std::nullopt},
            ModuleIORequirement{
                .requirementID = "forge.security.authorization",
                .kind = ModuleIOKind::Security,
                .access = ModuleIOAccess::Execute,
                .required = true,
                .description = std::nullopt}},
        .ui = ModuleUIRequirements{
            .controlSchemeID = std::string{"forge-conductor.windows.controls.v1"},
            .layoutID = std::string{"forge-conductor.windows.shell.v1"},
            .themeIDs = {},
            .viewIDs = {
                "ForgeConductorShellView",
                "ForgeConductorSettingsOverlayView",
                "ForgeConductorResetConfirmationView",
                "ForgeConductorPurgeConfirmationView"},
            .slotIDs = {"appShell"},
            .toolbarItemIDs = {"forge-conductor.toolbar.settings"},
            .routeIDs = {
                "forge-conductor.route.settings",
                "forge-conductor.route.reset-project",
                "forge-conductor.route.purge-all"},
            .pointerIDs = {}},
        .dataIsolation = ModuleDataIsolation{
            .mode = ModuleDataIsolationMode::PrivateToModule,
            .ownedStoreIDs = {
                "forge-conductor.central",
                "forge-conductor.project",
                "forge-conductor.configuration",
                "forge-conductor.manager-state",
                "forge-conductor.session-ledger"},
            .requiredDefaultRoles = {}}};
}

} // namespace

ForgeConductorAppModule::ForgeConductorAppModule(
    std::shared_ptr<Contracts::IForgeApplicationLifecycle> lifecycle)
    : lifecycle_(std::move(lifecycle))
{
    if (!lifecycle_) {
        throw std::invalid_argument(
            "ForgeConductorAppModule requires an application lifecycle.");
    }
}

Forsetti::ModuleDescriptor ForgeConductorAppModule::descriptor() const
{
    return Forsetti::ModuleDescriptor{
        .moduleID = ModuleID,
        .displayName = DisplayName,
        .version = Forsetti::SemVer{0, 9, 0},
        .type = Forsetti::ModuleType::App};
}

Forsetti::ModuleManifest ForgeConductorAppModule::manifest() const
{
    return Forsetti::ModuleManifest{
        .schemaVersion = "1.1",
        .moduleID = ModuleID,
        .displayName = DisplayName,
        .moduleVersion = Forsetti::SemVer{0, 9, 0},
        .moduleType = Forsetti::ModuleType::App,
        .supportedPlatforms = {Forsetti::Platform::Windows},
        .minForsettiVersion = Forsetti::SemVer{0, 2, 0},
        .maxForsettiVersion = Forsetti::SemVer{0, 2, 0},
        .capabilitiesRequested = {
            Forsetti::Capability::Networking,
            Forsetti::Capability::Storage,
            Forsetti::Capability::SecureStorage,
            Forsetti::Capability::FileExport,
            Forsetti::Capability::Telemetry,
            Forsetti::Capability::RoutingOverlay,
            Forsetti::Capability::ToolbarItems,
            Forsetti::Capability::ViewInjection,
            Forsetti::Capability::EventPublishing,
            Forsetti::Capability::SharedDatabase,
            Forsetti::Capability::Diagnostics,
            Forsetti::Capability::API,
            Forsetti::Capability::Security},
        .iapProductID = std::nullopt,
        .entryPoint = EntryPoint,
        .manifestTemplateVersion = Forsetti::ManifestTemplateVersion::V1_1,
        .defaultModuleRole = Forsetti::DefaultModuleRole::UI,
        .runtimeRequirements = makeRuntimeRequirements()};
}

Forsetti::UIContributions ForgeConductorAppModule::uiContributions() const
{
    using Forsetti::ModuleOverlayDestination;
    using Forsetti::OpenOverlayAction;
    using Forsetti::OverlayDestination;
    using Forsetti::OverlayPresentation;
    using Forsetti::OverlayRoute;
    using Forsetti::OverlaySchema;
    using Forsetti::ToolbarAction;
    using Forsetti::ToolbarItemDescriptor;
    using Forsetti::UIContributions;
    using Forsetti::ViewInjectionDescriptor;

    return UIContributions{
        .themeMask = std::nullopt,
        .toolbarItems = {
            ToolbarItemDescriptor{
                .itemID = "forge-conductor.toolbar.settings",
                .title = "Settings",
                .systemImageName = "Settings",
                .action = ToolbarAction{OpenOverlayAction{
                    .routeID = "forge-conductor.route.settings"}}}},
        .viewInjections = {
            ViewInjectionDescriptor{
                .injectionID = "forge-conductor.view-injection.shell",
                .slot = "appShell",
                .viewID = "ForgeConductorShellView",
                .priority = 0}},
        .overlaySchema = OverlaySchema{
            .navigationPointers = {},
            .overlayRoutes = {
                OverlayRoute{
                    .routeID = "forge-conductor.route.settings",
                    .label = "Settings",
                    .presentation = OverlayPresentation::Sheet,
                    .destination = OverlayDestination{ModuleOverlayDestination{
                        .moduleID = ModuleID,
                        .viewID = "ForgeConductorSettingsOverlayView"}}},
                OverlayRoute{
                    .routeID = "forge-conductor.route.reset-project",
                    .label = "Reset Project",
                    .presentation = OverlayPresentation::Sheet,
                    .destination = OverlayDestination{ModuleOverlayDestination{
                        .moduleID = ModuleID,
                        .viewID = "ForgeConductorResetConfirmationView"}}},
                OverlayRoute{
                    .routeID = "forge-conductor.route.purge-all",
                    .label = "Purge All Data",
                    .presentation = OverlayPresentation::Sheet,
                    .destination = OverlayDestination{ModuleOverlayDestination{
                        .moduleID = ModuleID,
                        .viewID = "ForgeConductorPurgeConfirmationView"}}}}}};
}

void ForgeConductorAppModule::start(Forsetti::IForsettiModuleContext& context)
{
    {
        std::lock_guard lock(lifecycleMutex_);
        if (lifecycleState_ != LifecycleState::Stopped) {
            return;
        }
        lifecycleState_ = LifecycleState::Starting;
    }

    try {
        const auto result = lifecycle_->start();
        if (!result) {
            {
                std::lock_guard lock(lifecycleMutex_);
                lifecycleState_ = LifecycleState::Stopped;
            }
            const auto detail = result.error().code + ": " + result.error().message;
            recordLifecycleFailure(context, "start", detail.c_str());
            return;
        }
        std::lock_guard lock(lifecycleMutex_);
        lifecycleState_ = LifecycleState::Started;
    } catch (const std::exception& ex) {
        {
            std::lock_guard lock(lifecycleMutex_);
            lifecycleState_ = LifecycleState::Stopped;
        }
        recordLifecycleFailure(context, "start", ex.what());
    } catch (...) {
        {
            std::lock_guard lock(lifecycleMutex_);
            lifecycleState_ = LifecycleState::Stopped;
        }
        recordLifecycleFailure(context, "start", "non-standard exception");
    }
}

void ForgeConductorAppModule::stop(Forsetti::IForsettiModuleContext& context)
{
    {
        std::lock_guard lock(lifecycleMutex_);
        if (lifecycleState_ != LifecycleState::Started) {
            return;
        }
        lifecycleState_ = LifecycleState::Stopping;
    }

    try {
        const auto result = lifecycle_->stop();
        if (!result) {
            const auto detail = result.error().code + ": " + result.error().message;
            recordLifecycleFailure(context, "stop", detail.c_str());
        }
    } catch (const std::exception& ex) {
        recordLifecycleFailure(context, "stop", ex.what());
    } catch (...) {
        recordLifecycleFailure(context, "stop", "non-standard exception");
    }

    std::lock_guard lock(lifecycleMutex_);
    lifecycleState_ = LifecycleState::Stopped;
}

void ForgeConductorAppModule::recordLifecycleFailure(
    Forsetti::IForsettiModuleContext& context,
    const char* operation,
    const char* detail) const noexcept
{
    try {
        const auto logger = context.logger();
        if (logger) {
            logger->log(
                Forsetti::LogLevel::Error,
                std::string{"Forge Conductor application lifecycle "} + operation +
                    " failed: " + detail,
                ModuleID);
        }
    } catch (...) {
        // Logging is best-effort at the framework boundary.
    }
}

void registerForgeConductorAppModule(
    Forsetti::ModuleRegistry& registry,
    std::shared_ptr<Contracts::IForgeApplicationLifecycle> lifecycle)
{
    if (!lifecycle) {
        throw std::invalid_argument(
            "ForgeConductorAppModule registration requires an application lifecycle.");
    }
    if (registry.hasEntryPoint(ForgeConductorAppModule::EntryPoint)) {
        throw Forsetti::ModuleRegistryException(
            Forsetti::ModuleRegistryError::EntryPointAlreadyRegistered,
            "ForgeConductorAppModule entry point is already registered.");
    }

    registry.registerModule(
        ForgeConductorAppModule::EntryPoint,
        [lifecycle = std::move(lifecycle)]()
            -> std::unique_ptr<Forsetti::IForsettiModule> {
            return std::make_unique<ForgeConductorAppModule>(lifecycle);
        });
}

} // namespace ForgeConductor::ForsettiModule
