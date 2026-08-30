#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"
#include "ForgeConductor/Manager/ManagerTransportLimits.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace ForgeConductor::Infrastructure::Windows {

struct WindowsManagerNamedPipeServerOptions final {
    std::wstring pipeName;
    Manager::ManagerTransportLimits limits;
    std::size_t workerCount{4U};
    std::chrono::milliseconds ingressTimeout{std::chrono::seconds{2}};
};

// Process boundary for a named-pipe worker that cannot quiesce inside the
// bounded shutdown drain. Production terminates. Focused tests inject a
// recorder that returns; the server must then retain every worker and borrowed
// dependency behind an exact join barrier before returning a failure.
class IManagerNamedPipeServerFailFast {
public:
    virtual ~IManagerNamedPipeServerFailFast() noexcept = default;
    virtual void failFast() noexcept = 0;
};

// Fixed-worker current-user manager ingress. Each connected pipe instance owns
// exactly one authenticated request/response exchange before disconnecting.
class WindowsManagerNamedPipeServer final : public Contracts::IManagerServer {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Manager::ManagerRequestDispatcher> dispatcher,
        WindowsCurrentUserIdentity ownerIdentity,
        Domain::Sha256Digest nonce,
        WindowsManagerNamedPipeServerOptions options) noexcept;

    // Focused verification seam. Production composition must use the overload
    // above so terminal worker non-quiescence cannot return to its caller.
    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Manager::ManagerRequestDispatcher> dispatcher,
        WindowsCurrentUserIdentity ownerIdentity,
        Domain::Sha256Digest nonce,
        WindowsManagerNamedPipeServerOptions options,
        std::shared_ptr<IManagerNamedPipeServerFailFast> failFast) noexcept;

    ~WindowsManagerNamedPipeServer() noexcept override;

    WindowsManagerNamedPipeServer(const WindowsManagerNamedPipeServer&) = delete;
    WindowsManagerNamedPipeServer& operator=(
        const WindowsManagerNamedPipeServer&) = delete;
    WindowsManagerNamedPipeServer(WindowsManagerNamedPipeServer&&) = delete;
    WindowsManagerNamedPipeServer& operator=(WindowsManagerNamedPipeServer&&) = delete;

    [[nodiscard]] Domain::Result<void> run(
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;

    explicit WindowsManagerNamedPipeServer(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
