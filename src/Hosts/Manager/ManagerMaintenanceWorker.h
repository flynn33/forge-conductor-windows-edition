#pragma once

#include "ManagerTransitionWorker.h"

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ForgeConductor::Contracts {
class IClock;
class IManagerMaintenanceService;
class IUuidGenerator;
}

namespace ForgeConductor::Hosts::Manager {

namespace Detail {
struct ManagerMaintenanceWorkerTestAccess;
}

struct ManagerMaintenanceWorkerTiming final {
    std::chrono::milliseconds interval{std::chrono::minutes{1}};
    std::chrono::milliseconds operationTimeout{std::chrono::seconds{30}};

    static constexpr std::chrono::milliseconds MaximumInterval{
        std::chrono::hours{24}};
    static constexpr std::chrono::milliseconds MaximumOperationTimeout{
        std::chrono::minutes{5}};
};

// Capacity-one process owner for periodic maintenance. The first pass is
// immediate; every later pass waits one full interval through a cancellable
// event boundary. A sole worker means passes cannot queue, overlap, or build a
// retry backlog when a typed pass failure is returned.
class ManagerMaintenanceWorker final : public IManagerTransitionWorker {
public:
    ManagerMaintenanceWorker(
        std::shared_ptr<Contracts::IManagerMaintenanceService> service,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        ManagerMaintenanceWorkerTiming timing = {});
    ~ManagerMaintenanceWorker() noexcept override;

    ManagerMaintenanceWorker(const ManagerMaintenanceWorker&) = delete;
    ManagerMaintenanceWorker& operator=(const ManagerMaintenanceWorker&) =
        delete;
    ManagerMaintenanceWorker(ManagerMaintenanceWorker&&) = delete;
    ManagerMaintenanceWorker& operator=(ManagerMaintenanceWorker&&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept override;
    void beginShutdown() noexcept override;
    void shutdown() noexcept override;

private:
    friend struct Detail::ManagerMaintenanceWorkerTestAccess;

    enum class Lifecycle {
        Constructed,
        Running,
        Stopping,
        Stopped,
    };

    [[nodiscard]] Domain::Result<Domain::OperationContext> makeContext(
        std::stop_token cancellation) noexcept;
    void run(std::stop_token cancellation) noexcept;

    std::shared_ptr<Contracts::IManagerMaintenanceService> service_;
    std::shared_ptr<Contracts::IClock> clock_;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    const ManagerMaintenanceWorkerTiming timing_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    Lifecycle lifecycle_{Lifecycle::Constructed};
    bool shutdownJoinOwned_{};
    std::thread::id workerThreadId_{};
    std::jthread worker_;

    std::mutex waitMutex_;
    std::condition_variable_any waitCondition_;
};

} // namespace ForgeConductor::Hosts::Manager
