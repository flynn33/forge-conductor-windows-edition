#include "ManagerProcessHost.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

[[nodiscard]] Domain::Error hostError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

} // namespace

ManagerProcessHost::ManagerProcessHost(
    std::shared_ptr<Contracts::IManagerController> controller,
    std::shared_ptr<ForgeConductor::Manager::ManagerRequestDispatcher>
        dispatcher,
    std::unique_ptr<Contracts::IManagerServer> server)
    : controller_{std::move(controller)},
      dispatcher_{std::move(dispatcher)},
      server_{std::move(server)}
{
    if (!controller_ || !dispatcher_ || !server_) {
        throw std::invalid_argument{
            "The manager process host requires a controller, dispatcher, and server."};
    }
}

ManagerProcessHost::~ManagerProcessHost() noexcept
{
    shutdown();
    try {
        const std::lock_guard lock{stateMutex_};
        if (runActive_) {
            // Destruction concurrent with the caller-owned blocking run would
            // otherwise release the server and dispatcher underneath it.
            std::terminate();
        }
    } catch (...) {
        std::terminate();
    }
}

ManagerProcessHost::RunLifetimeGuard::RunLifetimeGuard(
    ManagerProcessHost& owner) noexcept
    : owner_{owner}
{
}

ManagerProcessHost::RunLifetimeGuard::~RunLifetimeGuard() noexcept
{
    owner_.endRunFrame();
}

Domain::Result<void> ManagerProcessHost::run(
    const Domain::OperationContext& startupContext,
    const Domain::OperationContext& ingressContext) noexcept
{
    // Declared before the exception boundary and every other run-frame owner so
    // runActive_ remains true through callback destruction and catch cleanup.
    std::optional<RunLifetimeGuard> runLifetime;
    try {
        {
            const std::lock_guard lock{stateMutex_};
            if (runInvoked_) {
                return Domain::Result<void>::failure(hostError(
                    Domain::ErrorCodes::Conflict,
                    "The manager process host may run only once."));
            }
            if (shutdownClaimed_) {
                return Domain::Result<void>::failure(hostError(
                    Domain::ErrorCodes::TransportClosed,
                    "The manager process host is shut down."));
            }
            runInvoked_ = true;
            runActive_ = true;
            runLifetime.emplace(*this);
        }

        const std::stop_callback callerCancellation{
            startupContext.cancellation,
            [this]() noexcept { startupStopSource_.request_stop(); }};
        const Domain::OperationContext ownedStartupContext{
            startupContext.operationId,
            startupContext.deadline,
            startupStopSource_.get_token(),
            startupContext.correlationId};

        auto initialized = controller_->initialize(ownedStartupContext);
        if (!initialized) {
            auto error = std::move(initialized).error();
            completeRun();
            return Domain::Result<void>::failure(std::move(error));
        }

        bool stopBeforeIngress = false;
        {
            const std::lock_guard lock{stateMutex_};
            stopBeforeIngress = shutdownClaimed_;
        }
        if (stopBeforeIngress) {
            completeRun();
            return Domain::Result<void>::success();
        }

        auto served = server_->run(ingressContext);
        bool externalShutdownRequested = false;
        {
            const std::lock_guard lock{stateMutex_};
            externalShutdownRequested = externalShutdownRequested_;
        }
        if (!served) {
            auto error = std::move(served).error();
            completeRun();
            if (externalShutdownRequested &&
                error.code == Domain::ErrorCodes::TransportClosed) {
                return Domain::Result<void>::success();
            }
            return Domain::Result<void>::failure(std::move(error));
        }
        completeRun();
        return Domain::Result<void>::success();
    } catch (...) {
        completeRun();
        return Domain::Result<void>::failure(hostError(
            Domain::ErrorCodes::InternalFailure,
            "The manager process host failed at its application boundary."));
    }
}

void ManagerProcessHost::completeRun() noexcept
{
    try {
        startupStopSource_.request_stop();
        bool finalize = false;
        {
            const std::lock_guard lock{stateMutex_};
            shutdownClaimed_ = true;
            if (!shutdownFinalizationClaimed_) {
                shutdownFinalizationClaimed_ = true;
                finalize = true;
            }
        }
        if (finalize) {
            server_->shutdown();
            dispatcher_->shutdown();
        }
    } catch (...) {
    }
}

void ManagerProcessHost::endRunFrame() noexcept
{
    try {
        const std::lock_guard lock{stateMutex_};
        runActive_ = false;
    } catch (...) {
        std::terminate();
    }
}

void ManagerProcessHost::shutdown() noexcept
{
    try {
        startupStopSource_.request_stop();
        bool signal = false;
        bool finalize = false;
        {
            const std::lock_guard lock{stateMutex_};
            if (!shutdownClaimed_) {
                shutdownClaimed_ = true;
                signal = true;
            }
            externalShutdownRequested_ = true;
            if (!runActive_ && !shutdownFinalizationClaimed_) {
                shutdownFinalizationClaimed_ = true;
                finalize = true;
            }
        }

        if (!signal && !finalize) {
            return;
        }

        // Do not reset either owner here: run() may still be unwinding on a
        // different thread. Active startup is cancelled and final controller
        // closure is deferred to that run thread so shutdown itself stays
        // bounded even if an injected startup callback is non-cooperative.
        server_->shutdown();
        if (signal) {
            dispatcher_->beginShutdown();
        }
        if (finalize) {
            dispatcher_->shutdown();
        }
    } catch (...) {
        // This is a noexcept process boundary. Injected shutdown contracts are
        // noexcept; retain process teardown if an implementation violates one.
    }
}

} // namespace ForgeConductor::Hosts::Manager
