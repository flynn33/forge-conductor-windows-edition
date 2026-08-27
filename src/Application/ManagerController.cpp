#include "ForgeConductor/Application/ManagerController.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::Error controllerError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock,
    const std::string_view action)
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(controllerError(
            Domain::ErrorCodes::Cancelled,
            std::string{action} + " was cancelled."));
    }
    if (context.isExpired(clock.monotonicNow())) {
        return Domain::Result<void>::failure(controllerError(
            Domain::ErrorCodes::DeadlineExceeded,
            std::string{action} + " exceeded its deadline."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::ManagerSettings settingsFromConfig(
    const Domain::AppConfig& config)
{
    return Domain::ManagerSettings{
        config.dashboard.host,
        config.dashboard.port,
        config.dashboard.refreshInterval,
        config.manager.autoRestart,
        config.manager.watchdogInterval,
        config.manager.openBrowserOnStart,
        config.sessions.idleTimeToLive,
        config.shell.defaultTimeout,
        config.logLevel};
}

[[nodiscard]] Domain::AppConfigPatch configPatchFromManagerPatch(
    const Domain::ManagerSettingsPatch& patch)
{
    Domain::AppConfigPatch mapped;
    mapped.logLevel = patch.logLevel;
    mapped.shellTimeout = patch.shellTimeout;
    mapped.dashboardHost = patch.dashboardHost;
    mapped.dashboardPort = patch.dashboardPort;
    mapped.dashboardRefreshInterval = patch.dashboardRefreshInterval;
    mapped.managerAutoRestart = patch.autoRestart;
    mapped.managerWatchdogInterval = patch.watchdogInterval;
    mapped.managerOpenBrowserOnStart = patch.openBrowserOnStart;
    mapped.sessionIdleTimeToLive = patch.sessionIdleTtl;
    return mapped;
}

[[nodiscard]] bool bindingChanged(
    const Domain::AppConfig& before,
    const Domain::AppConfig& after) noexcept
{
    return before.dashboard.host != after.dashboard.host ||
        before.dashboard.port != after.dashboard.port;
}

[[nodiscard]] Domain::Result<void> validateRuntimeSnapshot(
    const Domain::ManagerRuntimeSnapshot& snapshot)
{
    if (snapshot.operationalServiceActive && !snapshot.listenerListening) {
        return Domain::Result<void>::failure(controllerError(
            Domain::ErrorCodes::IntegrityFailure,
            "The manager runtime reported an active service without its listener."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> requireRuntimeState(
    const Domain::ManagerRuntimeSnapshot& snapshot,
    const bool listenerListening,
    const bool operationalServiceActive,
    const bool shutdownRequested,
    const std::string_view action)
{
    auto valid = validateRuntimeSnapshot(snapshot);
    if (!valid) {
        return valid;
    }
    if (snapshot.listenerListening != listenerListening ||
        snapshot.operationalServiceActive != operationalServiceActive ||
        snapshot.shutdownRequested != shutdownRequested) {
        return Domain::Result<void>::failure(controllerError(
            Domain::ErrorCodes::IntegrityFailure,
            std::string{action} +
                " returned a runtime snapshot that does not satisfy the requested state."));
    }
    return Domain::Result<void>::success();
}

} // namespace

class ManagerController::Impl final {
public:
    Impl(
        std::shared_ptr<Contracts::IConfigurationStore> configurationStore,
        std::shared_ptr<Contracts::IManagerRuntime> runtime,
        std::shared_ptr<Contracts::IClock> clock,
        Domain::ManagerControllerOptions options)
        : configurationStore_{std::move(configurationStore)},
          runtime_{std::move(runtime)},
          clock_{std::move(clock)},
          options_{std::move(options)},
          configuration_{Domain::defaultAppConfig()},
          status_{
              true,
              true,
              Domain::ManagerServiceState::Stopped,
              false,
              false,
              false,
              options_.processId,
              std::nullopt,
              std::nullopt,
              0U,
              std::nullopt,
              configuration_.manager.autoRestart,
              configuration_.manager.watchdogInterval,
              configuration_.manager.openBrowserOnStart,
              configuration_.dashboard.host,
              configuration_.dashboard.port,
              configuration_.dashboard.refreshInterval,
              options_.home,
              options_.version}
    {
        if (!configurationStore_ || !runtime_ || !clock_) {
            throw std::invalid_argument(
                "ManagerController requires a configuration store, runtime, and clock.");
        }
        if (options_.version.empty() || options_.processId == 0U) {
            throw std::invalid_argument(
                "ManagerController requires a non-empty version and process identifier.");
        }

    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admitMutation(context, "Initialize the manager");
            if (!admitted) {
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(admitted).error());
            }
            auto lease = std::move(admitted).value();
            beginTransition(Domain::ManagerServiceState::Starting, true);

            auto loaded = configurationStore_->load(context);
            if (!loaded) {
                auto error = std::move(loaded).error();
                publishFailure(error, true, context, false);
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(error));
            }

            auto validConfig = Domain::validateAppConfig(loaded.value());
            if (!validConfig) {
                auto error = std::move(validConfig).error();
                publishFailure(error, true, context, false);
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(error));
            }

            auto contextState = validateContext(
                context, *clock_, "Initialize the manager");
            if (!contextState) {
                auto error = std::move(contextState).error();
                publishFailure(error, true, context, false);
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(error));
            }

            Domain::AppConfig config = std::move(loaded).value();
            publishConfiguration(config);
            auto started = runtime_->start(config, context);
            if (!started) {
                auto error = std::move(started).error();
                publishFailure(error, true, context, true);
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(error));
            }

            auto state = requireRuntimeState(
                started.value(), true, true, false, "Manager initialization");
            if (!state) {
                auto error = std::move(state).error();
                publishFailure(error, true, context, true);
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(error));
            }
            publishSuccess(started.value(), true, Domain::ManagerServiceState::Running);

            contextState = validateContext(
                context, *clock_, "Initialize the manager");
            if (!contextState) {
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(contextState).error());
            }
            return Domain::Result<Domain::ManagerStatus>::success(statusSnapshot());
        } catch (...) {
            auto error = controllerError(
                Domain::ErrorCodes::InternalFailure,
                "Manager initialization failed at the application boundary.");
            publishFailureWithoutRuntime(error, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admitObservation(context, "Read manager status");
            if (!admitted) {
                return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                    std::move(admitted).error());
            }
            auto lease = std::move(admitted).value();
            if (lease.refreshRuntime()) {
                auto observed = runtime_->snapshot(context);
                if (!observed) {
                    return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                        std::move(observed).error());
                }
                auto valid = validateRuntimeSnapshot(observed.value());
                if (!valid) {
                    return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                        std::move(valid).error());
                }
                publishObservation(observed.value());
            }

            auto contextState = validateContext(
                context, *clock_, "Read manager status");
            if (!contextState) {
                return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                    std::move(contextState).error());
            }
            return Domain::Result<Domain::ManagerControllerSnapshot>::success(
                controllerSnapshot());
        } catch (...) {
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                controllerError(
                    Domain::ErrorCodes::InternalFailure,
                    "Manager status could not be read at the application boundary."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept
    {
        auto current = snapshot(context);
        if (!current) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                std::move(current).error());
        }
        return Domain::Result<Domain::ManagerStatus>::success(
            std::move(current).value().status);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admitRead(context, "Read manager settings");
            if (!admitted) {
                return Domain::Result<Domain::ManagerSettings>::failure(
                    std::move(admitted).error());
            }
            auto lease = std::move(admitted).value();
            Domain::AppConfig config;
            {
                std::lock_guard lock{mutex_};
                config = configuration_;
            }
            return Domain::Result<Domain::ManagerSettings>::success(
                settingsFromConfig(config));
        } catch (...) {
            return Domain::Result<Domain::ManagerSettings>::failure(
                controllerError(
                    Domain::ErrorCodes::InternalFailure,
                    "Manager settings could not be read at the application boundary."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admitMutation(context, "Apply manager control");
            if (!admitted) {
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(admitted).error());
            }
            auto lease = std::move(admitted).value();
            switch (request.action) {
            case Domain::ManagerControlAction::Start:
                return start(context);
            case Domain::ManagerControlAction::Stop:
                return stop(context);
            case Domain::ManagerControlAction::Restart:
                return restart(context);
            case Domain::ManagerControlAction::Repair:
                return repair(context);
            }
            return Domain::Result<Domain::ManagerStatus>::failure(
                controllerError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The manager control action is not supported."));
        } catch (...) {
            const bool desired = desiredRunning();
            auto error = controllerError(
                Domain::ErrorCodes::InternalFailure,
                "Manager control failed at the application boundary.");
            publishFailureWithoutRuntime(error, desired);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admitMutation(context, "Update manager settings");
            if (!admitted) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(admitted).error());
            }
            auto lease = std::move(admitted).value();

            const auto mapped = configPatchFromManagerPatch(patch);
            const auto before = cachedState();
            auto prospective = Domain::applyConfigPatch(before.configuration, mapped);
            if (!prospective) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(prospective).error());
            }

            auto persisted = configurationStore_->update(mapped, context);
            if (!persisted) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(persisted).error());
            }
            auto validConfig = Domain::validateAppConfig(persisted.value());
            if (!validConfig) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(validConfig).error());
            }

            Domain::AppConfig config = std::move(persisted).value();
            const bool changedBinding =
                bindingChanged(before.configuration, config);
            publishConfiguration(config);
            auto contextState = validateContext(
                context, *clock_, "Update manager settings");
            if (!contextState) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(contextState).error());
            }
            if (!applyImmediately) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::success({
                    settingsFromConfig(config),
                    false,
                    changedBinding,
                    statusSnapshot()});
            }

            Domain::Result<Domain::ManagerRuntimeSnapshot> applied = changedBinding
                ? runtime_->rebind(config, before.desiredRunning, context)
                : runtime_->applySettings(config, context);
            if (!applied) {
                auto error = std::move(applied).error();
                publishFailure(error, before.desiredRunning, context, true);
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(error));
            }

            Domain::Result<void> runtimeState = changedBinding
                ? requireRuntimeState(
                      applied.value(), true, before.desiredRunning, false,
                      "Manager settings rebind")
                : validateNonbindingResult(before, applied.value());
            if (!runtimeState) {
                auto error = std::move(runtimeState).error();
                publishFailure(error, before.desiredRunning, context, true);
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(error));
            }
            publishStableSuccess(applied.value(), before.desiredRunning);

            contextState = validateContext(
                context, *clock_, "Update manager settings");
            if (!contextState) {
                return Domain::Result<
                    Domain::ManagerSettingsUpdateOutcome>::failure(
                    std::move(contextState).error());
            }
            return Domain::Result<
                Domain::ManagerSettingsUpdateOutcome>::success({
                settingsFromConfig(config),
                true,
                changedBinding,
                statusSnapshot()});
        } catch (...) {
            return Domain::Result<
                Domain::ManagerSettingsUpdateOutcome>::failure(
                controllerError(
                    Domain::ErrorCodes::InternalFailure,
                    "Manager settings failed at the application boundary."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext& context) noexcept
    {
        try {
            auto admitted = admitMutation(context, "Request manager shutdown");
            if (!admitted) {
                return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                    std::move(admitted).error());
            }
            auto lease = std::move(admitted).value();
            beginTransition(Domain::ManagerServiceState::Stopping, false);
            auto requested = runtime_->requestShutdown(context);
            if (!requested) {
                auto error = std::move(requested).error();
                publishFailure(error, false, context, true);
                return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                    std::move(error));
            }
            auto valid = validateRuntimeSnapshot(requested.value());
            if (!valid || !requested.value().shutdownRequested) {
                auto error = valid
                    ? controllerError(
                          Domain::ErrorCodes::IntegrityFailure,
                          "The manager runtime did not retain its shutdown request.")
                    : std::move(valid).error();
                publishFailure(error, false, context, true);
                return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                    std::move(error));
            }
            publishSuccess(
                requested.value(), false, Domain::ManagerServiceState::Stopping);
            auto contextState = validateContext(
                context, *clock_, "Request manager shutdown");
            if (!contextState) {
                return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                    std::move(contextState).error());
            }
            return Domain::Result<Domain::ManagerControllerSnapshot>::success(
                controllerSnapshot());
        } catch (...) {
            auto error = controllerError(
                Domain::ErrorCodes::InternalFailure,
                "Manager shutdown request failed at the application boundary.");
            publishFailureWithoutRuntime(error, false);
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                std::move(error));
        }
    }

    void shutdown() noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            if (shutdownCompleted_) {
                return;
            }
            if (shutdownInProgress_) {
                condition_.wait(lock, [this]() noexcept { return shutdownCompleted_; });
                return;
            }
            shutdownInProgress_ = true;
            closed_ = true;
            condition_.wait(lock, [this]() noexcept { return activeCalls_ == 0U; });
            lock.unlock();

            // Runtime was started after configuration load, so it is closed
            // first. No controller lock is held across either callback.
            runtime_->shutdown();
            configurationStore_->shutdown();

            lock.lock();
            shutdownCompleted_ = true;
            shutdownInProgress_ = false;
            condition_.notify_all();
        } catch (...) {
            try {
                runtime_->shutdown();
            } catch (...) {
            }
            try {
                configurationStore_->shutdown();
            } catch (...) {
            }
            try {
                std::lock_guard lock{mutex_};
                closed_ = true;
                shutdownCompleted_ = true;
                shutdownInProgress_ = false;
                condition_.notify_all();
            } catch (...) {
            }
        }
    }

