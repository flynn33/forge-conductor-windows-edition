#include "ForgeConductor/ForsettiModule/ForgeConductorAppModule.h"

#include <ForsettiCore/ActivationStore.h>
#include <ForsettiCore/CapabilityPolicy.h>
#include <ForsettiCore/EntitlementProviders.h>
#include <ForsettiCore/ForsettiContext.h>
#include <ForsettiCore/ForsettiEventBus.h>
#include <ForsettiCore/ForsettiLogger.h>
#include <ForsettiCore/ForsettiServiceContainer.h>
#include <ForsettiCore/ForsettiServices.h>
#include <ForsettiCore/ManifestLoader.h>
#include <ForsettiCore/ModuleRegistration.h>
#include <ForsettiCore/ModuleRequirementValidator.h>
#include <ForsettiCore/UISurfaceManager.h>
#include <ForsettiHostTemplate/ForsettiHostBootstrap.h>
#include <ForsettiHostTemplate/ForsettiHostOverlayRouter.h>

#include <algorithm>
#include <any>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using ForgeConductor::Contracts::IForgeApplicationLifecycle;
using ForgeConductor::Domain::ErrorCodes::InternalFailure;
using ForgeConductor::Domain::Result;
using ForgeConductor::Domain::makeError;
using ForgeConductor::ForsettiModule::ForgeConductorAppModule;
using ForgeConductor::ForsettiModule::registerForgeConductorAppModule;
using namespace Forsetti;

class SmokeFailure final : public std::runtime_error {
public:
    explicit SmokeFailure(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw SmokeFailure(message);
    }
}

template <typename T>
std::future<T> readyFuture(T value)
{
    std::promise<T> promise;
    promise.set_value(std::move(value));
    return promise.get_future();
}

