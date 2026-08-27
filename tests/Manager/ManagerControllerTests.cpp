#include "../Infrastructure/TestSupport.h"

#include "ForgeConductor/Application/ManagerController.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using namespace std::chrono_literals;

static_assert(std::is_abstract_v<Contracts::IManagerRuntime>);
static_assert(std::is_abstract_v<Contracts::IManagerController>);
static_assert(std::has_virtual_destructor_v<Contracts::IManagerRuntime>);
static_assert(std::has_virtual_destructor_v<Contracts::IManagerController>);
static_assert(std::is_final_v<Application::ManagerController>);

[[nodiscard]] Domain::Error failure(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::string{message});
}

class ManualClock final : public Contracts::IClock {
public:
    ManualClock(
        const Domain::UtcTimePoint utc,
        const Domain::MonotonicTimePoint monotonic) noexcept
        : utc_{utc}, monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return monotonic_;
    }

    void advance(const std::chrono::seconds amount) noexcept
    {
        std::lock_guard lock{mutex_};
        utc_ += amount;
        monotonic_ += amount;
    }

private:
    mutable std::mutex mutex_;
    Domain::UtcTimePoint utc_;
    Domain::MonotonicTimePoint monotonic_;
};

class RecordingConfigurationStore final
    : public Contracts::IConfigurationStore {
public:
    explicit RecordingConfigurationStore(Domain::AppConfig config)
        : config_{std::move(config)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig> load(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++loadCalls_;
            if (loadFailure_) {
                return Domain::Result<Domain::AppConfig>::failure(*loadFailure_);
            }
            return Domain::Result<Domain::AppConfig>::success(config_);
        } catch (...) {
            return Domain::Result<Domain::AppConfig>::failure(
                failure("The recording configuration load failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig> update(
        const Domain::AppConfigPatch& patch,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++updateCalls_;
            lastPatch_ = patch;
            updateEntered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this]() noexcept { return !blockUpdate_; });
            if (updateFailure_) {
                return Domain::Result<Domain::AppConfig>::failure(*updateFailure_);
            }
            auto applied = Domain::applyConfigPatch(config_, patch);
            if (!applied) {
                return Domain::Result<Domain::AppConfig>::failure(
                    std::move(applied).error());
            }
            config_ = returnedOverride_.value_or(std::move(applied).value());
            return Domain::Result<Domain::AppConfig>::success(config_);
        } catch (...) {
            return Domain::Result<Domain::AppConfig>::failure(
                failure("The recording configuration update failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig> reload(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++reloadCalls_;
            return Domain::Result<Domain::AppConfig>::success(config_);
        } catch (...) {
            return Domain::Result<Domain::AppConfig>::failure(
                failure("The recording configuration reload failed."));
        }
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        ++shutdownCalls_;
        if (shutdownOrder_ != nullptr) {
            shutdownOrder_->push_back("store");
        }
    }

    void failLoad(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        loadFailure_ = std::move(error);
    }

    void failUpdate(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        updateFailure_ = std::move(error);
    }

    void clearUpdateFailure()
    {
        std::lock_guard lock{mutex_};
        updateFailure_.reset();
    }

    void returnConfig(Domain::AppConfig config)
    {
        std::lock_guard lock{mutex_};
        returnedOverride_ = std::move(config);
    }

    void clearReturnedConfig()
    {
        std::lock_guard lock{mutex_};
        returnedOverride_.reset();
    }

    void blockUpdate()
    {
        std::lock_guard lock{mutex_};
        blockUpdate_ = true;
        updateEntered_ = false;
    }

    [[nodiscard]] bool waitForBlockedUpdate()
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, 2s, [this]() noexcept { return updateEntered_; });
    }

    void releaseUpdate()
    {
        std::lock_guard lock{mutex_};
        blockUpdate_ = false;
        condition_.notify_all();
    }

    void recordShutdownIn(std::vector<std::string>& order) noexcept
    {
        std::lock_guard lock{mutex_};
        shutdownOrder_ = &order;
    }

    [[nodiscard]] std::size_t loadCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return loadCalls_;
    }

    [[nodiscard]] std::size_t updateCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return updateCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdownCalls_;
    }

    [[nodiscard]] std::optional<Domain::AppConfigPatch> lastPatch() const
    {
        std::lock_guard lock{mutex_};
        return lastPatch_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Domain::AppConfig config_;
    std::optional<Domain::Error> loadFailure_;
    std::optional<Domain::Error> updateFailure_;
    std::optional<Domain::AppConfig> returnedOverride_;
    std::optional<Domain::AppConfigPatch> lastPatch_;
    std::vector<std::string>* shutdownOrder_{};
    std::size_t loadCalls_{};
    std::size_t updateCalls_{};
    std::size_t reloadCalls_{};
    std::size_t shutdownCalls_{};
    bool blockUpdate_{};
    bool updateEntered_{};
};

