#include "ManagerProcessWorkerGroup.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

[[nodiscard]] Domain::Error groupError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

} // namespace

ManagerProcessWorkerGroup::ManagerProcessWorkerGroup(
    std::vector<std::unique_ptr<IManagerTransitionWorker>> workers)
    : workers_{std::move(workers)}
{
    if (workers_.empty() || workers_.size() > MaximumWorkers) {
        throw std::invalid_argument{
            "ManagerProcessWorkerGroup requires a bounded non-empty worker set."};
    }
    for (const auto& worker : workers_) {
        if (!worker) {
            throw std::invalid_argument{
                "ManagerProcessWorkerGroup does not accept null workers."};
        }
    }
}

ManagerProcessWorkerGroup::~ManagerProcessWorkerGroup() noexcept
{
    shutdown();
}

Domain::Result<void> ManagerProcessWorkerGroup::start() noexcept
{
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            if (lifecycle_ == Lifecycle::Starting ||
                lifecycle_ == Lifecycle::Running) {
                return Domain::Result<void>::failure(groupError(
                    Domain::ErrorCodes::Conflict,
                    "The manager process worker group is already running.",
                    true));
            }
            if (lifecycle_ == Lifecycle::Stopping ||
                lifecycle_ == Lifecycle::Stopped) {
                return Domain::Result<void>::failure(groupError(
                    Domain::ErrorCodes::TransportClosed,
                    "The manager process worker group is shut down."));
            }
            lifecycle_ = Lifecycle::Starting;
            startActive_ = true;
        }

        std::optional<Domain::Error> failure;
        std::size_t started = 0U;
        for (std::size_t index = 0U; index < workers_.size(); ++index) {
            if (stopRequested()) {
                failure = groupError(
                    Domain::ErrorCodes::TransportClosed,
                    "The manager process worker group was stopped during startup.");
                break;
            }

            auto result = workers_[index]->start();
            if (!result) {
                failure = std::move(result).error();
                break;
            }
            ++started;
            {
                const std::lock_guard lock{lifecycleMutex_};
                startedCount_ = started;
            }
        }

        {
            const std::lock_guard lock{lifecycleMutex_};
            if (!failure && stopRequested_) {
                failure = groupError(
                    Domain::ErrorCodes::TransportClosed,
                    "The manager process worker group was stopped during startup.");
            }
            if (!failure) {
                lifecycle_ = Lifecycle::Running;
                startActive_ = false;
            }
        }
        if (!failure) {
            lifecycleCondition_.notify_all();
            return Domain::Result<void>::success();
        }

        signalShutdownPrefix(started);
        finalizeFailedStart();
        return Domain::Result<void>::failure(std::move(*failure));
    } catch (...) {
        signalShutdownPrefix(startedCount_);
        finalizeFailedStart();
        return Domain::Result<void>::failure(groupError(
            Domain::ErrorCodes::InternalFailure,
            "The manager process worker group could not start.",
            true));
    }
}

void ManagerProcessWorkerGroup::beginShutdown() noexcept
{
    signalShutdownPrefix(workers_.size());
}

void ManagerProcessWorkerGroup::shutdown() noexcept
{
    signalShutdownPrefix(workers_.size());

    std::size_t workerCount = 0U;
    try {
        std::unique_lock lock{lifecycleMutex_};
        lifecycleCondition_.wait(lock, [this]() noexcept {
            return !startActive_ && !beginSignalActive_ &&
                !shutdownOwnerActive_;
        });
        if (lifecycle_ == Lifecycle::Stopped) {
            return;
        }
        shutdownOwnerActive_ = true;
        workerCount = shutdownCount_;
    } catch (...) {
        return;
    }

    shutdownPrefix(workerCount);
    publishStopped();
}

bool ManagerProcessWorkerGroup::stopRequested() noexcept
{
    try {
        const std::lock_guard lock{lifecycleMutex_};
        return stopRequested_;
    } catch (...) {
        return true;
    }
}

void ManagerProcessWorkerGroup::signalShutdownPrefix(
    const std::size_t workerCount) noexcept
{
    bool signalOwner = false;
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            if (lifecycle_ == Lifecycle::Stopped || stopRequested_) {
                return;
            }
            stopRequested_ = true;
            lifecycle_ = Lifecycle::Stopping;
            beginSignalActive_ = true;
            shutdownCount_ = workerCount;
            signalOwner = true;
        }

        if (signalOwner) {
            for (std::size_t index = workerCount; index > 0U; --index) {
                workers_[index - 1U]->beginShutdown();
            }
        }
    } catch (...) {
    }

    if (!signalOwner) {
        return;
    }
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            beginSignalActive_ = false;
        }
        lifecycleCondition_.notify_all();
    } catch (...) {
    }
}

void ManagerProcessWorkerGroup::finalizeFailedStart() noexcept
{
    std::size_t workerCount = 0U;
    try {
        std::unique_lock lock{lifecycleMutex_};
        lifecycleCondition_.wait(
            lock,
            [this]() noexcept { return !beginSignalActive_; });
        shutdownOwnerActive_ = true;
        workerCount = shutdownCount_;
    } catch (...) {
        return;
    }

    shutdownPrefix(workerCount);
    publishStopped();
}

void ManagerProcessWorkerGroup::publishStopped() noexcept
{
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            lifecycle_ = Lifecycle::Stopped;
            startedCount_ = 0U;
            startActive_ = false;
            beginSignalActive_ = false;
            shutdownOwnerActive_ = false;
        }
        lifecycleCondition_.notify_all();
    } catch (...) {
    }
}

void ManagerProcessWorkerGroup::shutdownPrefix(
    const std::size_t workerCount) noexcept
{
    for (std::size_t index = workerCount; index > 0U; --index) {
        workers_[index - 1U]->shutdown();
    }
}

} // namespace ForgeConductor::Hosts::Manager