class TrackingLifecycle final : public IForgeApplicationLifecycle {
public:
    [[nodiscard]] Result<void> start() noexcept override
    {
        ++startCalls;
        if (failOnStart) {
            return Result<void>::failure(
                makeError(InternalFailure, "deterministic start failure"));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> stop() noexcept override
    {
        ++stopCalls;
        if (failOnStop) {
            return Result<void>::failure(
                makeError(InternalFailure, "deterministic stop failure"));
        }
        return Result<void>::success();
    }

    int startCalls = 0;
    int stopCalls = 0;
    bool failOnStart = false;
    bool failOnStop = false;
};

class MemoryActivationStore final : public IActivationStore {
public:
    ActivationState loadState() const override
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

    void saveState(const ActivationState& state) override
    {
        std::lock_guard lock(mutex_);
        state_ = state;
    }

private:
    mutable std::mutex mutex_;
    ActivationState state_;
};

class RecordingLogger final : public IForsettiLogger {
public:
    void log(
        LogLevel level,
        const std::string& message,
        const std::string& sourceModuleID = "",
        const std::map<std::string, std::string>& metadata = {}) override
    {
        std::lock_guard lock(mutex_);
        entries_.push_back(Entry{level, message, sourceModuleID, metadata});
    }

    [[nodiscard]] std::size_t entryCount() const
    {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

private:
    struct Entry final {
        LogLevel level;
        std::string message;
        std::string sourceModuleID;
        std::map<std::string, std::string> metadata;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

class DeterministicNetworkingService final : public INetworkingService {
public:
    std::future<std::vector<uint8_t>> data(
        const std::string&,
        const std::map<std::string, std::string>&) override
    {
        return readyFuture(std::vector<uint8_t>{});
    }
};

class DeterministicStorageService final : public IStorageService {
public:
    void set(const std::string& key, const std::string& value) override
    {
        values_[key] = value;
    }

    std::optional<std::string> get(const std::string& key) override
    {
        const auto found = values_.find(key);
        if (found == values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    void remove(const std::string& key) override
    {
        values_.erase(key);
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

class DeterministicSecureStorageService final : public ISecureStorageService {
public:
    void set(const std::string& key, const std::vector<uint8_t>& data) override
    {
        values_[key] = data;
    }

    std::optional<std::vector<uint8_t>> get(const std::string& key) override
    {
        const auto found = values_.find(key);
        if (found == values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    void remove(const std::string& key) override
    {
        values_.erase(key);
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> values_;
};

class DeterministicFileExportService final : public IFileExportService {
public:
    bool exportData(
        const std::vector<uint8_t>&,
        const std::string& filename) override
    {
        return !filename.empty();
    }
};

class DeterministicTelemetryService final : public ITelemetryService {
public:
    void trackEvent(
        const std::string& name,
        const std::map<std::string, std::string>&) override
    {
        if (!name.empty()) {
            ++eventCount_;
        }
    }

private:
    std::size_t eventCount_ = 0;
};

class DeterministicSharedDatabaseService final : public ISharedDatabaseService {
public:
    void execute(
        const std::string& operation,
        const std::map<std::string, std::string>&) override
    {
        lastOperation_ = operation;
    }

private:
    std::string lastOperation_;
};

class DeterministicDiagnosticsService final : public IDiagnosticsService {
public:
    void recordDiagnostic(
        const std::string& name,
        const std::map<std::string, std::string>&) override
    {
        lastDiagnostic_ = name;
    }

private:
    std::string lastDiagnostic_;
};

class DeterministicApiService final : public IApiService {
public:
    std::future<std::vector<uint8_t>> invoke(
        const std::string&,
        const std::vector<uint8_t>&,
        const std::map<std::string, std::string>&) override
    {
        return readyFuture(std::vector<uint8_t>{});
    }
};

class DeterministicSecurityService final : public ISecurityService {
public:
    bool authorize(
        const std::string& operation,
        const std::map<std::string, std::string>&) override
    {
        return operation == "forge.smoke.authorized";
    }
};

std::set<Capability> requestedCapabilities()
{
    return {
        Capability::Networking,
        Capability::Storage,
        Capability::SecureStorage,
        Capability::FileExport,
        Capability::Telemetry,
        Capability::RoutingOverlay,
        Capability::ToolbarItems,
        Capability::ViewInjection,
        Capability::EventPublishing,
        Capability::SharedDatabase,
        Capability::Diagnostics,
        Capability::API,
        Capability::Security};
}

struct HostDependencies final {
    HostDependencies()
        : activationStore(std::make_shared<MemoryActivationStore>()),
          entitlementProvider(std::make_shared<StaticEntitlementProvider>()),
          capabilityPolicy(std::make_shared<FixedCapabilityPolicy>(requestedCapabilities())),
          services(std::make_shared<ServiceContainer>()),
          eventBus(std::make_shared<InMemoryEventBus>()),
          logger(std::make_shared<RecordingLogger>()),
          overlayRouter(std::make_shared<ForsettiHostOverlayRouter>()),
          communicationGuard(std::make_shared<DefaultModuleCommunicationGuard>()),
          surfaceManager(std::make_shared<UISurfaceManager>()),
          registrationService(ModuleRegistrationService::makeInMemory()),
          requirementValidator(std::make_shared<ModuleRequirementValidator>())
    {
        entitlementProvider->setUnlockedModules({ForgeConductorAppModule::ModuleID});

        services->registerService<INetworkingService>(
            std::make_shared<DeterministicNetworkingService>());
        services->registerService<IStorageService>(
            std::make_shared<DeterministicStorageService>());
        services->registerService<ISecureStorageService>(
            std::make_shared<DeterministicSecureStorageService>());
        services->registerService<IFileExportService>(
            std::make_shared<DeterministicFileExportService>());
        services->registerService<ITelemetryService>(
            std::make_shared<DeterministicTelemetryService>());
        services->registerService<ISharedDatabaseService>(
            std::make_shared<DeterministicSharedDatabaseService>());
        services->registerService<IDiagnosticsService>(
            std::make_shared<DeterministicDiagnosticsService>());
        services->registerService<IApiService>(
            std::make_shared<DeterministicApiService>());
        services->registerService<ISecurityService>(
            std::make_shared<DeterministicSecurityService>());
    }

    std::shared_ptr<MemoryActivationStore> activationStore;
    std::shared_ptr<StaticEntitlementProvider> entitlementProvider;
    std::shared_ptr<FixedCapabilityPolicy> capabilityPolicy;
    std::shared_ptr<ServiceContainer> services;
    std::shared_ptr<InMemoryEventBus> eventBus;
    std::shared_ptr<RecordingLogger> logger;
    std::shared_ptr<ForsettiHostOverlayRouter> overlayRouter;
    std::shared_ptr<DefaultModuleCommunicationGuard> communicationGuard;
    std::shared_ptr<UISurfaceManager> surfaceManager;
    std::shared_ptr<ModuleRegistrationService> registrationService;
    std::shared_ptr<ModuleRequirementValidator> requirementValidator;
};

ModuleRegistry makeRegistry(
    const std::shared_ptr<IForgeApplicationLifecycle>& lifecycle)
{
    ModuleRegistry registry;
    registerForgeConductorAppModule(registry, lifecycle);
    return registry;
}

std::shared_ptr<IForsettiHostController> makeController(
    const std::string& manifestDirectory,
    const std::shared_ptr<IForgeApplicationLifecycle>& lifecycle,
    const HostDependencies& dependencies)
{
    ForsettiHostBootstrap bootstrap;
    return bootstrap.makeController(ForsettiHostBootstrapConfiguration{
        .manifestDirectory = manifestDirectory,
        .moduleRegistry = makeRegistry(lifecycle),
        .activationStore = dependencies.activationStore,
        .entitlementProvider = dependencies.entitlementProvider,
        .capabilityPolicy = dependencies.capabilityPolicy,
        .services = dependencies.services,
        .eventBus = dependencies.eventBus,
        .logger = dependencies.logger,
        .overlayRouter = dependencies.overlayRouter,
        .communicationGuard = dependencies.communicationGuard,
        .surfaceManager = dependencies.surfaceManager,
        .registrationService = dependencies.registrationService,
        .requirementValidator = dependencies.requirementValidator});
}

void verifyUIContributions(const UIContributions& contributions)
{
    require(!contributions.themeMask.has_value(), "theme mask must not be contributed");
    require(contributions.toolbarItems.size() == 1, "expected one toolbar item");
    require(
        contributions.toolbarItems.front().itemID == "forge-conductor.toolbar.settings",
        "settings toolbar item ID mismatch");
    require(contributions.viewInjections.size() == 1, "expected one view injection");
    require(
        contributions.viewInjections.front().slot == "appShell" &&
            contributions.viewInjections.front().viewID == "ForgeConductorShellView",
        "shell view injection mismatch");
    require(contributions.overlaySchema.has_value(), "overlay schema is required");
    require(
        contributions.overlaySchema->navigationPointers.empty(),
        "navigation pointers must be empty");
    require(
        contributions.overlaySchema->overlayRoutes.size() == 3,
        "expected three overlay routes");
}

void runModuleAndManifestSmoke(const std::string& manifestDirectory)
{
    auto lifecycle = std::make_shared<TrackingLifecycle>();
    auto registry = makeRegistry(lifecycle);

    require(
        registry.hasEntryPoint(ForgeConductorAppModule::EntryPoint),
        "module entry point was not registered");

    auto module = registry.makeModule(ForgeConductorAppModule::EntryPoint);
    require(module != nullptr, "module factory returned null");
    require(
        dynamic_cast<IForsettiAppModule*>(module.get()) != nullptr,
        "module does not implement IForsettiAppModule");

    const auto expectedDescriptor = ModuleDescriptor{
        .moduleID = ForgeConductorAppModule::ModuleID,
        .displayName = "Forge Conductor",
        .version = SemVer{0, 9, 0},
        .type = ModuleType::App};
    require(module->descriptor() == expectedDescriptor, "module descriptor mismatch");

    const auto discovered = ManifestLoader::loadManifests(manifestDirectory);
    require(discovered.size() == 1, "runtime directory must contain one app manifest");
    require(
        discovered.front().moduleID == ForgeConductorAppModule::ModuleID,
        "discovered module ID mismatch");
    require(
        module->manifest() == discovered.front(),
        "bundled C++ manifest does not equal the canonical JSON manifest");

    const auto* uiModule = dynamic_cast<IForsettiUIModule*>(module.get());
    require(uiModule != nullptr, "app module does not implement the UI contract");
    verifyUIContributions(uiModule->uiContributions());

    HostDependencies dependencies;
    ForsettiContext context(
        dependencies.services,
        dependencies.eventBus,
        dependencies.logger,
        dependencies.overlayRouter,
        dependencies.communicationGuard);

    module->start(context);
    module->start(context);
    require(lifecycle->startCalls == 1, "module start must be idempotent");
    module->stop(context);
    module->stop(context);
    require(lifecycle->stopCalls == 1, "module stop must be idempotent");

    auto failingLifecycle = std::make_shared<TrackingLifecycle>();
    failingLifecycle->failOnStart = true;
    ForgeConductorAppModule failingModule(failingLifecycle);
    failingModule.start(context);
    require(
        failingLifecycle->startCalls == 1,
        "failing lifecycle start was not invoked exactly once");
    require(
        dependencies.logger->entryCount() >= 1,
        "typed lifecycle failure was not recorded");

    std::cout << "PASS module registration, descriptor, manifest, UI, and lifecycle boundary\n";
}

void runDiscoveryOnlyHostSmoke(const std::string& manifestDirectory)
{
    auto lifecycle = std::make_shared<TrackingLifecycle>();
    HostDependencies dependencies;
    auto controller = makeController(manifestDirectory, lifecycle, dependencies);

    controller->boot(ForsettiHostLaunchStrategy::restoreOnly());
    const auto ready = controller->snapshot();
    require(
        ready.state == ForsettiHostStateKind::ReadyWithoutActiveUI,
        "restore-only boot did not reach ReadyWithoutActiveUI");
    require(
        ready.availableModuleIDs ==
            std::vector<std::string>{ForgeConductorAppModule::ModuleID},
        "restore-only discovery did not expose exactly the Forge app module");
    require(ready.activeModuleIDs.empty(), "restore-only boot activated a module");
    require(!ready.activeUIModuleID.has_value(), "restore-only boot selected a UI module");
    require(!ready.lastError.has_value(), "restore-only boot recorded an error");
    require(lifecycle->startCalls == 0, "restore-only boot invoked the module lifecycle");

    controller->shutdown();
    require(
        controller->snapshot().state == ForsettiHostStateKind::NotStarted,
        "host did not return to NotStarted during shutdown");
    controller.reset();

    std::cout << "PASS public HostTemplate discovery-only boot and clean shutdown\n";
}

void runKnownActivationRegression(const std::string& manifestDirectory)
{
    auto lifecycle = std::make_shared<TrackingLifecycle>();
    HostDependencies dependencies;
    auto controller = makeController(manifestDirectory, lifecycle, dependencies);

    bool caughtBadAnyCast = false;
    try {
        controller->boot(ForsettiHostLaunchStrategy::explicitModuleIDs(
            {ForgeConductorAppModule::ModuleID}));
    } catch (const std::bad_any_cast&) {
        caughtBadAnyCast = true;
    }

    require(
        caughtBadAnyCast,
        "sealed Forsetti 0.2.0 activation regression no longer produced std::bad_any_cast");
    const auto failed = controller->snapshot();
    require(
        failed.state == ForsettiHostStateKind::FatalError,
        "known activation regression was not recorded as FatalError");
    require(failed.lastError.has_value(), "known activation regression omitted its error");
    require(
        lifecycle->startCalls == 0,
        "activation regression unexpectedly reached the module lifecycle");

    controller->shutdown();
    controller.reset();

    std::cout
        << "KNOWN_UPSTREAM_REGRESSION Forsetti 0.2.0 explicit activation is blocked by "
           "CapabilityScopedServiceProvider storage std::bad_any_cast\n";
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: ForgeConductor.ForsettiHostSmoke <manifest-directory>\n";
        return 2;
    }

    try {
        const std::string manifestDirectory = argv[1];
        runModuleAndManifestSmoke(manifestDirectory);
        runDiscoveryOnlyHostSmoke(manifestDirectory);
        runKnownActivationRegression(manifestDirectory);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL " << ex.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "FAIL non-standard exception\n";
        return 1;
    }
}