class RecordingManagerRuntime final : public Contracts::IManagerRuntime {
public:
    explicit RecordingManagerRuntime(Domain::UtcTimePoint startedAt)
        : startedAt_{startedAt}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> start(
        const Domain::AppConfig& config,
        const Domain::OperationContext&) noexcept override
    {
        return invoke(RuntimeCall::Start, config, true);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> pause(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++pauseCalls_;
            if (pauseFailure_) {
                current_.lastError = pauseFailure_->message;
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *pauseFailure_);
            }
            current_.listenerListening = true;
            current_.operationalServiceActive = false;
            current_.lastError.reset();
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime pause failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> rebind(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++rebindCalls_;
            ++current_.restartCount;
            lastConfig_ = config;
            lastDesired_ = operationalServiceDesired;
            if (rebindFailure_) {
                current_.listenerListening = false;
                current_.operationalServiceActive = false;
                current_.lastError = rebindFailure_->message;
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *rebindFailure_);
            }
            current_.listenerListening = true;
            current_.operationalServiceActive = operationalServiceDesired;
            current_.startedAt = startedAt_;
            current_.lastError.reset();
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime rebind failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> reconcile(
        const Domain::AppConfig& config,
        const bool operationalServiceDesired,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++reconcileCalls_;
            lastConfig_ = config;
            lastDesired_ = operationalServiceDesired;
            if (reconcileFailure_) {
                current_.lastError = reconcileFailure_->message;
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *reconcileFailure_);
            }
            current_.listenerListening = true;
            current_.operationalServiceActive = operationalServiceDesired;
            current_.lastError.reset();
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime reconcile failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> applySettings(
        const Domain::AppConfig& config,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++applyCalls_;
            lastConfig_ = config;
            if (applyFailure_) {
                current_.lastError = applyFailure_->message;
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *applyFailure_);
            }
            current_.lastError.reset();
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime settings apply failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++snapshotCalls_;
            if (snapshotFailure_) {
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *snapshotFailure_);
            }
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime snapshot failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> requestShutdown(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++requestShutdownCalls_;
            if (requestShutdownFailure_) {
                current_.lastError = requestShutdownFailure_->message;
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *requestShutdownFailure_);
            }
            current_.shutdownRequested = true;
            current_.lastError.reset();
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime shutdown request failed."));
        }
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        ++shutdownCalls_;
        if (shutdownOrder_ != nullptr) {
            shutdownOrder_->push_back("runtime");
        }
    }

    void onStart(std::function<void()> callback)
    {
        std::lock_guard lock{mutex_};
        startCallback_ = std::move(callback);
    }

    void failStart(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        startFailure_ = std::move(error);
    }

    void failRebind(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        rebindFailure_ = std::move(error);
    }

    void clearRebindFailure()
    {
        std::lock_guard lock{mutex_};
        rebindFailure_.reset();
    }

    void failReconcile(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        reconcileFailure_ = std::move(error);
    }

    void failApply(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        applyFailure_ = std::move(error);
    }

    void recordShutdownIn(std::vector<std::string>& order) noexcept
    {
        std::lock_guard lock{mutex_};
        shutdownOrder_ = &order;
    }

    [[nodiscard]] std::size_t startCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return startCalls_;
    }

    [[nodiscard]] std::size_t pauseCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return pauseCalls_;
    }

    [[nodiscard]] std::size_t rebindCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return rebindCalls_;
    }

    [[nodiscard]] std::size_t reconcileCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return reconcileCalls_;
    }

    [[nodiscard]] std::size_t applyCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return applyCalls_;
    }

    [[nodiscard]] std::size_t snapshotCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return snapshotCalls_;
    }

    [[nodiscard]] std::size_t requestShutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return requestShutdownCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdownCalls_;
    }

