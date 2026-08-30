#pragma once

#include "ForgeConductor/Contracts/IManagerRuntime.h"
#include "ForgeConductor/Contracts/IManagerServices.h"

#include <memory>
#include <mutex>

namespace ForgeConductor::Hosts::Manager {

class ManagerProcessRestartSignal;

// Process-local dashboard adapter. It retains only a weak controller edge so
// the controller/runtime/dashboard ownership graph cannot form a cycle.
class ManagerControllerClient final : public Contracts::IManagerClient {
public:
    explicit ManagerControllerClient(
        ManagerProcessRestartSignal& restartSignal) noexcept;
    ~ManagerControllerClient() noexcept override;

    ManagerControllerClient(const ManagerControllerClient&) = delete;
    ManagerControllerClient& operator=(const ManagerControllerClient&) = delete;
    ManagerControllerClient(ManagerControllerClient&&) = delete;
    ManagerControllerClient& operator=(ManagerControllerClient&&) = delete;

    // Composition-only bind edge. A live controller may be installed exactly
    // once and is weakly retained thereafter.
    [[nodiscard]] Domain::Result<void> bind(
        const std::shared_ptr<Contracts::IManagerController>& controller)
        noexcept;

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        bool applyImmediately,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> requestRestart(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> requestShutdown(
        const Domain::OperationContext& context) noexcept override;

    // Closes only this adapter. Controller/process shutdown remains owned by
    // the Manager composition root and process host.
    void shutdown() noexcept override;

private:
    [[nodiscard]] Domain::Result<
        std::shared_ptr<Contracts::IManagerController>>
    pinController() noexcept;

    std::mutex mutex_;
    ManagerProcessRestartSignal& restartSignal_;
    std::weak_ptr<Contracts::IManagerController> controller_;
    bool bound_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Hosts::Manager
