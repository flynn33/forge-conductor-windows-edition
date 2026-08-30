#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerServices.h"
#include "ForgeConductor/Manager/ManagerTransportLimits.h"

#include <memory>
#include <string>

namespace ForgeConductor::Infrastructure::Windows {

// Synchronous current-user manager client. Each call owns one independent
// named-pipe connection and one request/response exchange. The shared
// implementation keeps concurrent calls alive while shutdown cancels native
// I/O and closes admission for new work.
class WindowsManagerNamedPipeClient final : public Contracts::IManagerClient {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsManagerNamedPipeClient>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::wstring pipeName,
        Domain::Sha256Digest nonce,
        Manager::ManagerTransportLimits limits = {}) noexcept;

    ~WindowsManagerNamedPipeClient() noexcept override;

    WindowsManagerNamedPipeClient(const WindowsManagerNamedPipeClient&) = delete;
    WindowsManagerNamedPipeClient& operator=(const WindowsManagerNamedPipeClient&) = delete;
    WindowsManagerNamedPipeClient(WindowsManagerNamedPipeClient&&) = delete;
    WindowsManagerNamedPipeClient& operator=(WindowsManagerNamedPipeClient&&) = delete;

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

    void shutdown() noexcept override;

private:
    class Impl;

    explicit WindowsManagerNamedPipeClient(std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