private:
    enum class RuntimeCall { Start };

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> invoke(
        const RuntimeCall,
        const Domain::AppConfig& config,
        const bool operationalServiceDesired) noexcept
    {
        try {
            std::function<void()> callback;
            {
                std::lock_guard lock{mutex_};
                ++startCalls_;
                lastConfig_ = config;
                lastDesired_ = operationalServiceDesired;
                callback = startCallback_;
            }
            if (callback) {
                callback();
            }
            std::lock_guard lock{mutex_};
            if (startFailure_) {
                current_.lastError = startFailure_->message;
                return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                    *startFailure_);
            }
            current_.listenerListening = true;
            current_.operationalServiceActive = true;
            current_.startedAt = startedAt_;
            current_.shutdownRequested = false;
            current_.lastError.reset();
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::success(current_);
        } catch (...) {
            return Domain::Result<Domain::ManagerRuntimeSnapshot>::failure(
                failure("The recording runtime start failed."));
        }
    }

    mutable std::mutex mutex_;
    Domain::ManagerRuntimeSnapshot current_;
    Domain::UtcTimePoint startedAt_;
    Domain::AppConfig lastConfig_{Domain::defaultAppConfig()};
    std::function<void()> startCallback_;
    std::optional<Domain::Error> startFailure_;
    std::optional<Domain::Error> pauseFailure_;
    std::optional<Domain::Error> rebindFailure_;
    std::optional<Domain::Error> reconcileFailure_;
    std::optional<Domain::Error> applyFailure_;
    std::optional<Domain::Error> snapshotFailure_;
    std::optional<Domain::Error> requestShutdownFailure_;
    std::vector<std::string>* shutdownOrder_{};
    std::size_t startCalls_{};
    std::size_t pauseCalls_{};
    std::size_t rebindCalls_{};
    std::size_t reconcileCalls_{};
    std::size_t applyCalls_{};
    mutable std::size_t snapshotCalls_{};
    std::size_t requestShutdownCalls_{};
    std::size_t shutdownCalls_{};
    bool lastDesired_{};
};

struct Fixture final {
    static constexpr auto UtcNow = Domain::UtcTimePoint{1'000s};
    static constexpr auto MonotonicNow = Domain::MonotonicTimePoint{1'000s};

    Fixture()
        : store{std::make_shared<RecordingConfigurationStore>(config)},
          runtime{std::make_shared<RecordingManagerRuntime>(UtcNow - 20s)},
          clock{std::make_shared<ManualClock>(UtcNow, MonotonicNow)},
          controller{
              store,
              runtime,
              clock,
              Domain::ManagerControllerOptions{
                  take(Domain::PathText::create("C:\\ForgeFixture")),
                  "0.9.0-test",
                  4242U}}
    {
    }

    [[nodiscard]] Domain::OperationContext context(
        const std::stop_token cancellation = {},
        const std::chrono::seconds lifetime = 5min) const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>(
                "a6000000-0000-4000-8000-000000000001"),
            clock->monotonicNow() + lifetime,
            cancellation,
            parse<Domain::CorrelationId>("p16-manager-controller")};
    }

    Domain::AppConfig config{Domain::defaultAppConfig()};
    std::shared_ptr<RecordingConfigurationStore> store;
    std::shared_ptr<RecordingManagerRuntime> runtime;
    std::shared_ptr<ManualClock> clock;
    Application::ManagerController controller;
};

