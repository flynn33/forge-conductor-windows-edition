#pragma once

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
class IManagerController;
class IUuidGenerator;
}

namespace ForgeConductor::Hosts::Manager {

class ManagerProcessRestartSignal;

// Process-host lifecycle boundary for the sole serialized transition owner.
// The host starts it only after controller initialization and closes it before
// controller or runtime destruction. beginShutdown only closes successor
// admission and requests cancellation; shutdown retains the exact worker join.
class IManagerTransitionWorker {
public:
    virtual ~IManagerTransitionWorker() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> start() noexcept = 0;
    virtual void beginShutdown() noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

struct ManagerTransitionWorkerTiming final {
    // Test seam: production maps one configured watchdog second to one second.
    std::chrono::milliseconds watchdogSecond{std::chrono::seconds{1}};
    std::chrono::milliseconds operationTimeout{std::chrono::seconds{5}};

    static constexpr std::chrono::milliseconds MaximumWatchdogSecond{
        std::chrono::seconds{1}};
    static constexpr std::chrono::milliseconds MaximumOperationTimeout{
        std::chrono::seconds{60}};
};

// Process-owned capacity-one executor for deferred operator restarts and
// watchdog reconciliation. Construction does not start the worker; the
// process host calls start only after controller initialization.
class ManagerTransitionWorker final : public IManagerTransitionWorker {
public:
    ManagerTransitionWorker(
        std::shared_ptr<Contracts::IManagerController> controller,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        ManagerProcessRestartSignal& restartSignal,
        ManagerTransitionWorkerTiming timing = {});
    ~ManagerTransitionWorker() noexcept override;

    ManagerTransitionWorker(const ManagerTransitionWorker&) = delete;
    ManagerTransitionWorker& operator=(const ManagerTransitionWorker&) = delete;
    ManagerTransitionWorker(ManagerTransitionWorker&&) = delete;
    ManagerTransitionWorker& operator=(ManagerTransitionWorker&&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept override;
    void beginShutdown() noexcept override;
    void shutdown() noexcept override;

private:
    enum class Lifecycle {
        Constructed,
        Running,
        Stopping,
        Stopped,
    };

    [[nodiscard]] Domain::Result<Domain::OperationContext> makeContext(
        std::stop_token cancellation) noexcept;
    void run(std::stop_token cancellation) noexcept;

    std::shared_ptr<Contracts::IManagerController> controller_;
    std::shared_ptr<Contracts::IClock> clock_;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    ManagerProcessRestartSignal& restartSignal_;
    const ManagerTransitionWorkerTiming timing_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    Lifecycle lifecycle_{Lifecycle::Constructed};
    bool shutdownJoinOwned_{};
    std::jthread worker_;
};

} // namespace ForgeConductor::Hosts::Manager
