#pragma once

#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/ManagerRuntimeModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

namespace ForgeConductor::Contracts {

// Platform boundary owned by the manager host. Successful mutations return the
// resulting immutable runtime snapshot. rebind owns restart-count advancement,
// including the attempted rebind count exposed after a failed call.
class IManagerRuntime {
public:
    virtual ~IManagerRuntime() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot> start(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context) noexcept = 0;

    // Pauses operational APIs while preserving the dashboard/control listener.
    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot> pause(
        const Domain::OperationContext& context) noexcept = 0;

    // Replaces the dashboard binding and restores the supplied desired service
    // state. The runtime increments restartCount exactly once per invocation.
    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot> rebind(
        const Domain::AppConfig& config,
        bool operationalServiceDesired,
        const Domain::OperationContext& context) noexcept = 0;

    // Repairs drift without implying that a listener restart is necessary.
    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot> reconcile(
        const Domain::AppConfig& config,
        bool operationalServiceDesired,
        const Domain::OperationContext& context) noexcept = 0;

    // Applies watchdog, refresh, logging, and other settings that do not change
    // the listener binding.
    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot>
    applySettings(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept = 0;

    // Signals the manager host run loop. This does not close client transports.
    [[nodiscard]] virtual Domain::Result<Domain::ManagerRuntimeSnapshot>
    requestShutdown(const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IManagerController {
public:
    virtual ~IManagerController() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerControllerSnapshot>
    snapshot(const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        bool applyImmediately,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext& context) noexcept = 0;

    // Closes the application boundary and dependencies. It is intentionally
    // distinct from requestShutdown, which only signals the manager run loop.
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
