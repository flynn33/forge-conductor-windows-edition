#include "Infrastructure/Windows/Detail/DashboardConnectionRegistry.h"
#include "Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.h"
#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"
#include "Infrastructure/Windows/Detail/DashboardIocpCompletionRouter.h"
#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"
#include "Infrastructure/Windows/Detail/DashboardShutdownDrain.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Api = Detail::IDashboardIoCompletionPortApi;
using Bridge = Detail::DashboardDeadlineIocpBridge;
using Drain = Detail::DashboardShutdownDrain;
using FinalizeDisposition =
    Detail::DashboardDeadlineRoutingFinalizeDisposition;
using Kernel = Detail::DashboardIocpWorkerKernel;
using Key = Detail::DashboardIoCompletionKey;
using Port = Detail::DashboardIoCompletionPort;
using Registry = Detail::DashboardConnectionRegistry;
using Router = Detail::DashboardIocpCompletionRouter;
using Scheduler = Windows::WindowsDashboardDeadlineScheduler;

constexpr Key DeadlineKey{0x53485554444F574EU};
constexpr std::uint64_t ShutdownRegistrationId = 7'911U;
constexpr auto TestTimeout = std::chrono::seconds{5};

std::size_t assertionCount{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        fail(message);
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TestTimeout;
    while (!std::invoke(std::forward<Predicate>(predicate))) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

class AdjustableClock final : public ForgeConductor::Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return Domain::MonotonicTimePoint{
            std::chrono::milliseconds{milliseconds_.load()}};
    }

    void advance(const std::chrono::milliseconds duration) noexcept
    {
        milliseconds_.fetch_add(duration.count());
    }

private:
    std::atomic<std::int64_t> milliseconds_{10'000};
};

// The real IOCP primitive and four-worker kernel use this narrow native-call
// seam. Data packets remain queued until the test releases them, while control
// packets are always visible so kernel shutdown cannot be stranded.
class ControlledCompletionPortApi final : public Api {
public:
    struct Packet final {
        DWORD transferredBytes{};
        ULONG_PTR completionKey{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(0x5A17C0DEU));
    }

    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE fileHandle,
        const HANDLE existingCompletionPort,
        const ULONG_PTR,
        const DWORD) noexcept override
    {
        if (fileHandle == INVALID_HANDLE_VALUE &&
            existingCompletionPort == nullptr) {
            setThreadError(ERROR_SUCCESS);
            return portHandle();
        }
        if (existingCompletionPort == portHandle()) {
            setThreadError(ERROR_SUCCESS);
            return portHandle();
        }
        setThreadError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        const HANDLE completionPort,
        const DWORD transferredBytes,
        const ULONG_PTR completionKey,
        OVERLAPPED* const operation) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (completionPort != portHandle() || closed_) {
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        Packet packet{transferredBytes, completionKey, operation};
        if (completionKey == Kernel::ShutdownKeyValue) {
            controlPackets_.push_back(packet);
        } else {
            dataPackets_.push_back(packet);
            ++dataPostCount_;
        }
        changed_.notify_all();
        setThreadError(ERROR_SUCCESS);
        return TRUE;
    }

    [[nodiscard]] BOOL getQueuedCompletionStatus(
        const HANDLE completionPort,
        DWORD& transferredBytes,
        ULONG_PTR& completionKey,
        OVERLAPPED*& operation,
        const DWORD timeoutMilliseconds) noexcept override
    {
        std::unique_lock lock{mutex_};
        const auto ready = [this] {
            return closed_ || !controlPackets_.empty() ||
                (releaseData_ && !dataPackets_.empty());
        };
        if (!ready()) {
            static_cast<void>(changed_.wait_for(
                lock,
                std::chrono::milliseconds{timeoutMilliseconds},
                ready));
        }
        if (closed_ || completionPort != portHandle()) {
            transferredBytes = 0U;
            completionKey = 0U;
            operation = nullptr;
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        std::optional<Packet> packet;
        if (!controlPackets_.empty()) {
            packet.emplace(controlPackets_.front());
            controlPackets_.pop_front();
        } else if (releaseData_ && !dataPackets_.empty()) {
            packet.emplace(dataPackets_.front());
            dataPackets_.pop_front();
        }
        if (!packet.has_value()) {
            transferredBytes = 0U;
            completionKey = 0U;
            operation = nullptr;
            setThreadError(WAIT_TIMEOUT);
            return FALSE;
        }

        transferredBytes = packet->transferredBytes;
        completionKey = packet->completionKey;
        operation = packet->operation;
        setThreadError(ERROR_SUCCESS);
        return TRUE;
    }

    [[nodiscard]] BOOL closeHandle(const HANDLE handle) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (handle != portHandle()) {
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        closed_ = true;
        changed_.notify_all();
        setThreadError(ERROR_SUCCESS);
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override
    {
        return threadError_;
    }

    [[nodiscard]] bool waitForDataPost() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, TestTimeout, [this] {
            return dataPostCount_ != 0U;
        });
    }

    void releaseData() noexcept
    {
        const std::lock_guard lock{mutex_};
        releaseData_ = true;
        changed_.notify_all();
    }

