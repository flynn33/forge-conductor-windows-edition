#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILMStudioHostActivator.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

struct WindowsLMStudioHostActivatorOptions final {
    std::chrono::milliseconds observationInterval{std::chrono::milliseconds{100}};
    std::size_t maximumSynchronizedConfigurationBytes{2U * 1024U * 1024U};
};

class WindowsLMStudioHostActivator final : public Contracts::ILMStudioHostActivator {
public:
    WindowsLMStudioHostActivator(
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IFileSystem& fileSystem,
        Contracts::IClock& clock,
        WindowsLMStudioHostActivatorOptions options = {});

    WindowsLMStudioHostActivator(
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IFileSystem& fileSystem,
        Contracts::IClock& clock,
        std::unique_ptr<Contracts::ILMStudioHostPlatform> hostPlatform,
        WindowsLMStudioHostActivatorOptions options = {});

    ~WindowsLMStudioHostActivator() noexcept override;

    WindowsLMStudioHostActivator(const WindowsLMStudioHostActivator&) = delete;
    WindowsLMStudioHostActivator& operator=(const WindowsLMStudioHostActivator&) = delete;
    WindowsLMStudioHostActivator(WindowsLMStudioHostActivator&&) = delete;
    WindowsLMStudioHostActivator& operator=(WindowsLMStudioHostActivator&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioEnvironmentStatus& environment,
        const Domain::LMStudioHostActivationRequest& request,
        const Contracts::WorkspaceAuthority& executionAuthority,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
