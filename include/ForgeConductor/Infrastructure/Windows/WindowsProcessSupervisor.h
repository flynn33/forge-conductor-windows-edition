#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IProcessSupervisor.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {
namespace Detail {
class IProcessLaunchObserver;
struct ProcessSupervisorTestAccess;
} // namespace Detail

class WindowsProcessSupervisor final : public Contracts::IProcessSupervisor {
public:
    WindowsProcessSupervisor(Domain::ResourceBudgets budgets,
                             std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics);
    ~WindowsProcessSupervisor() override;

    WindowsProcessSupervisor(const WindowsProcessSupervisor&) = delete;
    WindowsProcessSupervisor& operator=(const WindowsProcessSupervisor&) = delete;
    WindowsProcessSupervisor(WindowsProcessSupervisor&&) = delete;
    WindowsProcessSupervisor& operator=(WindowsProcessSupervisor&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ProcessResult>
    run(const Domain::ProcessRequest& request, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void cancelAll() noexcept override;
    void shutdown() noexcept override;

private:
    friend struct Detail::ProcessSupervisorTestAccess;

    WindowsProcessSupervisor(Domain::ResourceBudgets budgets,
                             std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
                             std::shared_ptr<Detail::IProcessLaunchObserver> launchObserver);

    class Impl;
    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