void lifecycleAndSourceCompatibleStopAreDeterministic()
{
    Fixture fixture;
    fixture.config.dashboard.host = "::1";
    fixture.config.dashboard.port = 8123U;
    fixture.config.dashboard.refreshInterval = 11s;
    fixture.config.manager.autoRestart = false;
    fixture.config.manager.watchdogInterval = 7s;
    fixture.config.manager.openBrowserOnStart = true;
    fixture.config.sessions.idleTimeToLive = 500s;
    fixture.config.shell.defaultTimeout = 45s;
    fixture.config.logLevel = Domain::LogLevel::Warning;
    fixture.store = std::make_shared<RecordingConfigurationStore>(fixture.config);

    // Reconstruct because injected dependencies are immutable by design.
    Application::ManagerController controller{
        fixture.store,
        fixture.runtime,
        fixture.clock,
        Domain::ManagerControllerOptions{
            take(Domain::PathText::create("C:\\ForgeFixture")),
            "0.9.0-test",
            4242U}};

    const auto running = take(controller.initialize(fixture.context()));
    require(running.ok && running.isManager &&
                running.state == Domain::ManagerServiceState::Running &&
                running.desiredRunning && running.httpListening &&
                running.serviceActive,
            "Manager initialize did not publish the running listener snapshot.");
    require(running.processId == 4242U &&
                running.home.value() == "C:\\ForgeFixture" &&
                running.version == "0.9.0-test" &&
                running.uptime == 20s,
            "Manager status omitted immutable identity or measured uptime.");
    require(running.dashboardHost == "::1" && running.dashboardPort == 8123U &&
                running.dashboardRefreshInterval == 11s &&
                !running.autoRestart && running.watchdogInterval == 7s &&
                running.openBrowserOnStart,
            "Manager status did not map the complete application configuration.");

    const auto managerSettings = take(controller.settings(fixture.context()));
    require(managerSettings.dashboardHost == "::1" &&
                managerSettings.dashboardPort == 8123U &&
                managerSettings.dashboardRefreshInterval == 11s &&
                !managerSettings.autoRestart &&
                managerSettings.watchdogInterval == 7s &&
                managerSettings.openBrowserOnStart &&
                managerSettings.sessionIdleTtl == 500s &&
                managerSettings.shellTimeout == 45s &&
                managerSettings.logLevel == Domain::LogLevel::Warning,
            "Manager settings did not map AppConfig exactly.");

    const auto stopped = take(controller.control(
        {Domain::ManagerControlAction::Stop}, fixture.context()));
    require(stopped.state == Domain::ManagerServiceState::Stopped &&
                !stopped.desiredRunning && stopped.httpListening &&
                !stopped.serviceActive && fixture.runtime->pauseCalls() == 1U,
            "Stop did not preserve the source-compatible dashboard listener.");

    const auto resumed = take(controller.control(
        {Domain::ManagerControlAction::Start}, fixture.context()));
    require(resumed.state == Domain::ManagerServiceState::Running &&
                resumed.desiredRunning && resumed.httpListening &&
                resumed.serviceActive && fixture.runtime->startCalls() == 2U,
            "Start did not resume service and ensure the listener.");

    const auto restarted = take(controller.control(
        {Domain::ManagerControlAction::Restart}, fixture.context()));
    require(restarted.restartCount == 1U &&
                fixture.runtime->rebindCalls() == 1U,
            "Restart did not advance the runtime-owned count exactly once.");
    const auto repaired = take(controller.control(
        {Domain::ManagerControlAction::Repair}, fixture.context()));
    require(repaired.state == Domain::ManagerServiceState::Running &&
                fixture.runtime->reconcileCalls() == 1U &&
                fixture.runtime->rebindCalls() == 1U &&
                repaired.restartCount == 1U,
            "Repair performed an unconditional restart instead of reconciliation.");
}

