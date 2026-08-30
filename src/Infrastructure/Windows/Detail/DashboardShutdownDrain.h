#pragma once

#include "DashboardConnectionRegistry.h"
#include "DashboardDeadlineIocpBridge.h"
#include "DashboardIocpCompletionRouter.h"
#include "DashboardIocpWorkerKernel.h"
#include "DashboardListenerGenerationCoordinator.h"
#include "DashboardOverloadResponderSet.h"

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardShutdownDrainLifecycle : std::uint8_t {
    Registered,
    Arming,
    Draining,
    HardEscalated,
    RoutingReleasePending,
    Drained,
    Fatal,
};

class DashboardShutdownDrainHostSnapshot final {
public:
    DashboardShutdownDrainHostSnapshot(
        bool dependenciesAvailable,
        bool executorFullyDrained,
        bool listenerRoutingDrained,
        bool overloadFullyDrained,
        std::size_t registeredConnectionCount,
        std::size_t registeredAuxiliaryDeadlineTargetCount,
        std::size_t fixedCompletionTargetCount,
        std::uint64_t routingProgressRevision,
        bool deadlineRoutingInProgress,
        bool fatal) noexcept;

    [[nodiscard]] bool dependenciesAvailable() const noexcept;
    [[nodiscard]] bool executorFullyDrained() const noexcept;
    [[nodiscard]] bool listenerRoutingDrained() const noexcept;
    [[nodiscard]] bool overloadFullyDrained() const noexcept;
    [[nodiscard]] std::size_t registeredConnectionCount() const noexcept;
    [[nodiscard]] std::size_t registeredAuxiliaryDeadlineTargetCount()
        const noexcept;
    [[nodiscard]] std::size_t fixedCompletionTargetCount() const noexcept;
    [[nodiscard]] std::uint64_t routingProgressRevision() const noexcept;
    [[nodiscard]] bool deadlineRoutingInProgress() const noexcept;
    [[nodiscard]] bool fatal() const noexcept;

private:
    bool dependenciesAvailable_{};
    bool executorFullyDrained_{};
    bool listenerRoutingDrained_{};
    bool overloadFullyDrained_{};
    std::size_t registeredConnectionCount_{};
    std::size_t registeredAuxiliaryDeadlineTargetCount_{};
    std::size_t fixedCompletionTargetCount_{};
    std::uint64_t routingProgressRevision_{};
    bool deadlineRoutingInProgress_{};
    bool fatal_{};
};

class IDashboardShutdownDrainFailFast {
public:
    virtual ~IDashboardShutdownDrainFailFast() noexcept = default;
    virtual void failFast() noexcept = 0;
};

class DashboardShutdownDrainProcessFailFast final
    : public IDashboardShutdownDrainFailFast {
public:
    void failFast() noexcept override;
};

