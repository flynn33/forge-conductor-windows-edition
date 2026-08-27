#pragma once

#include "ForgeConductor/Contracts/ILMStudioServeVerifier.h"
#include "ForgeConductor/Contracts/IProcessSupervisor.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsLMStudioServeVerifier final : public Contracts::ILMStudioServeVerifier {
public:
    explicit WindowsLMStudioServeVerifier(
        Contracts::IProcessSupervisor& processSupervisor) noexcept;
    ~WindowsLMStudioServeVerifier() override;

    WindowsLMStudioServeVerifier(const WindowsLMStudioServeVerifier&) = delete;
    WindowsLMStudioServeVerifier& operator=(const WindowsLMStudioServeVerifier&) = delete;
    WindowsLMStudioServeVerifier(WindowsLMStudioServeVerifier&&) = delete;
    WindowsLMStudioServeVerifier& operator=(WindowsLMStudioServeVerifier&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LMStudioConnectorHealth> verify(
        const Domain::PathText& binaryPath,
        const Domain::PathText& forgeHome,
        Domain::LMStudioConnectorRole role,
        const std::optional<Domain::DeploymentId>& deploymentId,
        const Contracts::WorkspaceAuthority& executionAuthority,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