void settingsPersistenceAndRuntimeApplicationAreExact()
{
    Fixture fixture;
    static_cast<void>(take(fixture.controller.initialize(fixture.context())));

    Domain::ManagerSettingsPatch nonbinding;
    nonbinding.dashboardRefreshInterval = 13s;
    nonbinding.autoRestart = false;
    nonbinding.watchdogInterval = 9s;
    nonbinding.openBrowserOnStart = true;
    nonbinding.sessionIdleTtl = 600s;
    nonbinding.shellTimeout = 50s;
    nonbinding.logLevel = Domain::LogLevel::Debug;
    const auto nonbindingOutcome = take(fixture.controller.updateSettings(
        nonbinding, true, fixture.context()));
    require(nonbindingOutcome.settings.dashboardRefreshInterval == 13s &&
                !nonbindingOutcome.settings.autoRestart &&
                nonbindingOutcome.settings.watchdogInterval == 9s &&
                nonbindingOutcome.settings.openBrowserOnStart &&
                nonbindingOutcome.settings.sessionIdleTtl == 600s &&
                nonbindingOutcome.settings.shellTimeout == 50s &&
                nonbindingOutcome.settings.logLevel == Domain::LogLevel::Debug,
            "Nonbinding settings were not returned from persisted AppConfig.");
    require(nonbindingOutcome.applied &&
                !nonbindingOutcome.bindingChanged &&
                nonbindingOutcome.status.state ==
                    Domain::ManagerServiceState::Running &&
                nonbindingOutcome.status.desiredRunning &&
                nonbindingOutcome.status.httpListening &&
                nonbindingOutcome.status.serviceActive &&
                nonbindingOutcome.status.dashboardRefreshInterval == 13s &&
                !nonbindingOutcome.status.autoRestart &&
                nonbindingOutcome.status.watchdogInterval == 9s,
            "Nonbinding update metadata was not captured with its stable status.");
    require(fixture.runtime->applyCalls() == 1U &&
                fixture.runtime->rebindCalls() == 0U,
            "Nonbinding settings incorrectly rebound the listener.");

    const auto mapped = fixture.store->lastPatch();
    require(mapped.has_value() && !mapped->allowedRoots && !mapped->shellEnabled &&
                !mapped->mcpRole && !mapped->coordinatorEnabled &&
                mapped->dashboardRefreshInterval == 13s &&
                mapped->managerAutoRestart == false &&
                mapped->managerWatchdogInterval == 9s &&
                mapped->managerOpenBrowserOnStart == true &&
                mapped->sessionIdleTimeToLive == 600s &&
                mapped->shellTimeout == 50s &&
                mapped->logLevel == Domain::LogLevel::Debug,
            "ManagerSettingsPatch was not mapped narrowly to AppConfigPatch.");

    Domain::ManagerSettingsPatch binding;
    binding.dashboardHost = "::1";
    binding.dashboardPort = static_cast<std::uint16_t>(8450U);
    const auto bindingOutcome = take(
        fixture.controller.updateSettings(binding, true, fixture.context()));
    require(bindingOutcome.applied && bindingOutcome.bindingChanged &&
                bindingOutcome.settings.dashboardHost == "::1" &&
                bindingOutcome.settings.dashboardPort == 8450U &&
                bindingOutcome.status.dashboardHost == "::1" &&
                bindingOutcome.status.dashboardPort == 8450U &&
                bindingOutcome.status.state ==
                    Domain::ManagerServiceState::Running &&
                bindingOutcome.status.httpListening &&
                bindingOutcome.status.serviceActive &&
                bindingOutcome.status.restartCount == 1U &&
                fixture.runtime->rebindCalls() == 1U &&
                fixture.runtime->applyCalls() == 1U,
            "Binding update metadata was not captured after the runtime rebind.");

    Domain::ManagerSettingsPatch deferred;
    deferred.dashboardPort = static_cast<std::uint16_t>(8451U);
    deferred.watchdogInterval = 10s;
    const auto deferredOutcome = take(fixture.controller.updateSettings(
        deferred, false, fixture.context()));
    require(deferredOutcome.settings.dashboardPort == 8451U &&
                deferredOutcome.settings.watchdogInterval == 10s &&
                !deferredOutcome.applied && deferredOutcome.bindingChanged &&
                deferredOutcome.status.dashboardHost == "::1" &&
                deferredOutcome.status.dashboardPort == 8451U &&
                deferredOutcome.status.watchdogInterval == 10s &&
                deferredOutcome.status.state ==
                    Domain::ManagerServiceState::Running &&
                deferredOutcome.status.httpListening &&
                deferredOutcome.status.serviceActive &&
                deferredOutcome.status.restartCount == 1U &&
                fixture.runtime->rebindCalls() == 1U &&
                fixture.runtime->applyCalls() == 1U,
            "Deferred update metadata was not captured without touching runtime.");
}

