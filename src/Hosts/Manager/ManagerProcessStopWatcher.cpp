#include "ManagerProcessStopWatcher.h"

#include "ManagerProcessHost.h"
#include "ManagerProcessStopSignal.h"

#include <stdexcept>
#include <utility>

namespace ForgeConductor::Hosts::Manager {

ManagerProcessHostShutdownTarget::ManagerProcessHostShutdownTarget(
    std::shared_ptr<ManagerProcessHost> host)
    : host_{std::move(host)}
{
    if (!host_) {
        throw std::invalid_argument{
            "ManagerProcessHostShutdownTarget requires a process host."};
    }
}

void ManagerProcessHostShutdownTarget::shutdown() noexcept
{
    host_->shutdown();
}

ManagerProcessStopWatcher::ManagerProcessStopWatcher(
    ManagerProcessStopSignal& stopSignal,
    std::shared_ptr<IManagerProcessShutdownTarget> shutdownTarget)
    : stopSignal_{stopSignal}, shutdownTarget_{std::move(shutdownTarget)}
{
    if (!shutdownTarget_) {
        throw std::invalid_argument{
            "ManagerProcessStopWatcher requires a shutdown target."};
    }

    worker_ = std::jthread{
        [this](const std::stop_token cancellation) noexcept {
            try {
                const std::lock_guard lock{lifecycleMutex_};
                workerThreadId_ = std::this_thread::get_id();
            } catch (...) {
                return;
            }
            watch(cancellation);
        }};
}

ManagerProcessStopWatcher::~ManagerProcessStopWatcher() noexcept
{
    cancel();
}

void ManagerProcessStopWatcher::cancel() noexcept
{
    std::jthread claimedWorker;
    bool joinClaimPublished = false;
    try {
        std::unique_lock lock{lifecycleMutex_};
        if (workerThreadId_ == std::this_thread::get_id()) {
            // The shutdown target may cancel recursively on this worker. It
            // cannot join itself and must not wait for an external join owner,
            // because that owner is necessarily waiting for this callback to
            // return. A later process owner retains the sole exact-join edge.
            if (lifecycle_ == Lifecycle::Running) {
                lifecycle_ = Lifecycle::Stopping;
            }
            if (worker_.joinable()) {
                worker_.request_stop();
            }
            return;
        }

        while (lifecycle_ == Lifecycle::Stopping && cancelJoinOwned_) {
            lifecycleCondition_.wait(
                lock,
                [this]() noexcept { return !cancelJoinOwned_; });
        }
        if (lifecycle_ == Lifecycle::Stopped) {
            return;
        }

        lifecycle_ = Lifecycle::Stopping;
        if (worker_.joinable()) {
            worker_.request_stop();
            cancelJoinOwned_ = true;
            claimedWorker = std::move(worker_);
            joinClaimPublished = true;
        }
    } catch (...) {
        return;
    }

    if (joinClaimPublished) {
        lifecycleCondition_.notify_all();
    }

    // Keep a nested RAII owner until joining has completed. Stopped state is
    // never visible while the worker can still access the signal or target.
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
        cancelJoinOwned_ = false;
    } catch (...) {
        return;
    }
    lifecycleCondition_.notify_all();
}

void ManagerProcessStopWatcher::watch(
    const std::stop_token cancellation) noexcept
{
    if (stopSignal_.wait(cancellation)) {
        shutdownTarget_->shutdown();
    }
}

} // namespace ForgeConductor::Hosts::Manager