// One process-lifecycle port keeps the shutdown state machine independent of
// concrete Windows owners. Every method is either nonblocking or an explicitly
// named final join. Implementations may not destroy a dependency from inside
// an observer callback.
class IDashboardShutdownDrainHost {
public:
    virtual ~IDashboardShutdownDrainHost() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> bindBridgeFailureObserver(
        std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void>
    bindDeadlineSchedulerFailureObserver(
        std::weak_ptr<
            IWindowsDashboardDeadlineSchedulerFailureObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> bindHandlerDrainObserver(
        std::weak_ptr<IDashboardHandlerExecutorDrainObserver>
            observer) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> bindListenerDrainObserver(
        std::weak_ptr<
            IDashboardListenerGenerationCoordinatorDrainObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void>
    bindRegistryConnectionDrainObserver(
        std::weak_ptr<IDashboardConnectionRegistryDrainObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void>
    bindRegistryRoutingProgressObserver(
        std::weak_ptr<IDashboardConnectionRegistryRoutingProgressObserver>
            observer) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<IDashboardOverloadResponderSetDrainObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void>
    registerShutdownDeadlineTarget(
        std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> target)
        noexcept = 0;

    [[nodiscard]] virtual Domain::MonotonicTimePoint monotonicNow()
        const noexcept = 0;
    [[nodiscard]] virtual Domain::Result<WindowsDashboardDeadline>
    scheduleShutdownDeadline(WindowsDashboardDeadlineRequest request)
        noexcept = 0;
    [[nodiscard]] virtual bool cancelShutdownDeadline(
        std::uint64_t registrationId,
        std::uint64_t armSequence) noexcept = 0;

    virtual void closeRuntimeAdmission() noexcept = 0;
    virtual void closeHandlerAdmission() noexcept = 0;
    virtual void beginGracefulListenerShutdown() noexcept = 0;
    virtual void beginOverloadShutdown() noexcept = 0;
    virtual void beginGracefulRegistryShutdown() noexcept = 0;
    virtual void beginHardListenerShutdown() noexcept = 0;
    virtual void beginHardRegistryShutdown() noexcept = 0;
    virtual void beginCompletionRouterShutdown() noexcept = 0;
    virtual void joinHandlerExecutor() noexcept = 0;

    [[nodiscard]] virtual DashboardShutdownDrainHostSnapshot snapshot()
        const noexcept = 0;
    [[nodiscard]] virtual bool unregisterOverloadDeadlineTarget()
        noexcept = 0;
    [[nodiscard]] virtual bool unregisterOverloadCompletionTarget()
        noexcept = 0;
    [[nodiscard]] virtual bool unregisterShutdownDeadlineTarget(
        const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
        noexcept = 0;
    virtual void shutdownDeadlineScheduler() noexcept = 0;
    [[nodiscard]] virtual Domain::Result<
        DashboardDeadlineRoutingFinalizeDisposition>
    finalizeDeadlineRouting() noexcept = 0;
    virtual void shutdownIocpKernel() noexcept = 0;
};

// Production adapter. Unique process owners are borrowed. Shared graph owners
// are weakly retained so the registry's strong shutdown-target registration
// cannot form a cycle back through this host. The composition root must retain
// every dependency until DashboardShutdownDrain::wait() returns.
class DashboardShutdownDrainHost final
    : public IDashboardShutdownDrainHost {
public:
    DashboardShutdownDrainHost(
        DashboardConnectionRuntimeServices& runtimeServices,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardIocpWorkerKernel& kernel,
        std::shared_ptr<DashboardDeadlineIocpBridge> deadlineBridge,
        std::shared_ptr<DashboardListenerGenerationCoordinator>
            listenerCoordinator,
        std::shared_ptr<DashboardOverloadResponderSet> overloadResponders,
        std::shared_ptr<DashboardConnectionRegistry> connectionRegistry,
        std::shared_ptr<DashboardIocpCompletionRouter> completionRouter)
        noexcept;

    [[nodiscard]] Domain::Result<void> bindBridgeFailureObserver(
        std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void>
    bindDeadlineSchedulerFailureObserver(
        std::weak_ptr<
            IWindowsDashboardDeadlineSchedulerFailureObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void> bindHandlerDrainObserver(
        std::weak_ptr<IDashboardHandlerExecutorDrainObserver>
            observer) noexcept override;
    [[nodiscard]] Domain::Result<void> bindListenerDrainObserver(
        std::weak_ptr<
            IDashboardListenerGenerationCoordinatorDrainObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void>
    bindRegistryConnectionDrainObserver(
        std::weak_ptr<IDashboardConnectionRegistryDrainObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void>
    bindRegistryRoutingProgressObserver(
        std::weak_ptr<IDashboardConnectionRegistryRoutingProgressObserver>
            observer) noexcept override;
    [[nodiscard]] Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<IDashboardOverloadResponderSetDrainObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void> registerShutdownDeadlineTarget(
        std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> target)
        noexcept override;

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override;
    [[nodiscard]] Domain::Result<WindowsDashboardDeadline>
    scheduleShutdownDeadline(WindowsDashboardDeadlineRequest request)
        noexcept override;
    [[nodiscard]] bool cancelShutdownDeadline(
        std::uint64_t registrationId,
        std::uint64_t armSequence) noexcept override;

    void closeRuntimeAdmission() noexcept override;
    void closeHandlerAdmission() noexcept override;
    void beginGracefulListenerShutdown() noexcept override;
    void beginOverloadShutdown() noexcept override;
    void beginGracefulRegistryShutdown() noexcept override;
    void beginHardListenerShutdown() noexcept override;
    void beginHardRegistryShutdown() noexcept override;
    void beginCompletionRouterShutdown() noexcept override;
    void joinHandlerExecutor() noexcept override;

    [[nodiscard]] DashboardShutdownDrainHostSnapshot snapshot()
        const noexcept override;
    [[nodiscard]] bool unregisterOverloadDeadlineTarget()
        noexcept override;
    [[nodiscard]] bool unregisterOverloadCompletionTarget()
        noexcept override;
    [[nodiscard]] bool unregisterShutdownDeadlineTarget(
        const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
        noexcept override;
    void shutdownDeadlineScheduler() noexcept override;
    [[nodiscard]] Domain::Result<
        DashboardDeadlineRoutingFinalizeDisposition>
    finalizeDeadlineRouting() noexcept override;
    void shutdownIocpKernel() noexcept override;

private:
    DashboardConnectionRuntimeServices* runtimeServices_{};
    WindowsDashboardHandlerExecutor* handlerExecutor_{};
    WindowsDashboardDeadlineScheduler* deadlineScheduler_{};
    DashboardIocpWorkerKernel* kernel_{};
    std::weak_ptr<DashboardDeadlineIocpBridge> deadlineBridge_;
    std::weak_ptr<DashboardListenerGenerationCoordinator>
        listenerCoordinator_;
    std::weak_ptr<DashboardOverloadResponderSet> overloadResponders_;
    std::weak_ptr<DashboardConnectionRegistry> connectionRegistry_;
    std::weak_ptr<DashboardIocpCompletionRouter> completionRouter_;
};

class DashboardShutdownDrainSnapshot final {
public:
    [[nodiscard]] DashboardShutdownDrainLifecycle lifecycle()
        const noexcept;
    [[nodiscard]] bool installed() const noexcept;
    [[nodiscard]] bool gracefulShutdownRequested() const noexcept;
    [[nodiscard]] bool hardShutdownRequested() const noexcept;
    [[nodiscard]] bool handlerDrained() const noexcept;
    [[nodiscard]] bool listenersDrained() const noexcept;
    [[nodiscard]] bool overloadDrained() const noexcept;
    [[nodiscard]] bool registryConnectionsDrained() const noexcept;
    [[nodiscard]] bool executorFinalizerReturned() const noexcept;
    [[nodiscard]] std::uint64_t routingProgressRevision() const noexcept;
    [[nodiscard]] std::uint64_t staleDeadlineCount() const noexcept;
    [[nodiscard]] std::uint64_t hardEscalationCount() const noexcept;
    [[nodiscard]] std::uint64_t failFastCount() const noexcept;
    [[nodiscard]] const WindowsDashboardDeadline* currentDeadline()
        const noexcept;
    [[nodiscard]] const DashboardDeadlineIocpFailure* bridgeFailure()
        const noexcept;
    [[nodiscard]] const WindowsDashboardDeadlineSchedulerFailure*
    deadlineSchedulerFailure() const noexcept;

private:
    friend class DashboardShutdownDrain;

    DashboardShutdownDrainSnapshot(
        DashboardShutdownDrainLifecycle lifecycle,
        bool installed,
        bool gracefulShutdownRequested,
        bool hardShutdownRequested,
        bool handlerDrained,
        bool listenersDrained,
        bool overloadDrained,
        bool registryConnectionsDrained,
        bool executorFinalizerReturned,
        std::uint64_t routingProgressRevision,
        std::uint64_t staleDeadlineCount,
        std::uint64_t hardEscalationCount,
        std::uint64_t failFastCount,
        std::optional<WindowsDashboardDeadline> currentDeadline,
        std::optional<DashboardDeadlineIocpFailure> bridgeFailure,
        std::optional<WindowsDashboardDeadlineSchedulerFailure>
            deadlineSchedulerFailure) noexcept;

    DashboardShutdownDrainLifecycle lifecycle_{
        DashboardShutdownDrainLifecycle::Registered};
    bool installed_{};
    bool gracefulShutdownRequested_{};
    bool hardShutdownRequested_{};
    bool handlerDrained_{};
    bool listenersDrained_{};
    bool overloadDrained_{};
    bool registryConnectionsDrained_{};
    bool executorFinalizerReturned_{};
    std::uint64_t routingProgressRevision_{};
    std::uint64_t staleDeadlineCount_{};
    std::uint64_t hardEscalationCount_{};
    std::uint64_t failFastCount_{};
    std::optional<WindowsDashboardDeadline> currentDeadline_;
    std::optional<DashboardDeadlineIocpFailure> bridgeFailure_;
    std::optional<WindowsDashboardDeadlineSchedulerFailure>
        deadlineSchedulerFailure_;
};

class DashboardShutdownDrain final
    : public IDashboardAuxiliaryDeadlineTarget,
      public IDashboardHandlerExecutorDrainObserver,
      public IDashboardListenerGenerationCoordinatorDrainObserver,
      public IDashboardOverloadResponderSetDrainObserver,
      public IDashboardConnectionRegistryDrainObserver,
      public IDashboardConnectionRegistryRoutingProgressObserver,
      public IDashboardDeadlineIocpBridgeFailureObserver,
      public IWindowsDashboardDeadlineSchedulerFailureObserver,
      public std::enable_shared_from_this<DashboardShutdownDrain> {
public:
    static constexpr auto GraceLifetime = std::chrono::seconds{5};
    static constexpr auto DriverStartupTimeout = std::chrono::seconds{5};

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardShutdownDrain>>
    create(
        std::uint64_t registrationId,
        std::shared_ptr<IDashboardShutdownDrainHost> host,
        std::shared_ptr<IDashboardShutdownDrainFailFast> failFast) noexcept;

    DashboardShutdownDrain(const DashboardShutdownDrain&) = delete;
    DashboardShutdownDrain& operator=(const DashboardShutdownDrain&) = delete;
    DashboardShutdownDrain(DashboardShutdownDrain&&) = delete;
    DashboardShutdownDrain& operator=(DashboardShutdownDrain&&) = delete;
    ~DashboardShutdownDrain() noexcept override;

    [[nodiscard]] Domain::Result<void> install() noexcept;
    void requestGracefulShutdown() noexcept;
    [[nodiscard]] Domain::Result<void> wait() noexcept;

    [[nodiscard]] std::uint64_t registrationId() const noexcept override;
    void dispatchDeadline(WindowsDashboardDeadline deadline)
        noexcept override;
    void beginShutdown() noexcept override;

    void handlerExecutorMayHaveDrained() noexcept override;
    void listenerGenerationsMayHaveDrained() noexcept override;
    void overloadRespondersMayHaveDrained() noexcept override;
    void registryConnectionsMayHaveDrained() noexcept override;
    void registryRoutingMayHaveProgressed(
        std::uint64_t revision) noexcept override;
    void dashboardDeadlineIocpBridgeFailed(
        DashboardDeadlineIocpFailure failure) noexcept override;
    void dashboardDeadlineSchedulerFailed(
        WindowsDashboardDeadlineSchedulerFailure failure) noexcept override;

    [[nodiscard]] DashboardShutdownDrainSnapshot snapshot()
        const noexcept;
    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

private:
    DashboardShutdownDrain(
        std::uint64_t registrationId,
        std::shared_ptr<IDashboardShutdownDrainHost> host,
        std::shared_ptr<IDashboardShutdownDrainFailFast> failFast) noexcept;

    void driverMain() noexcept;
    [[nodiscard]] bool runGracefulCutoff() noexcept;
    void runHardFanout() noexcept;
    [[nodiscard]] bool startExecutorFinalizer() noexcept;
    [[nodiscard]] bool logicalDrainReady(
        const DashboardShutdownDrainHostSnapshot& hostSnapshot)
        const noexcept;
    [[nodiscard]] bool runFinalTeardown() noexcept;
    [[nodiscard]] bool runFatalTerminalTeardown() noexcept;
    [[nodiscard]] bool runTerminalTeardown(bool preserveFatal) noexcept;
    [[nodiscard]] bool finalizeDeadlineRouting(bool allowFatal) noexcept;
    void finishFatalTerminal() noexcept;
    void joinExecutorFinalizer() noexcept;
    void latchSimpleDrainEdge(bool& edge) noexcept;
    void retainFatal(Domain::Error error, bool requestHard) noexcept;
    void retainFixedFatal(bool requestHard) noexcept;
    void invokeFailFastOnce() noexcept;
    void finishDriver() noexcept;
    [[nodiscard]] Domain::Result<void> failInstallation(
        Domain::Error error) noexcept;
    [[nodiscard]] DashboardShutdownDrainSnapshot snapshotLocked()
        const noexcept;

    const std::uint64_t registrationId_{};
    const std::shared_ptr<IDashboardShutdownDrainHost> host_;
    const std::shared_ptr<IDashboardShutdownDrainFailFast> failFast_;

    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    // This process object owns both handles. driver_ is persistent from
    // create() through terminal publication; executorFinalizer_ exists only
    // to keep blocking executor join work off the deadline-driving thread.
    std::thread driver_;
    std::thread executorFinalizer_;
    std::thread::id driverThreadId_{};
    std::optional<WindowsDashboardDeadline> currentDeadline_;
    std::optional<WindowsDashboardDeadline> earlyDeadline_;
    std::optional<WindowsDashboardDeadline> finalizationCancellation_;
    std::optional<DashboardDeadlineIocpFailure> bridgeFailure_;
    std::optional<WindowsDashboardDeadlineSchedulerFailure>
        deadlineSchedulerFailure_;
    std::optional<Domain::Error> firstFailure_;
    DashboardShutdownDrainLifecycle lifecycle_{
        DashboardShutdownDrainLifecycle::Registered};
    std::uint64_t wakeRevision_{};
    std::uint64_t routingWakeRevision_{};
    std::uint64_t routingProgressRevision_{};
    std::uint64_t staleDeadlineCount_{};
    std::uint64_t hardEscalationCount_{};
    std::uint64_t failFastCount_{};
    bool driverReady_{};
    bool driverExited_{};
    bool exitRequested_{};
    bool installing_{};
    bool installed_{};
    bool shutdownDeadlineTargetRegistered_{};
    bool gracefulShutdownRequested_{};
    bool gracefulCutoffCompleted_{};
    bool hardShutdownRequested_{};
    bool hardFanoutClaimed_{};
    bool hardFanoutCompleted_{};
    bool handlerDrained_{};
    bool listenersDrained_{};
    bool overloadDrained_{};
    bool registryConnectionsDrained_{};
    bool executorFinalizerStarted_{};
    bool executorFinalizerReturned_{};
    bool executorFinalizerJoined_{};
    bool overloadDeadlineTargetRemoved_{};
    bool overloadCompletionTargetRemoved_{};
    bool completionRouterShutdownStarted_{};
    bool deadlineSchedulerShutdownCompleted_{};
    bool deadlineRoutingFinalized_{};
    bool iocpKernelShutdownCompleted_{};
    bool terminalTeardownCompleted_{};
    bool failFastInvoked_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