private:
    static void setThreadError(const DWORD error) noexcept
    {
        threadError_ = error;
    }

    inline static thread_local DWORD threadError_{ERROR_SUCCESS};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Packet> controlPackets_;
    std::deque<Packet> dataPackets_;
    std::size_t dataPostCount_{};
    bool releaseData_{};
    bool closed_{};
};

class FailFastRecorder final
    : public Detail::IDashboardShutdownDrainFailFast {
public:
    void failFast() noexcept override { calls_.fetch_add(1U); }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        return calls_.load();
    }

private:
    std::atomic_size_t calls_{};
};

// This adapter intentionally fakes only the unrelated runtime/listener/
// overload/executor owners. Every object on the shutdown-deadline retirement
// path is a real production component.
class TombstoneIntegrationHost final
    : public Detail::IDashboardShutdownDrainHost {
public:
    TombstoneIntegrationHost(
        std::shared_ptr<AdjustableClock> clock,
        Scheduler& scheduler,
        Kernel& kernel,
        std::shared_ptr<Bridge> bridge,
        std::shared_ptr<Registry> registry,
        std::shared_ptr<Router> router) noexcept
        : clock_{std::move(clock)},
          scheduler_{std::addressof(scheduler)},
          kernel_{std::addressof(kernel)},
          bridge_{std::move(bridge)},
          registry_{std::move(registry)},
          router_{std::move(router)}
    {
    }

    [[nodiscard]] Domain::Result<void> bindBridgeFailureObserver(
        std::weak_ptr<Detail::IDashboardDeadlineIocpBridgeFailureObserver>
            observer) noexcept override
    {
        return bridge_->bindFailureObserver(std::move(observer));
    }

    [[nodiscard]] Domain::Result<void>
    bindDeadlineSchedulerFailureObserver(
        std::weak_ptr<
            Windows::IWindowsDashboardDeadlineSchedulerFailureObserver>
            observer) noexcept override
    {
        return scheduler_->bindFailureObserver(std::move(observer));
    }

    [[nodiscard]] Domain::Result<void> bindHandlerDrainObserver(
        std::weak_ptr<Windows::IDashboardHandlerExecutorDrainObserver>
            observer) noexcept override
    {
        const std::lock_guard lock{mutex_};
        handlerObserver_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> bindListenerDrainObserver(
        std::weak_ptr<
            Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
            observer) noexcept override
    {
        const std::lock_guard lock{mutex_};
        listenerObserver_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void>
    bindRegistryConnectionDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionRegistryDrainObserver>
            observer) noexcept override
    {
        return registry_->bindShutdownDrainObserver(std::move(observer));
    }

    [[nodiscard]] Domain::Result<void>
    bindRegistryRoutingProgressObserver(
        std::weak_ptr<
            Detail::IDashboardConnectionRegistryRoutingProgressObserver>
            observer) noexcept override
    {
        return registry_->bindRoutingProgressObserver(std::move(observer));
    }

    [[nodiscard]] Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<Detail::IDashboardOverloadResponderSetDrainObserver>
            observer) noexcept override
    {
        const std::lock_guard lock{mutex_};
        overloadObserver_ = std::move(observer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> registerShutdownDeadlineTarget(
        std::shared_ptr<Detail::IDashboardAuxiliaryDeadlineTarget> target)
        noexcept override
    {
        return registry_->registerAuxiliaryDeadlineTarget(
            std::move(target));
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return clock_->monotonicNow();
    }

    [[nodiscard]] Domain::Result<Windows::WindowsDashboardDeadline>
    scheduleShutdownDeadline(
        Windows::WindowsDashboardDeadlineRequest request)
        noexcept override
    {
        auto scheduled = scheduler_->schedule(std::move(request));
        if (scheduled) {
            const std::lock_guard lock{mutex_};
            scheduledDeadline_.emplace(scheduled.value());
            changed_.notify_all();
        }
        return scheduled;
    }

    [[nodiscard]] bool cancelShutdownDeadline(
        const std::uint64_t registrationId,
        const std::uint64_t armSequence) noexcept override
    {
        const bool cancelled =
            scheduler_->cancel(registrationId, armSequence);
        const std::lock_guard lock{mutex_};
        cancelCalled_ = true;
        cancelResult_ = cancelled;
        changed_.notify_all();
        return cancelled;
    }

    void closeRuntimeAdmission() noexcept override {}
    void closeHandlerAdmission() noexcept override {}

    void beginGracefulListenerShutdown() noexcept override
    {
        publishListenerDrain();
    }

    void beginOverloadShutdown() noexcept override
    {
        publishOverloadDrain();
    }

    void beginGracefulRegistryShutdown() noexcept override
    {
        registry_->beginGracefulShutdown();
    }

    void beginHardListenerShutdown() noexcept override
    {
        publishListenerDrain();
    }

    void beginHardRegistryShutdown() noexcept override
    {
        registry_->beginShutdown();
    }

    void beginCompletionRouterShutdown() noexcept override
    {
        router_->beginShutdown();
    }

    void joinHandlerExecutor() noexcept override
    {
        std::weak_ptr<Windows::IDashboardHandlerExecutorDrainObserver>
            observer;
        {
            std::unique_lock lock{mutex_};
            handlerJoinEntered_ = true;
            changed_.notify_all();
            static_cast<void>(changed_.wait_for(
                lock, TestTimeout, [this] { return releaseHandlerJoin_; }));
            executorFullyDrained_ = true;
            observer = handlerObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->handlerExecutorMayHaveDrained();
        }
    }

    [[nodiscard]] Detail::DashboardShutdownDrainHostSnapshot snapshot()
        const noexcept override
    {
        bool executorFullyDrained{};
        bool listenerRoutingDrained{};
        bool overloadFullyDrained{};
        std::size_t fixedTargetCount{};
        {
            const std::lock_guard lock{mutex_};
            executorFullyDrained = executorFullyDrained_;
            listenerRoutingDrained = listenerRoutingDrained_;
            overloadFullyDrained = overloadFullyDrained_;
            fixedTargetCount = modeledFixedTargetCount_;
        }
        const auto registrySnapshot = registry_->snapshot();
        const auto bridgeSnapshot = bridge_->snapshot();
        const auto routerSnapshot = router_->snapshot();
        const auto schedulerSnapshot = scheduler_->snapshot();
        return Detail::DashboardShutdownDrainHostSnapshot{
            true,
            executorFullyDrained,
            listenerRoutingDrained,
            overloadFullyDrained,
            registrySnapshot.registeredConnectionCount(),
            registrySnapshot.registeredAuxiliaryDeadlineTargetCount(),
            fixedTargetCount,
            registrySnapshot.routingProgressRevision(),
            registrySnapshot.deadlineRoutingInProgress(),
            registrySnapshot.isFatal() || bridgeSnapshot.isFatal() ||
                routerSnapshot.fatalNativeError().has_value() ||
                schedulerSnapshot.failure() != nullptr};
    }

    [[nodiscard]] bool unregisterOverloadDeadlineTarget()
        noexcept override
    {
        const std::lock_guard lock{mutex_};
        overloadDeadlineTargetRemoved_ = true;
        return true;
    }

    [[nodiscard]] bool unregisterOverloadCompletionTarget()
        noexcept override
    {
        const std::lock_guard lock{mutex_};
        modeledFixedTargetCount_ = 0U;
        return true;
    }

    [[nodiscard]] bool unregisterShutdownDeadlineTarget(
        const std::shared_ptr<Detail::IDashboardAuxiliaryDeadlineTarget>&
            target) noexcept override
    {
        return registry_->unregisterAuxiliaryDeadlineTarget(target);
    }

    void shutdownDeadlineScheduler() noexcept override
    {
        scheduler_->shutdown();
    }

    [[nodiscard]] Domain::Result<FinalizeDisposition>
    finalizeDeadlineRouting() noexcept override
    {
        auto finalized = registry_->finalizeDeadlineRouting();
        {
            const std::lock_guard lock{mutex_};
            ++finalizeCallCount_;
            if (finalized && finalized.value() ==
                    FinalizeDisposition::Pending) {
                ++pendingFinalizeCount_;
            }
            changed_.notify_all();
        }
        return finalized;
    }

    void shutdownIocpKernel() noexcept override
    {
        kernel_->shutdown();
    }

    [[nodiscard]] bool waitForScheduledDeadline() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, TestTimeout, [this] {
            return scheduledDeadline_.has_value();
        });
    }

    [[nodiscard]] bool waitForHandlerJoin() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, TestTimeout, [this] {
            return handlerJoinEntered_;
        });
    }

    void releaseHandlerJoin() noexcept
    {
        const std::lock_guard lock{mutex_};
        releaseHandlerJoin_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitForPendingFinalize() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, TestTimeout, [this] {
            return pendingFinalizeCount_ != 0U;
        });
    }

    [[nodiscard]] bool cancelReturnedFalse() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return cancelCalled_ && !cancelResult_;
    }

    [[nodiscard]] std::size_t finalizeCallCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return finalizeCallCount_;
    }