private:
    struct CachedState final {
        Domain::AppConfig configuration;
        bool desiredRunning{};
        bool listenerListening{};
        bool operationalServiceActive{};
        std::uint32_t restartCount{};
    };

    class ReadLease final {
    public:
        explicit ReadLease(Impl& owner) noexcept : owner_{&owner} {}
        ~ReadLease() { release(); }

        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }
        ReadLease& operator=(ReadLease&&) = delete;

    private:
        friend class Impl;
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseRead();
                owner_ = nullptr;
            }
        }
        Impl* owner_{};
    };

    class MutationLease final {
    public:
        explicit MutationLease(Impl& owner) noexcept : owner_{&owner} {}
        ~MutationLease() { release(); }

        MutationLease(const MutationLease&) = delete;
        MutationLease& operator=(const MutationLease&) = delete;
        MutationLease(MutationLease&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }
        MutationLease& operator=(MutationLease&&) = delete;

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseMutation();
                owner_ = nullptr;
            }
        }
        Impl* owner_{};
    };

    class ObservationLease final {
    public:
        ObservationLease(Impl& owner, const bool refreshRuntime) noexcept
            : owner_{&owner}, refreshRuntime_{refreshRuntime}
        {
        }
        ~ObservationLease() { release(); }

        ObservationLease(const ObservationLease&) = delete;
        ObservationLease& operator=(const ObservationLease&) = delete;
        ObservationLease(ObservationLease&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)},
              refreshRuntime_{other.refreshRuntime_}
        {
        }
        ObservationLease& operator=(ObservationLease&&) = delete;

        [[nodiscard]] bool refreshRuntime() const noexcept
        {
            return refreshRuntime_;
        }

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseObservation(refreshRuntime_);
                owner_ = nullptr;
            }
        }
        Impl* owner_{};
        bool refreshRuntime_{};
    };

    [[nodiscard]] Domain::Result<ReadLease> admitRead(
        const Domain::OperationContext& context,
        const std::string_view action) noexcept
    {
        auto valid = validateContext(context, *clock_, action);
        if (!valid) {
            return Domain::Result<ReadLease>::failure(std::move(valid).error());
        }
        std::lock_guard lock{mutex_};
        if (closed_) {
            return Domain::Result<ReadLease>::failure(closedError());
        }
        ++activeCalls_;
        return Domain::Result<ReadLease>::success(ReadLease{*this});
    }

    [[nodiscard]] Domain::Result<MutationLease> admitMutation(
        const Domain::OperationContext& context,
        const std::string_view action) noexcept
    {
        auto valid = validateContext(context, *clock_, action);
        if (!valid) {
            return Domain::Result<MutationLease>::failure(
                std::move(valid).error());
        }
        std::lock_guard lock{mutex_};
        if (closed_) {
            return Domain::Result<MutationLease>::failure(closedError());
        }
        if (mutationActive_ || observationActive_) {
            return Domain::Result<MutationLease>::failure(controllerError(
                Domain::ErrorCodes::Conflict,
                "One bounded manager operation is already using the runtime.",
                true));
        }
        mutationActive_ = true;
        ++activeCalls_;
        return Domain::Result<MutationLease>::success(MutationLease{*this});
    }

    [[nodiscard]] Domain::Result<ObservationLease> admitObservation(
        const Domain::OperationContext& context,
        const std::string_view action) noexcept
    {
        auto valid = validateContext(context, *clock_, action);
        if (!valid) {
            return Domain::Result<ObservationLease>::failure(
                std::move(valid).error());
        }
        std::lock_guard lock{mutex_};
        if (closed_) {
            return Domain::Result<ObservationLease>::failure(closedError());
        }
        const bool refreshRuntime = !mutationActive_ && !observationActive_;
        if (refreshRuntime) {
            observationActive_ = true;
        }
        ++activeCalls_;
        return Domain::Result<ObservationLease>::success(
            ObservationLease{*this, refreshRuntime});
    }

    void releaseRead() noexcept
    {
        std::lock_guard lock{mutex_};
        if (activeCalls_ != 0U) {
            --activeCalls_;
        }
        condition_.notify_all();
    }

    void releaseMutation() noexcept
    {
        std::lock_guard lock{mutex_};
        mutationActive_ = false;
        if (activeCalls_ != 0U) {
            --activeCalls_;
        }
        condition_.notify_all();
    }

    void releaseObservation(const bool refreshedRuntime) noexcept
    {
        std::lock_guard lock{mutex_};
        if (refreshedRuntime) {
            observationActive_ = false;
        }
        if (activeCalls_ != 0U) {
            --activeCalls_;
        }
        condition_.notify_all();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> start(
        const Domain::OperationContext& context)
    {
        beginTransition(Domain::ManagerServiceState::Starting, true);
        const auto config = configurationSnapshot();
        auto started = runtime_->start(config, context);
        if (!started) {
            auto error = std::move(started).error();
            publishFailure(error, true, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        auto valid = requireRuntimeState(
            started.value(), true, true, false, "Manager start");
        if (!valid) {
            auto error = std::move(valid).error();
            publishFailure(error, true, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        publishSuccess(started.value(), true, Domain::ManagerServiceState::Running);
        return completeControl(context, "Start the manager");
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> stop(
        const Domain::OperationContext& context)
    {
        beginTransition(Domain::ManagerServiceState::Stopping, false);
        auto paused = runtime_->pause(context);
        if (!paused) {
            auto error = std::move(paused).error();
            publishFailure(error, false, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        auto valid = requireRuntimeState(
            paused.value(), true, false, false, "Manager stop");
        if (!valid) {
            auto error = std::move(valid).error();
            publishFailure(error, false, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        publishSuccess(paused.value(), false, Domain::ManagerServiceState::Stopped);
        return completeControl(context, "Stop the manager");
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> restart(
        const Domain::OperationContext& context)
    {
        beginTransition(Domain::ManagerServiceState::Restarting, true);
        const auto config = configurationSnapshot();
        auto rebound = runtime_->rebind(config, true, context);
        if (!rebound) {
            auto error = std::move(rebound).error();
            publishFailure(error, true, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        auto valid = requireRuntimeState(
            rebound.value(), true, true, false, "Manager restart");
        if (!valid) {
            auto error = std::move(valid).error();
            publishFailure(error, true, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        publishSuccess(rebound.value(), true, Domain::ManagerServiceState::Running);
        return completeControl(context, "Restart the manager");
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> repair(
        const Domain::OperationContext& context)
    {
        const auto before = cachedState();
        auto reconciled = runtime_->reconcile(
            before.configuration, before.desiredRunning, context);
        if (!reconciled) {
            auto error = std::move(reconciled).error();
            publishFailure(error, before.desiredRunning, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        auto valid = requireRuntimeState(
            reconciled.value(), true, before.desiredRunning, false,
            "Manager repair");
        if (!valid) {
            auto error = std::move(valid).error();
            publishFailure(error, before.desiredRunning, context, true);
            return Domain::Result<Domain::ManagerStatus>::failure(std::move(error));
        }
        publishStableSuccess(reconciled.value(), before.desiredRunning);
        return completeControl(context, "Repair the manager");
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> completeControl(
        const Domain::OperationContext& context,
        const std::string_view action)
    {
        auto valid = validateContext(context, *clock_, action);
        if (!valid) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                std::move(valid).error());
        }
        return Domain::Result<Domain::ManagerStatus>::success(statusSnapshot());
    }

    [[nodiscard]] Domain::Result<void> validateNonbindingResult(
        const CachedState& before,
        const Domain::ManagerRuntimeSnapshot& after) const
    {
        auto valid = validateRuntimeSnapshot(after);
        if (!valid) {
            return valid;
        }
        if (after.listenerListening != before.listenerListening ||
            after.operationalServiceActive != before.operationalServiceActive ||
            after.restartCount != before.restartCount ||
            after.shutdownRequested) {
            return Domain::Result<void>::failure(controllerError(
                Domain::ErrorCodes::IntegrityFailure,
                "A nonbinding manager settings update changed runtime ownership state."));
        }
        return Domain::Result<void>::success();
    }

    void beginTransition(
        const Domain::ManagerServiceState state,
        const bool desiredRunning) noexcept
    {
        std::lock_guard lock{mutex_};
        status_.state = state;
        status_.desiredRunning = desiredRunning;
        status_.ok = true;
        status_.lastError.reset();
    }

    void publishConfiguration(const Domain::AppConfig& configuration)
    {
        std::lock_guard lock{mutex_};
        configuration_ = configuration;
        applyConfigurationToStatusLocked();
    }

    void publishSuccess(
        const Domain::ManagerRuntimeSnapshot& runtime,
        const bool desiredRunning,
        const Domain::ManagerServiceState state) noexcept
    {
        std::lock_guard lock{mutex_};
        applyRuntimeToStatusLocked(runtime);
        status_.state = state;
        status_.desiredRunning = desiredRunning;
        status_.ok = true;
        status_.lastError.reset();
        shutdownRequested_ = runtime.shutdownRequested;
    }

    void publishStableSuccess(
        const Domain::ManagerRuntimeSnapshot& runtime,
        const bool desiredRunning) noexcept
    {
        publishSuccess(
            runtime,
            desiredRunning,
            desiredRunning
                ? Domain::ManagerServiceState::Running
                : Domain::ManagerServiceState::Stopped);
    }

    void publishObservation(
        const Domain::ManagerRuntimeSnapshot& runtime) noexcept
    {
        std::lock_guard lock{mutex_};
        applyRuntimeToStatusLocked(runtime);
        if (runtime.shutdownRequested || shutdownRequested_) {
            shutdownRequested_ = true;
            status_.desiredRunning = false;
            status_.state = Domain::ManagerServiceState::Stopping;
            status_.ok = true;
            status_.lastError.reset();
            return;
        }
        if (runtime.lastError) {
            status_.state = Domain::ManagerServiceState::Failed;
            status_.ok = false;
            status_.lastError = runtime.lastError;
            return;
        }
        if (status_.desiredRunning) {
            if (runtime.listenerListening && runtime.operationalServiceActive) {
                status_.state = Domain::ManagerServiceState::Running;
                status_.ok = true;
                status_.lastError.reset();
            } else {
                status_.state = Domain::ManagerServiceState::Failed;
                status_.ok = false;
                status_.lastError =
                    "The manager runtime does not satisfy desired running state.";
            }
        } else if (runtime.operationalServiceActive) {
            status_.state = Domain::ManagerServiceState::Failed;
            status_.ok = false;
            status_.lastError =
                "The manager runtime is active while desired state is stopped.";
        } else {
            status_.state = Domain::ManagerServiceState::Stopped;
            status_.ok = true;
            status_.lastError.reset();
        }
    }

    void publishFailure(
        const Domain::Error& error,
        const bool desiredRunning,
        const Domain::OperationContext& context,
        const bool observeRuntime) noexcept
    {
        std::optional<Domain::ManagerRuntimeSnapshot> observed;
        if (observeRuntime) {
            auto snapshotResult = runtime_->snapshot(context);
            if (snapshotResult && validateRuntimeSnapshot(snapshotResult.value())) {
                observed = std::move(snapshotResult).value();
            }
        }
        std::lock_guard lock{mutex_};
        if (observed) {
            applyRuntimeToStatusLocked(*observed);
            shutdownRequested_ = shutdownRequested_ || observed->shutdownRequested;
        }
        status_.state = Domain::ManagerServiceState::Failed;
        status_.desiredRunning = desiredRunning;
        status_.ok = false;
        status_.lastError = error.message;
    }

    void publishFailureWithoutRuntime(
        const Domain::Error& error,
        const bool desiredRunning) noexcept
    {
        std::lock_guard lock{mutex_};
        status_.state = Domain::ManagerServiceState::Failed;
        status_.desiredRunning = desiredRunning;
        status_.ok = false;
        status_.lastError = error.message;
    }

    void applyRuntimeToStatusLocked(
        const Domain::ManagerRuntimeSnapshot& runtime) noexcept
    {
        status_.httpListening = runtime.listenerListening;
        status_.serviceActive = runtime.operationalServiceActive;
        status_.startedAt = runtime.startedAt;
        status_.restartCount = runtime.restartCount;
    }

    void applyConfigurationToStatusLocked()
    {
        status_.autoRestart = configuration_.manager.autoRestart;
        status_.watchdogInterval = configuration_.manager.watchdogInterval;
        status_.openBrowserOnStart = configuration_.manager.openBrowserOnStart;
        status_.dashboardHost = configuration_.dashboard.host;
        status_.dashboardPort = configuration_.dashboard.port;
        status_.dashboardRefreshInterval = configuration_.dashboard.refreshInterval;
    }

    [[nodiscard]] Domain::ManagerStatus statusSnapshot() const noexcept
    {
        Domain::ManagerStatus result = [this]() {
            std::lock_guard lock{mutex_};
            return status_;
        }();
        if (result.startedAt) {
            const auto now = clock_->utcNow();
            result.uptime = now >= *result.startedAt
                ? std::chrono::duration_cast<std::chrono::seconds>(
                      now - *result.startedAt)
                : std::chrono::seconds::zero();
        } else {
            result.uptime.reset();
        }
        return result;
    }

    [[nodiscard]] Domain::ManagerControllerSnapshot controllerSnapshot()
        const noexcept
    {
        Domain::ManagerControllerSnapshot result = [this]() {
            std::lock_guard lock{mutex_};
            return Domain::ManagerControllerSnapshot{
                status_, shutdownRequested_};
        }();
        if (result.status.startedAt) {
            const auto now = clock_->utcNow();
            result.status.uptime = now >= *result.status.startedAt
                ? std::chrono::duration_cast<std::chrono::seconds>(
                      now - *result.status.startedAt)
                : std::chrono::seconds::zero();
        } else {
            result.status.uptime.reset();
        }
        return result;
    }

    [[nodiscard]] CachedState cachedState() const
    {
        std::lock_guard lock{mutex_};
        return CachedState{
            configuration_,
            status_.desiredRunning,
            status_.httpListening,
            status_.serviceActive,
            status_.restartCount};
    }

    [[nodiscard]] Domain::AppConfig configurationSnapshot() const
    {
        std::lock_guard lock{mutex_};
        return configuration_;
    }

    [[nodiscard]] bool desiredRunning() const noexcept
    {
        std::lock_guard lock{mutex_};
        return status_.desiredRunning;
    }

    [[nodiscard]] static Domain::Error closedError()
    {
        return controllerError(
            Domain::ErrorCodes::TransportClosed,
            "The manager controller is shut down.");
    }

    std::shared_ptr<Contracts::IConfigurationStore> configurationStore_;
    std::shared_ptr<Contracts::IManagerRuntime> runtime_;
    std::shared_ptr<Contracts::IClock> clock_;
    const Domain::ManagerControllerOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Domain::AppConfig configuration_;
    Domain::ManagerStatus status_;
    std::size_t activeCalls_{};
    bool mutationActive_{};
    bool observationActive_{};
    bool shutdownRequested_{};
    bool closed_{};
    bool shutdownInProgress_{};
    bool shutdownCompleted_{};
};

ManagerController::ManagerController(
    std::shared_ptr<Contracts::IConfigurationStore> configurationStore,
    std::shared_ptr<Contracts::IManagerRuntime> runtime,
    std::shared_ptr<Contracts::IClock> clock,
    Domain::ManagerControllerOptions options)
    : implementation_{std::make_unique<Impl>(
          std::move(configurationStore),
          std::move(runtime),
          std::move(clock),
          std::move(options))}
{
}

ManagerController::~ManagerController() noexcept
{
    shutdown();
}

Domain::Result<Domain::ManagerStatus> ManagerController::initialize(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->initialize(context);
}

Domain::Result<Domain::ManagerControllerSnapshot> ManagerController::snapshot(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->snapshot(context);
}

Domain::Result<Domain::ManagerStatus> ManagerController::status(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->status(context);
}

Domain::Result<Domain::ManagerSettings> ManagerController::settings(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->settings(context);
}

Domain::Result<Domain::ManagerStatus> ManagerController::control(
    const Domain::ManagerControlRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->control(request, context);
}

Domain::Result<Domain::ManagerSettingsUpdateOutcome>
ManagerController::updateSettings(
    const Domain::ManagerSettingsPatch& patch,
    const bool applyImmediately,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->updateSettings(patch, applyImmediately, context);
}

Domain::Result<Domain::ManagerControllerSnapshot>
ManagerController::requestShutdown(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->requestShutdown(context);
}

void ManagerController::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Application
