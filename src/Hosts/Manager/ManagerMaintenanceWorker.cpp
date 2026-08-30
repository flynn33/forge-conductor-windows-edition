#include "ManagerMaintenanceWorker.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerMaintenanceService.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

[[nodiscard]] Domain::Error maintenanceError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

} // namespace

ManagerMaintenanceWorker::ManagerMaintenanceWorker(
    std::shared_ptr<Contracts::IManagerMaintenanceService> service,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    const ManagerMaintenanceWorkerTiming timing)
    : service_{std::move(service)},
      clock_{std::move(clock)},
      uuidGenerator_{std::move(uuidGenerator)},
      timing_{timing}
{
    if (!service_ || !clock_ || !uuidGenerator_) {
        throw std::invalid_argument{
            "ManagerMaintenanceWorker requires service, clock, and UUID dependencies."};
    }
    if (timing_.interval <= std::chrono::milliseconds::zero() ||
        timing_.interval > ManagerMaintenanceWorkerTiming::MaximumInterval ||
        timing_.operationTimeout <= std::chrono::milliseconds::zero() ||
        timing_.operationTimeout >
            ManagerMaintenanceWorkerTiming::MaximumOperationTimeout) {
        throw std::invalid_argument{
            "ManagerMaintenanceWorker timing values are outside their bounded ranges."};
    }
}

ManagerMaintenanceWorker::~ManagerMaintenanceWorker() noexcept
{
    shutdown();
}

Domain::Result<void> ManagerMaintenanceWorker::start() noexcept
{
    try {
        const std::lock_guard lock{lifecycleMutex_};
        if (lifecycle_ == Lifecycle::Running) {
            return Domain::Result<void>::failure(maintenanceError(
                Domain::ErrorCodes::Conflict,
                "The manager maintenance worker is already running.",
                true));
        }
        if (lifecycle_ == Lifecycle::Stopping ||
            lifecycle_ == Lifecycle::Stopped) {
            return Domain::Result<void>::failure(maintenanceError(
                Domain::ErrorCodes::TransportClosed,
                "The manager maintenance worker is shut down."));
        }

        worker_ = std::jthread{
            [this](const std::stop_token cancellation) noexcept {
                try {
                    const std::lock_guard lock{lifecycleMutex_};
                    workerThreadId_ = std::this_thread::get_id();
                } catch (...) {
                    return;
                }
                run(cancellation);
            }};
        lifecycle_ = Lifecycle::Running;
        return Domain::Result<void>::success();
    } catch (const std::exception& error) {
        return Domain::Result<void>::failure(maintenanceError(
            Domain::ErrorCodes::InternalFailure,
            std::string{"The manager maintenance worker could not start: "} +
                error.what(),
            true));
    } catch (...) {
        return Domain::Result<void>::failure(maintenanceError(
            Domain::ErrorCodes::InternalFailure,
            "The manager maintenance worker could not start.",
            true));
    }
}

void ManagerMaintenanceWorker::beginShutdown() noexcept
{
    bool wake = false;
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            if (lifecycle_ == Lifecycle::Stopped) {
                return;
            }
            lifecycle_ = Lifecycle::Stopping;
            if (worker_.joinable()) {
                worker_.request_stop();
                wake = true;
            }
        }
        if (wake) {
            waitCondition_.notify_all();
        }
    } catch (...) {
    }
}

void ManagerMaintenanceWorker::shutdown() noexcept
{
    std::jthread claimedWorker;
    bool wake = false;
    try {
        std::unique_lock lock{lifecycleMutex_};
        if (lifecycle_ == Lifecycle::Stopped) {
            return;
        }
        if (workerThreadId_ == std::this_thread::get_id()) {
            // A maintenance dependency can synchronously request process
            // shutdown from this worker. It cannot join itself or wait for an
            // external join owner that is necessarily waiting for this pass
            // to return. The later process owner retains the exact-join edge.
            if (lifecycle_ == Lifecycle::Running) {
                lifecycle_ = Lifecycle::Stopping;
            }
            if (worker_.joinable()) {
                worker_.request_stop();
            }
            return;
        }
        while (lifecycle_ == Lifecycle::Stopping && shutdownJoinOwned_) {
            lifecycleCondition_.wait(
                lock,
                [this]() noexcept { return !shutdownJoinOwned_; });
        }
        if (lifecycle_ == Lifecycle::Stopped) {
            return;
        }

        lifecycle_ = Lifecycle::Stopping;
        if (worker_.joinable()) {
            worker_.request_stop();
            wake = true;
            if (worker_.get_id() == std::this_thread::get_id()) {
                return;
            }
            shutdownJoinOwned_ = true;
            claimedWorker = std::move(worker_);
        }
    } catch (...) {
        return;
    }

    if (wake) {
        waitCondition_.notify_all();
    }

    {
        std::jthread joiningWorker = std::move(claimedWorker);
        if (joiningWorker.joinable()) {
            try {
                joiningWorker.join();
            } catch (...) {
                joiningWorker.request_stop();
            }
        }
    }

    try {
        const std::lock_guard lock{lifecycleMutex_};
        lifecycle_ = Lifecycle::Stopped;
        shutdownJoinOwned_ = false;
    } catch (...) {
        return;
    }
    lifecycleCondition_.notify_all();
}

Domain::Result<Domain::OperationContext>
ManagerMaintenanceWorker::makeContext(
    const std::stop_token cancellation) noexcept
{
    try {
        auto operationUuid = uuidGenerator_->next();
        if (!operationUuid) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(operationUuid).error());
        }
        auto correlationUuid = uuidGenerator_->next();
        if (!correlationUuid) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(correlationUuid).error());
        }

        auto correlation = Domain::CorrelationId::parse(
            correlationUuid.value().value());
        if (!correlation) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(correlation).error());
        }

        return Domain::Result<Domain::OperationContext>::success(
            Domain::OperationContext{
                Domain::OperationId{std::move(operationUuid).value()},
                clock_->monotonicNow() + timing_.operationTimeout,
                cancellation,
                std::move(correlation).value()});
    } catch (...) {
        return Domain::Result<Domain::OperationContext>::failure(
            maintenanceError(
                Domain::ErrorCodes::InternalFailure,
                "The manager maintenance worker could not create an operation context."));
    }
}

void ManagerMaintenanceWorker::run(
    const std::stop_token cancellation) noexcept
{
    try {
        while (!cancellation.stop_requested()) {
            if (auto context = makeContext(cancellation); context) {
                auto pass = service_->reconcile(context.value());
                static_cast<void>(pass);
            }

            if (cancellation.stop_requested()) {
                return;
            }
            std::unique_lock waitLock{waitMutex_};
            static_cast<void>(waitCondition_.wait_for(
                waitLock,
                cancellation,
                timing_.interval,
                []() noexcept { return false; }));
        }
    } catch (...) {
        // The process thread cannot propagate an exception. Typed maintenance
        // failures are deliberately deferred until the next scheduled pass.
    }
}

} // namespace ForgeConductor::Hosts::Manager