void invalidAndFailedSettingsRemainAtomic()
{
    Fixture fixture;
    static_cast<void>(take(fixture.controller.initialize(fixture.context())));
    const auto original = take(fixture.controller.settings(fixture.context()));

    Domain::ManagerSettingsPatch invalid;
    invalid.dashboardHost = "0.0.0.0";
    const auto invalidResult =
        fixture.controller.updateSettings(invalid, true, fixture.context());
    requireError(
        invalidResult,
        Domain::ErrorCodes::InvalidRequest,
        "An invalid manager patch returned an outcome or reached persistence.");
    require(fixture.store->updateCalls() == 0U &&
                fixture.runtime->applyCalls() == 0U &&
                fixture.runtime->rebindCalls() == 0U,
            "Invalid manager settings caused an external effect.");

    fixture.store->failUpdate(failure("store update fault"));
    Domain::ManagerSettingsPatch storeFault;
    storeFault.watchdogInterval = 12s;
    const auto storeFailure =
        fixture.controller.updateSettings(storeFault, true, fixture.context());
    requireError(
        storeFailure,
        Domain::ErrorCodes::InternalFailure,
        "A configuration-store failure returned an outcome or was not propagated.");
    require(take(fixture.controller.settings(fixture.context())).watchdogInterval ==
                original.watchdogInterval &&
                fixture.runtime->applyCalls() == 0U,
            "A failed persistent update changed cached settings or runtime state.");

    fixture.store->clearUpdateFailure();
    auto invalidReturned = Domain::defaultAppConfig();
    invalidReturned.dashboard.host = "example.test";
    fixture.store->returnConfig(invalidReturned);
    const auto invalidPersistedResult =
        fixture.controller.updateSettings(storeFault, true, fixture.context());
    requireError(
        invalidPersistedResult,
        Domain::ErrorCodes::InvalidRequest,
        "An invalid persisted configuration returned an outcome or was accepted.");
    require(take(fixture.controller.settings(fixture.context())).watchdogInterval ==
                original.watchdogInterval &&
                fixture.runtime->applyCalls() == 0U,
            "An invalid store result reached controller cache or runtime.");

    Fixture runtimeFixture;
    static_cast<void>(take(
        runtimeFixture.controller.initialize(runtimeFixture.context())));
    runtimeFixture.runtime->failApply(failure("runtime apply fault"));
    Domain::ManagerSettingsPatch runtimeFault;
    runtimeFault.watchdogInterval = 14s;
    const auto runtimeFailure = runtimeFixture.controller.updateSettings(
        runtimeFault, true, runtimeFixture.context());
    requireError(
        runtimeFailure,
        Domain::ErrorCodes::InternalFailure,
        "A runtime settings failure returned an outcome or was not propagated.");
    const auto persisted = take(
        runtimeFixture.controller.settings(runtimeFixture.context()));
    const auto failed = take(
        runtimeFixture.controller.status(runtimeFixture.context()));
    require(persisted.watchdogInterval == 14s &&
                failed.state == Domain::ManagerServiceState::Failed &&
                failed.desiredRunning && failed.lastError == "runtime apply fault",
            "Runtime failure lost persisted settings or failed-state semantics.");
}

void failedTransitionsPreserveDesiredStateAndRestartCount()
{
    {
        Fixture fixture;
        static_cast<void>(take(
            fixture.controller.initialize(fixture.context())));
        static_cast<void>(take(fixture.controller.control(
            {Domain::ManagerControlAction::Stop}, fixture.context())));
        fixture.runtime->failStart(failure("start fault"));
        requireError(
            fixture.controller.control(
                {Domain::ManagerControlAction::Start}, fixture.context()),
            Domain::ErrorCodes::InternalFailure,
            "A failed manager start was not propagated.");
        const auto failed = take(fixture.controller.status(fixture.context()));
        require(failed.state == Domain::ManagerServiceState::Failed &&
                    failed.desiredRunning && failed.lastError == "start fault",
                "Failed start did not preserve desired-running semantics.");
    }

    {
        Fixture fixture;
        static_cast<void>(take(
            fixture.controller.initialize(fixture.context())));
        fixture.runtime->failRebind(failure("rebind fault"));
        requireError(
            fixture.controller.control(
                {Domain::ManagerControlAction::Restart}, fixture.context()),
            Domain::ErrorCodes::InternalFailure,
            "A failed manager restart was not propagated.");
        const auto failed = take(fixture.controller.status(fixture.context()));
        require(failed.state == Domain::ManagerServiceState::Failed &&
                    failed.desiredRunning && failed.restartCount == 1U &&
                    fixture.runtime->rebindCalls() == 1U,
                "Failed restart did not expose exactly one attempted runtime rebind.");
    }

    {
        Fixture fixture;
        static_cast<void>(take(
            fixture.controller.initialize(fixture.context())));
        static_cast<void>(take(fixture.controller.control(
            {Domain::ManagerControlAction::Stop}, fixture.context())));
        fixture.runtime->failReconcile(failure("repair fault"));
        requireError(
            fixture.controller.control(
                {Domain::ManagerControlAction::Repair}, fixture.context()),
            Domain::ErrorCodes::InternalFailure,
            "A failed manager repair was not propagated.");
        const auto failed = take(fixture.controller.status(fixture.context()));
        require(failed.state == Domain::ManagerServiceState::Failed &&
                    !failed.desiredRunning && failed.lastError == "repair fault" &&
                    fixture.runtime->rebindCalls() == 0U,
                "Failed repair changed desired state or performed a restart.");
    }
}

