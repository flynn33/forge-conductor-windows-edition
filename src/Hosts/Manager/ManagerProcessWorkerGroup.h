#pragma once

#include "ManagerTransitionWorker.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace ForgeConductor::Hosts::Manager {

// Bounded composite used where ManagerProcessHost needs multiple independent
// process workers without learning another concrete lifecycle special case.
class ManagerProcessWorkerGroup final : public IManagerTransitionWorker {
public:
    static constexpr std::size_t MaximumWorkers = 8U;

    explicit ManagerProcessWorkerGroup(
        std::vector<std::unique_ptr<IManagerTransitionWorker>> workers);
    ~ManagerProcessWorkerGroup() noexcept override;

    ManagerProcessWorkerGroup(const ManagerProcessWorkerGroup&) = delete;
    ManagerProcessWorkerGroup& operator=(const ManagerProcessWorkerGroup&) =
        delete;
    ManagerProcessWorkerGroup(ManagerProcessWorkerGroup&&) = delete;
    ManagerProcessWorkerGroup& operator=(ManagerProcessWorkerGroup&&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept override;
    void beginShutdown() noexcept override;
    void shutdown() noexcept override;

private:
    enum class Lifecycle {
        Constructed,
        Starting,
        Running,
        Stopping,
        Stopped,
    };

    [[nodiscard]] bool stopRequested() noexcept;
    void signalShutdownPrefix(std::size_t workerCount) noexcept;
    void finalizeFailedStart() noexcept;
    void publishStopped() noexcept;
    void shutdownPrefix(std::size_t workerCount) noexcept;

    std::vector<std::unique_ptr<IManagerTransitionWorker>> workers_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    Lifecycle lifecycle_{Lifecycle::Constructed};
    std::size_t startedCount_{};
    std::size_t shutdownCount_{};
    bool startActive_{};
    bool stopRequested_{};
    bool beginSignalActive_{};
    bool shutdownOwnerActive_{};
};

} // namespace ForgeConductor::Hosts::Manager
