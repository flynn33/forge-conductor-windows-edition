#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ForgeConductor::Hosts::Manager {

namespace Detail {
struct ManagerProcessStopWatcherTestAccess;
}

class ManagerProcessHost;
class ManagerProcessStopSignal;

// Narrow process-owner edge used by the stop watcher. Keeping this boundary
// independent of ManagerProcessHost makes the event-driven worker directly
// testable and prevents it from owning the rest of process composition.
class IManagerProcessShutdownTarget {
public:
    virtual ~IManagerProcessShutdownTarget() noexcept = default;

    virtual void shutdown() noexcept = 0;
};

// Production adapter that retains the process host through the shutdown
// callback. The watcher can therefore exact-join its worker before this final
// strong host owner is released.
class ManagerProcessHostShutdownTarget final
    : public IManagerProcessShutdownTarget {
public:
    explicit ManagerProcessHostShutdownTarget(
        std::shared_ptr<ManagerProcessHost> host);
    ~ManagerProcessHostShutdownTarget() noexcept override = default;

    ManagerProcessHostShutdownTarget(
        const ManagerProcessHostShutdownTarget&) = delete;
    ManagerProcessHostShutdownTarget& operator=(
        const ManagerProcessHostShutdownTarget&) = delete;
    ManagerProcessHostShutdownTarget(ManagerProcessHostShutdownTarget&&) =
        delete;
    ManagerProcessHostShutdownTarget& operator=(
        ManagerProcessHostShutdownTarget&&) = delete;

    void shutdown() noexcept override;

private:
    std::shared_ptr<ManagerProcessHost> host_;
};

// Process-owned one-shot watcher for dashboard and pipe stop requests. It owns
// exactly one worker, waits on ManagerProcessStopSignal without polling, and
// delegates host shutdown through the injected narrow target. The referenced
// stop signal must outlive this watcher; the shutdown target is retained until
// watcher destruction.
class ManagerProcessStopWatcher final {
public:
    ManagerProcessStopWatcher(
        ManagerProcessStopSignal& stopSignal,
        std::shared_ptr<IManagerProcessShutdownTarget> shutdownTarget);
    ~ManagerProcessStopWatcher() noexcept;

    ManagerProcessStopWatcher(const ManagerProcessStopWatcher&) = delete;
    ManagerProcessStopWatcher& operator=(const ManagerProcessStopWatcher&) =
        delete;
    ManagerProcessStopWatcher(ManagerProcessStopWatcher&&) = delete;
    ManagerProcessStopWatcher& operator=(ManagerProcessStopWatcher&&) = delete;

    // Idempotently cancels the wait and exact-joins the sole worker. Concurrent
    // callers return only after the join owner has published stopped state.
    void cancel() noexcept;

private:
    friend struct Detail::ManagerProcessStopWatcherTestAccess;

    enum class Lifecycle {
        Running,
        Stopping,
        Stopped,
    };

    void watch(std::stop_token cancellation) noexcept;

    ManagerProcessStopSignal& stopSignal_;
    std::shared_ptr<IManagerProcessShutdownTarget> shutdownTarget_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    Lifecycle lifecycle_{Lifecycle::Running};
    bool cancelJoinOwned_{};
    std::thread::id workerThreadId_{};
    std::jthread worker_;
};

} // namespace ForgeConductor::Hosts::Manager
