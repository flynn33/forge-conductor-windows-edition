#include "ManagerTransitionWorker.h"

#include "ManagerProcessRestartSignal.h"
#include "ManagerWatchdogPolicy.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerRuntime.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

[[nodiscard]] Domain::Error workerError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

class RestartCompletion final {
public:
    explicit RestartCompletion(ManagerProcessRestartSignal& signal) noexcept
        : signal_{signal}
    {
    }

    ~RestartCompletion() noexcept
    {
        static_cast<void>(signal_.completeRestart());
    }

    RestartCompletion(const RestartCompletion&) = delete;
    RestartCompletion& operator=(const RestartCompletion&) = delete;

private:
    ManagerProcessRestartSignal& signal_;
};

} // namespace

ManagerTransitionWorker::ManagerTransitionWorker(
    std::shared_ptr<Contracts::IManagerController> controller,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    ManagerProcessRestartSignal& restartSignal,
    const ManagerTransitionWorkerTiming timing)
    : controller_{std::move(controller)},
      clock_{std::move(clock)},
      uuidGenerator_{std::move(uuidGenerator)},
      restartSignal_{restartSignal},
      timing_{timing}
{
    if (!controller_ || !clock_ || !uuidGenerator_) {
        throw std::invalid_argument(
            "ManagerTransitionWorker requires controller, clock, and UUID dependencies.");
    }
    if (timing_.watchdogSecond <= std::chrono::milliseconds::zero() ||
        timing_.watchdogSecond >
            ManagerTransitionWorkerTiming::MaximumWatchdogSecond ||
        timing_.operationTimeout <= std::chrono::milliseconds::zero() ||
        timing_.operationTimeout >
            ManagerTransitionWorkerTiming::MaximumOperationTimeout) {
        throw std::invalid_argument(
            "ManagerTransitionWorker timing values are outside their bounded ranges.");
    }
}

ManagerTransitionWorker::~ManagerTransitionWorker() noexcept
{
    shutdown();
}

Domain::Result<void> ManagerTransitionWorker::start() noexcept
{
    try {
        std::lock_guard lock{lifecycleMutex_};
        if (lifecycle_ == Lifecycle::Running) {
            return Domain::Result<void>::failure(workerError(
                Domain::ErrorCodes::Conflict,
                "The manager transition worker is already running.",
                true));
        }
        if (lifecycle_ == Lifecycle::Stopping ||
            lifecycle_ == Lifecycle::Stopped) {
            return Domain::Result<void>::failure(workerError(
                Domain::ErrorCodes::TransportClosed,
                "The manager transition worker is shut down."));
        }

        worker_ = std::jthread{
            [this](const std::stop_token cancellation) noexcept {
                run(cancellation);
            }};
        lifecycle_ = Lifecycle::Running;
        return Domain::Result<void>::success();
    } catch (const std::exception& error) {
        return Domain::Result<void>::failure(workerError(
            Domain::ErrorCodes::InternalFailure,
            std::string{"The manager transition worker could not start: "} +
                error.what(),
            true));
    } catch (...) {
        return Domain::Result<void>::failure(workerError(
            Domain::ErrorCodes::InternalFailure,
            "The manager transition worker could not start.",
            true));
    }
}

void ManagerTransitionWorker::shutdown() noexcept
{
    std::jthread claimedWorker;
    try {
        std::unique_lock lock{lifecycleMutex_};
        while (lifecycle_ == Lifecycle::Stopping && shutdownJoinOwned_) {
            lifecycleCondition_.wait(
                lock,
                [this]() noexcept { return !shutdownJoinOwned_; });
        }
        if (lifecycle_ == Lifecycle::Stopped) {
            return;
        }

        lifecycle_ = Lifecycle::Stopping;
        restartSignal_.close();
        if (worker_.joinable()) {
            worker_.request_stop();
            if (worker_.get_id() == std::this_thread::get_id()) {
                // A dependency callback may request stop, but only a distinct
                // process owner is allowed to claim and join this worker.
                return;
            }
            shutdownJoinOwned_ = true;
            claimedWorker = std::move(worker_);
        }
    } catch (...) {
        return;
    }

    // The nested owner is destroyed before Stopped is published. If join
    // unexpectedly reports a platform error, jthread's RAII destructor still
    // retains ownership and no stopped state is visible while it can execute.
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
        std::lock_guard lock{lifecycleMutex_};
        lifecycle_ = Lifecycle::Stopped;
        shutdownJoinOwned_ = false;
    } catch (...) {
        return;
    }
    lifecycleCondition_.notify_all();
}

Domain::Result<Domain::OperationContext> ManagerTransitionWorker::makeContext(
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
        return Domain::Result<Domain::OperationContext>::failure(workerError(
            Domain::ErrorCodes::InternalFailure,
            "The manager transition worker could not create an operation context."));
    }
}

void ManagerTransitionWorker::run(
    const std::stop_token cancellation) noexcept
{
    try {
        auto watchdogInterval = ManagerWatchdogPolicy::DefaultInterval;

        // Initialization is owned by the composition root. This first bounded
        // observation only seeds the configured cadence after initialization.
        if (auto context = makeContext(cancellation); context) {
            auto initial = controller_->snapshot(context.value());
            if (initial) {
                watchdogInterval =
                    ManagerWatchdogPolicy::normalizedInterval(initial.value());
            }
        }

        while (!cancellation.stop_requested()) {
            const auto watchdogDeadline = clock_->monotonicNow() +
                timing_.watchdogSecond * watchdogInterval.count();
            const auto wake = restartSignal_.waitAndBeginUntil(
                cancellation, watchdogDeadline);

            if (wake == ManagerProcessRestartWaitResult::Cancelled ||
                wake == ManagerProcessRestartWaitResult::Closed) {
                return;
            }

            if (wake == ManagerProcessRestartWaitResult::RestartRequested) {
                RestartCompletion completion{restartSignal_};
                if (cancellation.stop_requested()) {
                    continue;
                }
                auto context = makeContext(cancellation);
                if (!context) {
                    continue;
                }
                auto restarted = controller_->control(
                    {Domain::ManagerControlAction::Restart}, context.value());
                if (restarted) {
                    watchdogInterval =
                        ManagerWatchdogPolicy::normalizedInterval(
                            restarted.value());
                }
                continue;
            }

            if (cancellation.stop_requested()) {
                return;
            }
            auto observationContext = makeContext(cancellation);
            if (!observationContext) {
                continue;
            }
            auto observed = controller_->snapshot(observationContext.value());
            if (!observed) {
                continue;
            }
            watchdogInterval =
                ManagerWatchdogPolicy::normalizedInterval(observed.value());
            if (ManagerWatchdogPolicy::decide(observed.value()) !=
                    ManagerWatchdogAction::Repair ||
                cancellation.stop_requested()) {
                continue;
            }

            auto repairContext = makeContext(cancellation);
            if (!repairContext) {
                continue;
            }
            auto repaired = controller_->control(
                {Domain::ManagerControlAction::Repair}, repairContext.value());
            if (repaired) {
                watchdogInterval =
                    ManagerWatchdogPolicy::normalizedInterval(
                        repaired.value());
            }
        }
    } catch (...) {
        // This process-owned thread cannot propagate an exception. Controller
        // calls expose typed failures and the destructor still owns shutdown.
    }
}

} // namespace ForgeConductor::Hosts::Manager