private:
    void publishListenerDrain() noexcept
    {
        std::weak_ptr<
            Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
            observer;
        {
            const std::lock_guard lock{mutex_};
            listenerRoutingDrained_ = true;
            observer = listenerObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->listenerGenerationsMayHaveDrained();
        }
    }

    void publishOverloadDrain() noexcept
    {
        std::weak_ptr<Detail::IDashboardOverloadResponderSetDrainObserver>
            observer;
        {
            const std::lock_guard lock{mutex_};
            overloadFullyDrained_ = true;
            observer = overloadObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->overloadRespondersMayHaveDrained();
        }
    }

    const std::shared_ptr<AdjustableClock> clock_;
    Scheduler* scheduler_{};
    Kernel* kernel_{};
    const std::shared_ptr<Bridge> bridge_;
    const std::shared_ptr<Registry> registry_;
    const std::shared_ptr<Router> router_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::weak_ptr<Windows::IDashboardHandlerExecutorDrainObserver>
        handlerObserver_;
    std::weak_ptr<
        Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
        listenerObserver_;
    std::weak_ptr<Detail::IDashboardOverloadResponderSetDrainObserver>
        overloadObserver_;
    std::optional<Windows::WindowsDashboardDeadline> scheduledDeadline_;
    std::size_t modeledFixedTargetCount_{1U};
    std::size_t finalizeCallCount_{};
    std::size_t pendingFinalizeCount_{};
    bool handlerJoinEntered_{};
    bool releaseHandlerJoin_{};
    bool executorFullyDrained_{};
    bool listenerRoutingDrained_{};
    bool overloadFullyDrained_{};
    bool overloadDeadlineTargetRemoved_{};
    bool cancelCalled_{};
    bool cancelResult_{true};
};