void cancellationDeadlineShutdownAndCloseAreDistinct()
{
    {
        Fixture fixture;
        std::stop_source cancellation;
        cancellation.request_stop();
        requireError(
            fixture.controller.initialize(fixture.context(cancellation.get_token())),
            Domain::ErrorCodes::Cancelled,
            "Manager initialize ignored pre-cancellation.");
        require(fixture.store->loadCalls() == 0U &&
                    fixture.runtime->startCalls() == 0U,
                "Cancelled initialize reached a dependency.");
    }

    Fixture fixture;
    const auto expired = Domain::OperationContext{
        parse<Domain::OperationId>(
            "a6000000-0000-4000-8000-000000000002"),
        fixture.clock->monotonicNow(),
        {},
        parse<Domain::CorrelationId>("p16-expired")};
    requireError(
        fixture.controller.initialize(expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "Manager initialize ignored an expired deadline.");
    require(fixture.store->loadCalls() == 0U &&
                fixture.runtime->startCalls() == 0U,
            "Expired initialize reached a dependency.");

    static_cast<void>(take(fixture.controller.initialize(fixture.context())));
    const auto requested = take(
        fixture.controller.requestShutdown(fixture.context()));
    require(requested.shutdownRequested &&
                requested.status.state == Domain::ManagerServiceState::Stopping &&
                !requested.status.desiredRunning &&
                fixture.runtime->requestShutdownCalls() == 1U &&
                fixture.runtime->shutdownCalls() == 0U &&
                fixture.store->shutdownCalls() == 0U,
            "Shutdown intent closed transports instead of signaling the run loop.");

    std::vector<std::string> order;
    fixture.runtime->recordShutdownIn(order);
    fixture.store->recordShutdownIn(order);
    fixture.controller.shutdown();
    fixture.controller.shutdown();
    require(order == std::vector<std::string>{"runtime", "store"} &&
                fixture.runtime->shutdownCalls() == 1U &&
                fixture.store->shutdownCalls() == 1U,
            "Controller shutdown was not idempotent reverse-order teardown.");

    const auto runtimeCalls = fixture.runtime->startCalls() +
        fixture.runtime->pauseCalls() + fixture.runtime->rebindCalls() +
        fixture.runtime->reconcileCalls() + fixture.runtime->applyCalls() +
        fixture.runtime->snapshotCalls() +
        fixture.runtime->requestShutdownCalls();
    const auto storeCalls = fixture.store->loadCalls() + fixture.store->updateCalls();
    requireError(
        fixture.controller.status(fixture.context()),
        Domain::ErrorCodes::TransportClosed,
        "Status was accepted after controller shutdown.");
    requireError(
        fixture.controller.settings(fixture.context()),
        Domain::ErrorCodes::TransportClosed,
        "Settings were accepted after controller shutdown.");
    requireError(
        fixture.controller.control(
            {Domain::ManagerControlAction::Start}, fixture.context()),
        Domain::ErrorCodes::TransportClosed,
        "Control was accepted after controller shutdown.");
    requireError(
        fixture.controller.updateSettings({}, true, fixture.context()),
        Domain::ErrorCodes::TransportClosed,
        "Settings update was accepted after controller shutdown.");
    requireError(
        fixture.controller.requestShutdown(fixture.context()),
        Domain::ErrorCodes::TransportClosed,
        "Shutdown request was accepted after controller shutdown.");
    require(runtimeCalls == fixture.runtime->startCalls() +
                fixture.runtime->pauseCalls() + fixture.runtime->rebindCalls() +
                fixture.runtime->reconcileCalls() + fixture.runtime->applyCalls() +
                fixture.runtime->snapshotCalls() +
                fixture.runtime->requestShutdownCalls() &&
                storeCalls == fixture.store->loadCalls() + fixture.store->updateCalls(),
            "A post-shutdown call reached an injected dependency.");
}

void mutationAdmissionIsBoundedAndCallbacksAreReentrant()
{
    Fixture fixture;
    std::optional<Domain::ManagerServiceState> reentrantState;
    fixture.runtime->onStart([&]() {
        reentrantState = take(fixture.controller.status(fixture.context())).state;
    });
    static_cast<void>(take(fixture.controller.initialize(fixture.context())));
    require(reentrantState == Domain::ManagerServiceState::Starting &&
                fixture.runtime->snapshotCalls() == 0U,
            "Controller retained its state lock or recursively entered runtime status.");

    fixture.store->blockUpdate();
    Domain::ManagerSettingsPatch patch;
    patch.watchdogInterval = 17s;
    std::optional<Domain::Result<Domain::ManagerSettingsUpdateOutcome>>
        updateResult;
    std::jthread worker{[&]() {
        updateResult.emplace(
            fixture.controller.updateSettings(patch, false, fixture.context()));
    }};
    require(fixture.store->waitForBlockedUpdate(),
            "The deterministic settings update did not enter its blocking store.");
    const auto conflicting = fixture.controller.control(
        {Domain::ManagerControlAction::Restart}, fixture.context());
    requireError(
        conflicting,
        Domain::ErrorCodes::Conflict,
        "A concurrent manager mutation entered the capacity-one controller.");
    require(conflicting.error().retryable && fixture.runtime->rebindCalls() == 0U,
            "Concurrent mutation conflict was not retryable or reached runtime.");
    fixture.store->releaseUpdate();
    worker.join();
    require(updateResult.has_value() && updateResult.value(),
            "The admitted manager mutation did not finish after release.");
}

void invalidLoadedConfigurationFailsBeforeRuntime()
{
    Fixture fixture;
    auto invalid = Domain::defaultAppConfig();
    invalid.dashboard.host = "not-loopback.example";
    auto store = std::make_shared<RecordingConfigurationStore>(invalid);
    Application::ManagerController controller{
        store,
        fixture.runtime,
        fixture.clock,
        Domain::ManagerControllerOptions{
            take(Domain::PathText::create("C:\\ForgeFixture")),
            "0.9.0-test",
            4242U}};
    requireError(
        controller.initialize(fixture.context()),
        Domain::ErrorCodes::InvalidRequest,
        "Invalid loaded configuration reached the manager runtime.");
    require(fixture.runtime->startCalls() == 0U,
            "Invalid loaded configuration caused a runtime start.");
}

} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    try {
        lifecycleAndSourceCompatibleStopAreDeterministic();
        std::cout << "PASS manager_controller.lifecycle\n";
        settingsPersistenceAndRuntimeApplicationAreExact();
        std::cout << "PASS manager_controller.settings\n";
        invalidAndFailedSettingsRemainAtomic();
        std::cout << "PASS manager_controller.atomicity\n";
        failedTransitionsPreserveDesiredStateAndRestartCount();
        std::cout << "PASS manager_controller.failures\n";
        cancellationDeadlineShutdownAndCloseAreDistinct();
        std::cout << "PASS manager_controller.shutdown\n";
        mutationAdmissionIsBoundedAndCallbacksAreReentrant();
        std::cout << "PASS manager_controller.concurrency\n";
        invalidLoadedConfigurationFailsBeforeRuntime();
        std::cout << "PASS manager_controller.invalid_config\n";
        std::cout << "SUMMARY passed=7 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
