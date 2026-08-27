#pragma once

#include "ForgeConductor/Contracts/IManagerRuntime.h"
#include "ForgeConductor/Contracts/IManagerServices.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include <memory>
#include <mutex>
#include <stop_token>

namespace ForgeConductor::Hosts::Manager {

// Owns the blocking manager ingress lifetime without constructing platform
// services. The composition root retains the instance lease outside this class
// so that it can release that lease after every injected owner has shut down.
class ManagerProcessHost final {
public:
    ManagerProcessHost(
        std::shared_ptr<Contracts::IManagerController> controller,
        std::shared_ptr<ForgeConductor::Manager::ManagerRequestDispatcher>
            dispatcher,
        std::unique_ptr<Contracts::IManagerServer> server);
    ~ManagerProcessHost() noexcept;

    ManagerProcessHost(const ManagerProcessHost&) = delete;
    ManagerProcessHost& operator=(const ManagerProcessHost&) = delete;
    ManagerProcessHost(ManagerProcessHost&&) = delete;
    ManagerProcessHost& operator=(ManagerProcessHost&&) = delete;

    [[nodiscard]] Domain::Result<void> run(
        const Domain::OperationContext& startupContext,
        const Domain::OperationContext& ingressContext) noexcept;

    // Idempotently closes ingress before draining the dispatcher. Dispatcher
    // shutdown closes the controller and therefore the runtime and store. If a
    // run is active, this call only signals cancellation; the run thread owns
    // final closure. The owner must keep this object alive until run returns.
    void shutdown() noexcept;

private:
    class RunLifetimeGuard final {
    public:
        explicit RunLifetimeGuard(ManagerProcessHost& owner) noexcept;
        ~RunLifetimeGuard() noexcept;

        RunLifetimeGuard(const RunLifetimeGuard&) = delete;
        RunLifetimeGuard& operator=(const RunLifetimeGuard&) = delete;

    private:
        ManagerProcessHost& owner_;
    };

    void completeRun() noexcept;
    void endRunFrame() noexcept;

    std::shared_ptr<Contracts::IManagerController> controller_;
    std::shared_ptr<ForgeConductor::Manager::ManagerRequestDispatcher>
        dispatcher_;
    std::unique_ptr<Contracts::IManagerServer> server_;

    std::mutex stateMutex_;
    std::stop_source startupStopSource_;
    bool runInvoked_{};
    bool runActive_{};
    bool shutdownClaimed_{};
    bool externalShutdownRequested_{};
    bool shutdownFinalizationClaimed_{};
};

} // namespace ForgeConductor::Hosts::Manager
