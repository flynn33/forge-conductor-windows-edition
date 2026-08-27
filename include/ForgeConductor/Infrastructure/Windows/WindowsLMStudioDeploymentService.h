#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Contracts/ILMStudioEnvironment.h"
#include "ForgeConductor/Contracts/ILMStudioHostActivator.h"
#include "ForgeConductor/Contracts/ILMStudioServeVerifier.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsLMStudioDeploymentService final
    : public Contracts::ILMStudioDeploymentService {
public:
    WindowsLMStudioDeploymentService(
        Contracts::ILMStudioEnvironment& environment,
        Contracts::ILMStudioServeVerifier& serveVerifier,
        Contracts::ILMStudioHostActivator& hostActivator,
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IFileSystem& fileSystem,
        Contracts::IAtomicFileStore& atomicFileStore,
        Contracts::IApplicationPaths& applicationPaths,
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator,
        Contracts::IDiagnosticSink& diagnostics);

    ~WindowsLMStudioDeploymentService() noexcept override;

    WindowsLMStudioDeploymentService(const WindowsLMStudioDeploymentService&) = delete;
    WindowsLMStudioDeploymentService& operator=(const WindowsLMStudioDeploymentService&) = delete;
    WindowsLMStudioDeploymentService(WindowsLMStudioDeploymentService&&) = delete;
    WindowsLMStudioDeploymentService& operator=(WindowsLMStudioDeploymentService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LMStudioPluginStatus> status(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioHostActivationRequest& request,
        const Contracts::WorkspaceAuthority& executionAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
