#pragma once

#include "ForgeConductor/Contracts/IConfigurationStore.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerRuntime.h"

#include <memory>

namespace ForgeConductor::Application {

// Pure application orchestration for one manager process. Mutation admission
// has capacity one and never retains its state lock while invoking an injected
// dependency. The controller owns the shutdown order: runtime, then store.
class ManagerController final : public Contracts::IManagerController {
public:
    ManagerController(
        std::shared_ptr<Contracts::IConfigurationStore> configurationStore,
        std::shared_ptr<Contracts::IManagerRuntime> runtime,
        std::shared_ptr<Contracts::IClock> clock,
        Domain::ManagerControllerOptions options);
    ~ManagerController() noexcept override;

    ManagerController(const ManagerController&) = delete;
    ManagerController& operator=(const ManagerController&) = delete;
    ManagerController(ManagerController&&) = delete;
    ManagerController& operator=(ManagerController&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        bool applyImmediately,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
