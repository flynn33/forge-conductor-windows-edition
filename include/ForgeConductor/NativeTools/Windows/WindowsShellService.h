#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "ForgeConductor/Contracts/IProcessSupervisor.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace ForgeConductor::NativeTools::Windows {

class WindowsShellService final : public Contracts::IShellService {
public:
    static constexpr std::chrono::milliseconds DefaultTimeout{30'000};
    static constexpr std::chrono::milliseconds MaximumTimeout{120'000};
    static constexpr std::size_t MaximumCommandBytes = 4U * 1024U;
    static constexpr std::size_t MaximumOutputBytes = 80'000U;
    static constexpr std::size_t MaximumErrorBytes = 20'000U;

    WindowsShellService(
        Domain::PathText powerShellExecutable,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor);
    ~WindowsShellService() override;

    WindowsShellService(const WindowsShellService&) = delete;
    WindowsShellService& operator=(const WindowsShellService&) = delete;
    WindowsShellService(WindowsShellService&&) = delete;
    WindowsShellService& operator=(WindowsShellService&&) = delete;

    // The request is a command envelope: executable must exactly equal the
    // injected PowerShell path and arguments must contain one command string.
    // This service owns the fixed PowerShell switches passed to the supervisor.
    [[nodiscard]] Domain::Result<Domain::ProcessResult> execute(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::NativeTools::Windows
