#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerRuntime.h"
#include "ForgeConductor/Dashboard/IDashboardConnectionApplicationFactory.h"

#include <memory>

namespace ForgeConductor::Hosts::Manager {

// Windows process adapter that owns the dashboard transport for the complete
// Manager lifetime. All public operations are serialized by the private
// implementation; same-endpoint replacement drains and destroys the old
// transport before a successor may bind the exclusive loopback endpoint.
class WindowsManagerRuntime final : public Contracts::IManagerRuntime {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsManagerRuntime>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Dashboard::IDashboardConnectionApplicationFactory>
            applicationFactory) noexcept;

    ~WindowsManagerRuntime() noexcept override;

    WindowsManagerRuntime(const WindowsManagerRuntime&) = delete;
    WindowsManagerRuntime& operator=(const WindowsManagerRuntime&) = delete;
    WindowsManagerRuntime(WindowsManagerRuntime&&) = delete;
    WindowsManagerRuntime& operator=(WindowsManagerRuntime&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> start(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> pause(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> rebind(
        const Domain::AppConfig& config,
        bool operationalServiceDesired,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> reconcile(
        const Domain::AppConfig& config,
        bool operationalServiceDesired,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> applySettings(
        const Domain::AppConfig& config,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagerRuntimeSnapshot>
    requestShutdown(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;

    explicit WindowsManagerRuntime(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Hosts::Manager
