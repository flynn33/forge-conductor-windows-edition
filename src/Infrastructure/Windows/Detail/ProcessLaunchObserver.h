#pragma once

#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"

#include <memory>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class IProcessLaunchObserver {
public:
    virtual ~IProcessLaunchObserver() = default;

    virtual void beforeCreateProcess() noexcept = 0;
    virtual void afterCreateProcess() noexcept = 0;
};

struct ProcessSupervisorTestAccess final {
    ProcessSupervisorTestAccess() = delete;

    [[nodiscard]] static std::unique_ptr<WindowsProcessSupervisor>
    create(Domain::ResourceBudgets budgets,
           std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
           std::shared_ptr<IProcessLaunchObserver> launchObserver)
    {
        return std::unique_ptr<WindowsProcessSupervisor>{new WindowsProcessSupervisor{
            std::move(budgets), std::move(runtimeDiagnostics), std::move(launchObserver)}};
    }
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