void postedRetiredTombstoneIsReapedBeforeRoutingFinalizes()
{
    auto clock = std::make_shared<AdjustableClock>();
    auto api = std::make_shared<ControlledCompletionPortApi>();
    auto registry = take(Registry::create(DeadlineKey));
    auto router = take(Router::create(DeadlineKey, registry));
    auto port = take(Port::create(api));
    auto kernel = take(Kernel::create(std::move(port), router));
    auto bridge = take(Bridge::create(*kernel, DeadlineKey));
    take(registry->bindDeadlineBridge(bridge));
    auto scheduler = take(Scheduler::create(clock, bridge));
    auto host = std::make_shared<TombstoneIntegrationHost>(
        clock,
        *scheduler,
        *kernel,
        bridge,
        registry,
        router);
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = take(Drain::create(
        ShutdownRegistrationId, host, failFast));
    take(drain->install());

    drain->requestGracefulShutdown();
    const bool scheduled = host->waitForScheduledDeadline();
    const bool handlerJoinStarted = host->waitForHandlerJoin();
    clock->advance(std::chrono::seconds{6});
    const bool dataPosted = api->waitForDataPost();
    const bool postedBeforeRetirement = waitUntil([&] {
        const auto snapshot = bridge->snapshot();
        return snapshot.postedOperationCount() == 1U &&
            snapshot.retiredAwaitingReapCount() == 0U;
    });

    host->releaseHandlerJoin();
    const bool pendingFinalize = host->waitForPendingFinalize();
    const bool exactTombstone = waitUntil([&] {
        const auto snapshot = bridge->snapshot();
        return snapshot.registeredOwnerCount() == 0U &&
            snapshot.postedOperationCount() == 1U &&
            snapshot.retiredAwaitingReapCount() == 1U;
    });

    api->releaseData();
    const auto drained = drain->wait();

    require(scheduled, "the real scheduler was not armed");
    require(handlerJoinStarted,
            "the executor seam did not hold natural teardown open");
    require(dataPosted,
            "the real scheduler did not publish through the deadline bridge");
    require(postedBeforeRetirement,
            "the live bridge packet was not held before owner retirement");
    require(pendingFinalize,
            "routing finalization did not wait for the retired packet");
    require(exactTombstone,
            "self-route retirement did not retain one exact posted tombstone");
    require(host->cancelReturnedFalse(),
            "natural teardown did not observe the already-published arm as uncancellable");
    take(std::move(drained));

    const auto drainSnapshot = drain->snapshot();
    const auto bridgeSnapshot = bridge->snapshot();
    const auto registrySnapshot = registry->snapshot();
    const auto routerSnapshot = router->snapshot();
    const auto kernelSnapshot = kernel->snapshot();
    require(drainSnapshot.lifecycle() ==
                Detail::DashboardShutdownDrainLifecycle::Drained,
            "the real tombstone chain did not reach Drained");
    require(drainSnapshot.hardEscalationCount() == 0U &&
                failFast->calls() == 0U,
            "the real tombstone chain unexpectedly failed closed");
    require(bridgeSnapshot.registeredOwnerCount() == 0U &&
                bridgeSnapshot.postedOperationCount() == 0U &&
                bridgeSnapshot.retiredAwaitingReapCount() == 0U &&
                bridgeSnapshot.drainedRetiredCount() == 1U &&
                bridgeSnapshot.isShutdown(),
            "the bridge did not reap and close the exact retired packet");
    require(registrySnapshot.retiredDeadlineDrainCount() == 1U &&
                registrySnapshot.routingProgressRevision() >= 2U &&
                !registrySnapshot.isFatal(),
            "the registry did not publish exact tombstone routing progress");
    require(routerSnapshot.fallbackDispatchCount() == 1U &&
                !routerSnapshot.fatalNativeError().has_value(),
            "the worker kernel did not route the packet through the real fallback router");
    require(host->finalizeCallCount() >= 2U,
            "routing finalization did not retry after the progress edge");
    require(kernelSnapshot.startedWorkerCount() == Kernel::WorkerCount &&
                kernelSnapshot.exitedWorkerCount() == Kernel::WorkerCount &&
                kernelSnapshot.isShuttingDown() &&
                !kernelSnapshot.controlPostFailed() &&
                !kernelSnapshot.fatalNativeError().has_value(),
            "the real IOCP kernel did not complete its exact shutdown join");
}

} // namespace

int main()
{
    try {
        postedRetiredTombstoneIsReapedBeforeRoutingFinalizes();
        std::cout
            << "DashboardShutdownDrainTombstoneIntegrationTests passed: 1 case, "
            << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "DashboardShutdownDrainTombstoneIntegrationTests failed after "
            << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    }
}
