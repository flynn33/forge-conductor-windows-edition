#include "Infrastructure/Windows/Detail/DashboardConnectionRegistry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Api = Detail::IDashboardIoCompletionPortApi;
using Bridge = Detail::DashboardDeadlineIocpBridge;
using EventFailure = Detail::DashboardConnectionEventFailure;
using EventFailureKind = Detail::DashboardConnectionEventFailureKind;
using EventNotification =
    Detail::DashboardConnectionEventFatalNotification;
using Key = Detail::DashboardIoCompletionKey;
using Kernel = Detail::DashboardIocpWorkerKernel;
using Packet = Detail::DashboardIoCompletionPacket;
using Port = Detail::DashboardIoCompletionPort;
using Registry = Detail::DashboardConnectionRegistry;
using RegistryFailureKind =
    Detail::DashboardConnectionRegistryFailureKind;
using Target = Detail::IDashboardConnectionDispatchTarget;
using AuxiliaryTarget = Detail::IDashboardAuxiliaryDeadlineTarget;
using RegistryFailFast = Detail::IDashboardConnectionRegistryFailFast;

class TestClock final : public ForgeConductor::Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

static_assert(std::is_final_v<Registry>);
static_assert(std::is_base_of_v<Detail::IDashboardIocpCompletionSink, Registry>);
static_assert(std::is_base_of_v<
              Detail::IDashboardConnectionEventFatalSink,
              Registry>);
static_assert(std::is_base_of_v<
              Detail::IDashboardConnectionDrainObserver,
              Registry>);
static_assert(!std::is_copy_constructible_v<Registry>);
static_assert(!std::is_move_constructible_v<Registry>);
static_assert(Registry::MaximumConnectionCount == 40U);
static_assert(Registry::MaximumAuxiliaryDeadlineTargetCount == 4U);
static_assert(noexcept(Registry::create(Key{1U})));
static_assert(noexcept(std::declval<Registry&>().bindDeadlineBridge(nullptr)));
static_assert(noexcept(
    std::declval<Registry&>().bindGenerationDrainObserver({})));
static_assert(noexcept(std::declval<Registry&>().registerConnection(nullptr)));
static_assert(noexcept(
    std::declval<Registry&>().registerAuxiliaryDeadlineTarget(nullptr)));
static_assert(noexcept(
    std::declval<Registry&>().unregisterAuxiliaryDeadlineTarget(
        std::declval<const std::shared_ptr<AuxiliaryTarget>&>())));
static_assert(noexcept(std::declval<Registry&>().removeIfDrained(
    std::declval<const std::shared_ptr<Target>&>())));
static_assert(noexcept(
    std::declval<const Registry&>().connectionCountForGeneration(1U)));
static_assert(noexcept(
    std::declval<Registry&>().beginShutdownGeneration(1U)));
static_assert(noexcept(
    std::declval<Registry&>().connectionMayHaveDrained(
        Key{1U}, 1U, 1U)));
static_assert(noexcept(std::declval<Registry&>().consume(
    Packet{}, ERROR_SUCCESS)));
static_assert(noexcept(std::declval<Registry&>().fatal(ERROR_INVALID_DATA)));
static_assert(noexcept(std::declval<Registry&>().fatal(
    EventNotification{})));
static_assert(noexcept(std::declval<const Registry&>().snapshot()));
static_assert(!noexcept(std::declval<const Registry&>().fullFailure()));
static_assert(noexcept(std::declval<Registry&>().beginShutdown()));
static_assert(noexcept(
    std::declval<Registry&>().finalizeDeadlineRouting()));
static_assert(noexcept(std::declval<const Bridge&>().completionKey()));

constexpr Key DeadlineKey{0x444541444C494E45U};
constexpr Key OtherDeadlineKey{0x444541444C494E46U};
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

class RecordingRegistryFailFast final : public RegistryFailFast {
public:
    void failFast() noexcept override { calls_.fetch_add(1U); }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        return calls_.load();
    }

private:
    std::atomic_size_t calls_{};
};

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

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

class FakeTarget final : public Target {
public:
    FakeTarget(
        const Key key,
        const std::uint64_t registrationId,
        const std::uint64_t generationId) noexcept
        : key_{key},
          registrationId_{registrationId},
          generationId_{generationId}
    {
    }

    ~FakeTarget() noexcept override
    {
        if (destructorCount_ != nullptr) {
            ++(*destructorCount_);
        }
        if (auto registry = destructorRegistry_.lock(); registry != nullptr) {
            static_cast<void>(registry->snapshot());
            if (destructorReentered_ != nullptr) {
                destructorReentered_->store(true);
            }
        }
    }

    [[nodiscard]] Key completionKey() const noexcept override { return key_; }
    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return registrationId_;
    }
    [[nodiscard]] std::uint64_t generationId() const noexcept override
    {
        return generationId_;
    }

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionDrainObserver> observer)
        noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (drainObserverBound_ || observer.expired()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "fake target drain observer binding failed"));
        }
        drainObserver_ = std::move(observer);
        drainObserverBound_ = true;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        bool postDuringStart{};
        std::shared_ptr<Registry> registry;
        {
            std::unique_lock lock{mutex_};
            ++startCount_;
            startEntered_ = true;
            changed_.notify_all();
            if (blockStart_) {
                static_cast<void>(changed_.wait_for(
                    lock,
                    TestTimeout,
                    [this] { return releaseStart_; }));
            }
            postDuringStart = routeDuringStart_;
            registry = registry_.lock();
            if (startFails_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "fake target start failure"));
            }
        }
        if (postDuringStart && registry != nullptr) {
            registry->consume(
                Packet{17U, key_, std::addressof(operation_)},
                ERROR_SUCCESS);
        }
        return Domain::Result<void>::success();
    }

    void dispatchIocp(
        const DWORD transferredBytes,
        OVERLAPPED* const operation,
        const DWORD nativeError) noexcept override
    {
        std::unique_lock lock{mutex_};
        ++iocpCount_;
        lastTransferredBytes_ = transferredBytes;
        lastOperation_ = operation;
        lastNativeError_ = nativeError;
        dispatchEntered_ = true;
        changed_.notify_all();
        if (blockIocp_) {
            static_cast<void>(changed_.wait_for(
                lock,
                TestTimeout,
                [this] { return releaseIocp_; }));
        }
        if (drainOnIocp_) {
            drained_ = true;
        }
    }

    void dispatchDeadline(
        const Windows::WindowsDashboardDeadline deadline) noexcept override
    {
        std::unique_lock lock{mutex_};
        ++deadlineCount_;
        lastDeadline_ = deadline;
        deadlineDispatchEntered_ = true;
        changed_.notify_all();
        if (blockDeadline_) {
            static_cast<void>(changed_.wait_for(
                lock,
                TestTimeout,
                [this] { return releaseDeadline_; }));
        }
        if (drainOnDeadline_) {
            drained_ = true;
        }
    }

    void beginShutdown() noexcept override
    {
        std::shared_ptr<Registry> registry;
        {
            const std::lock_guard lock{mutex_};
            ++shutdownCount_;
            shutdownRequested_ = true;
            if (drainOnShutdown_) {
                drained_ = true;
            }
            registry = reenterOnShutdown_ ? registry_.lock() : nullptr;
            changed_.notify_all();
        }
        if (registry != nullptr) {
            static_cast<void>(registry->snapshot());
            reenteredShutdown_.store(true);
        }
    }

    [[nodiscard]] bool isDrained() const noexcept override
    {
        if (observeNextDrain_.exchange(false, std::memory_order_acq_rel)) {
            drainObserved_.store(true, std::memory_order_release);
        }
        const std::lock_guard lock{mutex_};
        return drained_;
    }

    [[nodiscard]] Detail::DashboardConnectionStateSnapshot snapshot()
        const noexcept override
    {
        std::terminate();
    }

    void attachRegistry(const std::shared_ptr<Registry>& registry) noexcept
    {
        const std::lock_guard lock{mutex_};
        registry_ = registry;
    }

    void routeOneCompletionDuringStart() noexcept
    {
        const std::lock_guard lock{mutex_};
        routeDuringStart_ = true;
    }

    void failStart() noexcept
    {
        const std::lock_guard lock{mutex_};
        startFails_ = true;
        drainOnShutdown_ = false;
    }

    void drainDuringIocp() noexcept
    {
        const std::lock_guard lock{mutex_};
        drainOnIocp_ = true;
    }

    void drainDuringDeadline() noexcept
    {
        const std::lock_guard lock{mutex_};
        drainOnDeadline_ = true;
    }

    void retainDuringShutdown() noexcept
    {
        const std::lock_guard lock{mutex_};
        drainOnShutdown_ = false;
    }

    void reenterRegistryDuringShutdown() noexcept
    {
        const std::lock_guard lock{mutex_};
        reenterOnShutdown_ = true;
    }

    void blockIocp() noexcept
    {
        const std::lock_guard lock{mutex_};
        blockIocp_ = true;
    }

    void blockDeadline() noexcept
    {
        const std::lock_guard lock{mutex_};
        blockDeadline_ = true;
    }

    void blockStart() noexcept
    {
        const std::lock_guard lock{mutex_};
        blockStart_ = true;
    }

    [[nodiscard]] bool waitForStartEntry() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, TestTimeout, [this] { return startEntered_; });
    }

    void releaseStart() noexcept
    {
        const std::lock_guard lock{mutex_};
        releaseStart_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitForIocpEntry() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, TestTimeout, [this] { return dispatchEntered_; });
    }

    void releaseIocp() noexcept
    {
        const std::lock_guard lock{mutex_};
        releaseIocp_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitForDeadlineEntry() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, TestTimeout, [this] { return deadlineDispatchEntered_; });
    }

    void releaseDeadline() noexcept
    {
        const std::lock_guard lock{mutex_};
        releaseDeadline_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitForShutdownEntry() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, TestTimeout, [this] { return shutdownCount_ != 0U; });
    }

    void markDrained() noexcept
    {
        const std::lock_guard lock{mutex_};
        drained_ = true;
    }

    void markDrainedAndNotify() noexcept
    {
        std::shared_ptr<Detail::IDashboardConnectionDrainObserver> observer;
        {
            const std::lock_guard lock{mutex_};
            drained_ = true;
            observer = drainObserver_.lock();
        }
        if (observer != nullptr) {
            observer->connectionMayHaveDrained(
                key_, registrationId_, generationId_);
        }
    }

    void observeNextDrainCheck() noexcept
    {
        drainObserved_.store(false, std::memory_order_release);
        observeNextDrain_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool drainCheckObserved() const noexcept
    {
        return drainObserved_.load(std::memory_order_acquire);
    }

    void reenterRegistryOnDestruction(
        const std::shared_ptr<Registry>& registry,
        std::atomic<std::size_t>& destructorCount,
        std::atomic<bool>& reentered) noexcept
    {
        destructorRegistry_ = registry;
        destructorCount_ = std::addressof(destructorCount);
        destructorReentered_ = std::addressof(reentered);
    }

    [[nodiscard]] std::size_t startCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return startCount_;
    }

    [[nodiscard]] std::size_t iocpCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return iocpCount_;
    }

    [[nodiscard]] std::size_t deadlineCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return deadlineCount_;
    }

    [[nodiscard]] std::size_t shutdownCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return shutdownCount_;
    }

    [[nodiscard]] DWORD lastTransferredBytes() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return lastTransferredBytes_;
    }

    [[nodiscard]] OVERLAPPED* lastOperation() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return lastOperation_;
    }

    [[nodiscard]] DWORD lastNativeError() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return lastNativeError_;
    }

    [[nodiscard]] std::optional<Windows::WindowsDashboardDeadline>
    lastDeadline() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return lastDeadline_;
    }

    [[nodiscard]] bool reenteredShutdown() const noexcept
    {
        return reenteredShutdown_.load();
    }

private:
    const Key key_;
    const std::uint64_t registrationId_{};
    const std::uint64_t generationId_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::weak_ptr<Registry> registry_;
    std::weak_ptr<Registry> destructorRegistry_;
    std::weak_ptr<Detail::IDashboardConnectionDrainObserver>
        drainObserver_;
    OVERLAPPED operation_{};
    OVERLAPPED* lastOperation_{};
    std::optional<Windows::WindowsDashboardDeadline> lastDeadline_;
    std::atomic<std::size_t>* destructorCount_{};
    std::atomic<bool>* destructorReentered_{};
    std::atomic<bool> reenteredShutdown_{};
    mutable std::atomic_bool observeNextDrain_{};
    mutable std::atomic_bool drainObserved_{};
    DWORD lastTransferredBytes_{};
    DWORD lastNativeError_{ERROR_SUCCESS};
    std::size_t startCount_{};
    std::size_t iocpCount_{};
    std::size_t deadlineCount_{};
    std::size_t shutdownCount_{};
    bool startFails_{};
    bool routeDuringStart_{};
    bool drainOnIocp_{};
    bool drainOnDeadline_{};
    bool drainOnShutdown_{true};
    bool reenterOnShutdown_{};
    bool blockStart_{};
    bool startEntered_{};
    bool releaseStart_{};
    bool blockIocp_{};
    bool dispatchEntered_{};
    bool releaseIocp_{};
    bool blockDeadline_{};
    bool deadlineDispatchEntered_{};
    bool releaseDeadline_{};
    bool drained_{};
    bool drainObserverBound_{};
    bool shutdownRequested_{};
};

class FakeAuxiliaryDeadlineTarget final : public AuxiliaryTarget {
public:
    explicit FakeAuxiliaryDeadlineTarget(
        const std::uint64_t registrationId) noexcept
        : registrationId_{registrationId}
    {
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return registrationId_;
    }

    void dispatchDeadline(
        const Windows::WindowsDashboardDeadline deadline) noexcept override
    {
        std::unique_lock lock{mutex_};
        ++deadlineCount_;
        lastDeadline_ = deadline;
        deadlineEntered_ = true;
        changed_.notify_all();
        if (blockDeadline_) {
            static_cast<void>(changed_.wait_for(
                lock,
                TestTimeout,
                [this] { return releaseDeadline_; }));
        }
    }

    void beginShutdown() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        ++shutdownCount_;
        changed_.notify_all();
    }

    void blockDeadline() noexcept
    {
        const std::scoped_lock lock{mutex_};
        blockDeadline_ = true;
    }

    [[nodiscard]] bool waitForDeadlineEntry() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, TestTimeout, [this] { return deadlineEntered_; });
    }

    void releaseDeadline() noexcept
    {
        const std::scoped_lock lock{mutex_};
        releaseDeadline_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t deadlineCount() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return deadlineCount_;
    }

    [[nodiscard]] std::size_t shutdownCount() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return shutdownCount_;
    }

    [[nodiscard]] std::optional<Windows::WindowsDashboardDeadline>
    lastDeadline() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return lastDeadline_;
    }

private:
    const std::uint64_t registrationId_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Windows::WindowsDashboardDeadline> lastDeadline_;
    std::size_t deadlineCount_{};
    std::size_t shutdownCount_{};
    bool blockDeadline_{};
    bool deadlineEntered_{};
    bool releaseDeadline_{};
};

class RecordingGenerationDrainObserver final
    : public Detail::IDashboardConnectionGenerationDrainObserver {
public:
    void attachRegistry(const std::shared_ptr<Registry>& registry) noexcept
    {
        registry_ = registry;
    }

    void generationConnectionsMayHaveDrained(
        const std::uint64_t generationId) noexcept override
    {
        lastGeneration_.store(generationId, std::memory_order_release);
        callbackCount_.fetch_add(1U, std::memory_order_acq_rel);
        if (auto registry = registry_.lock(); registry != nullptr) {
            static_cast<void>(registry->snapshot());
            reentered_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] std::size_t callbackCount() const noexcept
    {
        return callbackCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t lastGeneration() const noexcept
    {
        return lastGeneration_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool reentered() const noexcept
    {
        return reentered_.load(std::memory_order_acquire);
    }

private:
    std::weak_ptr<Registry> registry_;
    std::atomic_size_t callbackCount_{};
    std::atomic_uint64_t lastGeneration_{};
    std::atomic_bool reentered_{};
};

class FakeCompletionPortApi final : public Api {
public:
    struct QueuedPacket final {
        DWORD transferredBytes{};
        ULONG_PTR completionKey{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(0xC011EC7U));
    }

    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE fileHandle,
        const HANDLE existingCompletionPort,
        const ULONG_PTR,
        const DWORD) noexcept override
    {
        return fileHandle == INVALID_HANDLE_VALUE
            ? portHandle()
            : existingCompletionPort;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        const HANDLE completionPort,
        const DWORD transferredBytes,
        const ULONG_PTR completionKey,
        OVERLAPPED* const operation) noexcept override
    {
        std::unique_lock lock{mutex_};
        if (completionPort != portHandle()) {
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        if (completionKey != Kernel::ShutdownKeyValue &&
            blockNextDataPost_) {
            blockNextDataPost_ = false;
            blockedDataPostEntered_ = true;
            changed_.notify_all();
            static_cast<void>(changed_.wait_for(
                lock,
                TestTimeout,
                [this] { return releaseBlockedDataPost_; }));
        }
        if (completionKey == Kernel::ShutdownKeyValue) {
            controls_.push_back(
                QueuedPacket{transferredBytes, completionKey, operation});
        } else {
            data_.push_back(
                QueuedPacket{transferredBytes, completionKey, operation});
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
        const auto ready = [this] { return closed_ || !controls_.empty(); };
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
        if (controls_.empty()) {
            transferredBytes = 0U;
            completionKey = 0U;
            operation = nullptr;
            setThreadError(WAIT_TIMEOUT);
            return FALSE;
        }
        const auto packet = controls_.front();
        controls_.pop_front();
        transferredBytes = packet.transferredBytes;
        completionKey = packet.completionKey;
        operation = packet.operation;
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
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override { return threadError_; }

    [[nodiscard]] std::optional<QueuedPacket> takeDataPacket() noexcept
    {
        const std::lock_guard lock{mutex_};
        if (data_.empty()) {
            return std::nullopt;
        }
        const auto packet = data_.front();
        data_.pop_front();
        return packet;
    }

    void blockNextDataPost() noexcept
    {
        const std::lock_guard lock{mutex_};
        blockNextDataPost_ = true;
        blockedDataPostEntered_ = false;
        releaseBlockedDataPost_ = false;
    }

    [[nodiscard]] bool waitForBlockedDataPost() noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            TestTimeout,
            [this] { return blockedDataPostEntered_; });
    }

    void releaseBlockedDataPost() noexcept
    {
        const std::lock_guard lock{mutex_};
        releaseBlockedDataPost_ = true;
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
    std::deque<QueuedPacket> data_;
    std::deque<QueuedPacket> controls_;
    bool blockNextDataPost_{};
    bool blockedDataPostEntered_{};
    bool releaseBlockedDataPost_{};
    bool closed_{};
};

struct DeadlineFixture final {
    explicit DeadlineFixture(
        const Key bridgeKey = DeadlineKey,
        const bool bind = true)
        : api{std::make_shared<FakeCompletionPortApi>()},
          registry{take(Registry::create(DeadlineKey))}
    {
        auto port = take(Port::create(api));
        kernel = take(Kernel::create(std::move(port), registry));
        bridge = take(Bridge::create(*kernel, bridgeKey));
        if (bind) {
            auto bound = registry->bindDeadlineBridge(bridge);
            require(static_cast<bool>(bound),
                    "deadline fixture could not bind its bridge");
        }
    }

    ~DeadlineFixture() noexcept
    {
        if (registry != nullptr) {
            registry->beginShutdown();
            while (const auto packet = api->takeDataPacket()) {
                registry->consume(
                    Packet{
                        packet->transferredBytes,
                        Key{static_cast<std::uintptr_t>(
                            packet->completionKey)},
                        packet->operation},
                    ERROR_SUCCESS);
            }
            static_cast<void>(registry->finalizeDeadlineRouting());
        }
        bridge.reset();
        if (kernel != nullptr) {
            kernel->shutdown();
        }
        registry.reset();
        kernel.reset();
    }

    [[nodiscard]] FakeCompletionPortApi::QueuedPacket takePacket()
    {
        const auto packet = api->takeDataPacket();
        if (!packet.has_value()) {
            fail("deadline bridge did not post a packet");
        }
        return *packet;
    }

    std::shared_ptr<FakeCompletionPortApi> api;
    std::shared_ptr<Registry> registry;
    std::unique_ptr<Kernel> kernel;
    std::shared_ptr<Bridge> bridge;
};

class BlockingForwardingDeadlineSink final
    : public Windows::IWindowsDashboardDeadlineSink {
public:
    explicit BlockingForwardingDeadlineSink(
        std::shared_ptr<Bridge> bridge) noexcept
        : bridge_{std::move(bridge)}
    {
    }

    ~BlockingForwardingDeadlineSink() noexcept override { release(); }

    void signal(Windows::WindowsDashboardDeadline deadline) noexcept override
    {
        try {
            {
                std::unique_lock lock{mutex_};
                entered_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this] { return released_; });
            }
            bridge_->signal(std::move(deadline));
            {
                const std::lock_guard lock{mutex_};
                completed_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitUntilEntered() noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock, TestTimeout, [this] { return entered_; });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool waitUntilCompleted() noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock, TestTimeout, [this] { return completed_; });
        } catch (...) {
            return false;
        }
    }

    void release() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

private:
    const std::shared_ptr<Bridge> bridge_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
    bool completed_{};
};

[[nodiscard]] std::shared_ptr<FakeTarget> target(
    const std::uintptr_t key,
    const std::uint64_t registration,
    const std::uint64_t generation = 1U)
{
    return std::make_shared<FakeTarget>(
        Key{key}, registration, generation);
}

void validatesFixedContractsAndReservedKey()
{
    auto registry = take(Registry::create(DeadlineKey));
    const auto initial = registry->snapshot();
    require(initial.registeredConnectionCount() == 0U,
            "new registry was not empty");
    require(initial.maximumConnectionCount() == 40U,
            "registry capacity changed");
    require(initial.deadlineCompletionKey() == DeadlineKey,
            "snapshot lost deadline key");
    require(!initial.deadlineBridgeBound() &&
                !initial.isShuttingDown() && !initial.isFatal(),
            "new registry started bound or closed");

    auto reserved = Registry::create(Key{Kernel::ShutdownKeyValue});
    require(!reserved, "reserved shutdown key was accepted");
    require(reserved.error().code == Domain::ErrorCodes::InvalidRequest,
            "reserved key used the wrong error");
    auto missingFailFast = Registry::create(DeadlineKey, nullptr);
    require(!missingFailFast &&
                missingFailFast.error().code ==
                    Domain::ErrorCodes::InvalidRequest,
            "null registry fail-fast boundary was accepted");
    registry->beginShutdown();
}

void insertsBeforeStartAndRoutesImmediateCompletion()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto owner = target(101U, 1001U, 11U);
    owner->attachRegistry(registry);
    owner->routeOneCompletionDuringStart();
    auto registered = registry->registerConnection(owner);
    require(static_cast<bool>(registered),
            "register-before-start owner was rejected");
    require(owner->startCount() == 1U && owner->iocpCount() == 1U,
            "immediate start completion was not routed");
    require(owner->lastTransferredBytes() == 17U &&
                owner->lastNativeError() == ERROR_SUCCESS,
            "immediate completion fields changed");
    require(registry->snapshot().registeredConnectionCount() == 1U,
            "owner was not retained after start");
    owner->markDrained();
    require(registry->removeIfDrained(owner),
            "drained immediate owner was not removed");
    registry->beginShutdown();
}

void rejectsConnectionUntilDeadlineBridgeIsBound()
{
    auto registry = take(Registry::create(DeadlineKey));
    auto owner = target(100U, 1'000U, 10U);
    auto registered = registry->registerConnection(owner);
    require(!registered &&
                registered.error().code == Domain::ErrorCodes::Conflict,
            "an unbound registry accepted a connection owner");
    require(owner->startCount() == 0U && owner->shutdownCount() == 1U,
            "an unbound registry started or failed to close its owner");
    const auto snapshot = registry->snapshot();
    require(snapshot.registeredConnectionCount() == 0U &&
                snapshot.isFatal(),
            "an unbound connection admission leaked state or was not fatal");
}

void retainsStartFailureUntilExactDrain()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto owner = target(102U, 1002U, 12U);
    owner->failStart();
    auto registered = registry->registerConnection(owner);
    require(!registered, "fake start failure was accepted");
    require(registered.error().code == Domain::ErrorCodes::TransportClosed,
            "start failure changed classification");
    require(owner->shutdownCount() == 1U,
            "start failure did not request owner shutdown");
    require(registry->snapshot().registeredConnectionCount() == 1U,
            "undrained start failure was released early");
    const auto retiredAfterStartFailure = fixture.bridge->snapshot();
    require(retiredAfterStartFailure.registeredOwnerCount() == 0U &&
                retiredAfterStartFailure.postedOperationCount() == 0U &&
                retiredAfterStartFailure.retiredAwaitingReapCount() == 0U,
            "start failure leaked its exact deadline owner");
    owner->markDrained();
    require(registry->removeIfDrained(owner),
            "drained start failure was not removed");
    registry->beginShutdown();
}

void acceptsOutOfOrderUniqueRegistrationsConcurrently()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto higher = target(151U, 15'002U, 15U);
    auto lower = target(150U, 15'001U, 15U);
    higher->blockStart();

    std::atomic<bool> higherSucceeded{};
    std::thread higherThread{[&] {
        auto registered = registry->registerConnection(higher);
        higherSucceeded.store(static_cast<bool>(registered));
    }};
    require(higher->waitForStartEntry(),
            "higher unique registration did not reach start");
    require(fixture.bridge->snapshot().registeredOwnerCount() == 1U,
            "deadline owner was not registered before target start entered");

    auto lowerRegistered = registry->registerConnection(lower);
    require(static_cast<bool>(lowerRegistered),
            "lower unique registration was rejected while higher start was active");
    higher->releaseStart();
    higherThread.join();

    const auto snapshot = registry->snapshot();
    require(higherSucceeded.load() &&
                snapshot.registeredConnectionCount() == 2U &&
                !snapshot.isFatal(),
            "out-of-order unique concurrent registration changed fatal state");
    registry->beginShutdown();
    require(registry->snapshot().registeredConnectionCount() == 0U,
            "out-of-order owners did not shutdown and drain");
}

void enforcesExactlyFortyWithoutFatalOverload()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    std::array<std::shared_ptr<FakeTarget>, 41U> owners;
    for (std::size_t index{}; index < Registry::MaximumConnectionCount;
         ++index) {
        owners[index] = target(
            200U + index, 2'000U + index, 21U);
        auto registered = registry->registerConnection(owners[index]);
        require(static_cast<bool>(registered),
                "one of forty fixed owners was rejected");
    }
    owners.back() = target(999U, 9'999U, 21U);
    auto overflow = registry->registerConnection(owners.back());
    require(!overflow, "forty-first owner was accepted");
    require(overflow.error().code == Domain::ErrorCodes::LimitExceeded &&
                overflow.error().retryable,
            "capacity rejection used the wrong bounded error");
    const auto full = registry->snapshot();
    require(full.registeredConnectionCount() == 40U && !full.isFatal(),
            "capacity rejection corrupted registry state");
    require(fixture.bridge->snapshot().registeredOwnerCount() == 40U,
            "capacity rejection leaked an uncommitted deadline owner");
    registry->beginShutdown();
    require(registry->snapshot().registeredConnectionCount() == 0U,
            "shutdown did not remove forty synchronously drained owners");
    const auto drained = fixture.bridge->snapshot();
    require(drained.registeredOwnerCount() == 0U &&
                drained.postedOperationCount() == 0U &&
                drained.retiredAwaitingReapCount() == 0U,
            "shutdown did not release all forty deadline owners");
}

void duplicateIdentityIsFatalAndClosesOutsideLock()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto first = target(301U, 3001U, 31U);
    first->attachRegistry(registry);
    first->reenterRegistryDuringShutdown();
    require(static_cast<bool>(registry->registerConnection(first)),
            "first duplicate-test owner was rejected");
    auto duplicate = target(301U, 3002U, 32U);
    auto second = registry->registerConnection(duplicate);
    require(!second && second.error().code == Domain::ErrorCodes::Conflict,
            "duplicate key did not return conflict");
    const auto snapshot = registry->snapshot();
    require(snapshot.isFatal() && snapshot.isShuttingDown(),
            "duplicate identity did not retain fatal shutdown");
    require(snapshot.failure() != nullptr &&
                snapshot.failure()->kind == RegistryFailureKind::Conflict,
            "duplicate identity retained the wrong fixed failure");
    require(first->reenteredShutdown(),
            "duplicate shutdown callback ran under the registry lock");
    require(snapshot.registeredConnectionCount() == 0U,
            "duplicate shutdown retained a drained owner");
    require(fixture.bridge->snapshot().registeredOwnerCount() == 0U,
            "duplicate cleanup leaked a deadline owner slot");
}

void routesConnectionAndRejectsStaleRemovalIdentity()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto first = target(401U, 4001U, 41U);
    first->drainDuringIocp();
    require(static_cast<bool>(registry->registerConnection(first)),
            "connection route owner was rejected");
    OVERLAPPED operation{};
    registry->consume(Packet{29U, Key{401U}, &operation}, ERROR_NETNAME_DELETED);
    require(first->iocpCount() == 1U &&
                first->lastOperation() == &operation &&
                first->lastTransferredBytes() == 29U &&
                first->lastNativeError() == ERROR_NETNAME_DELETED,
            "connection packet fields were not routed exactly");
    require(registry->snapshot().registeredConnectionCount() == 0U,
            "drained connection was not automatically removed");

    auto replacement = target(401U, 4002U, 42U);
    require(static_cast<bool>(registry->registerConnection(replacement)),
            "replacement exact identity was rejected after removal");
    require(!registry->removeIfDrained(first),
            "stale target removed its replacement");
    require(registry->snapshot().registeredConnectionCount() == 1U,
            "stale removal mutated replacement entry");
    replacement->markDrained();
    require(registry->removeIfDrained(replacement),
            "replacement did not remove by exact object identity");
    registry->beginShutdown();
}

void malformedAndUnknownRoutingRetainFatalState()
{
    {
        auto registry = take(Registry::create(DeadlineKey));
        OVERLAPPED operation{};
        registry->consume(Packet{0U, Key{777U}, &operation}, ERROR_SUCCESS);
        const auto snapshot = registry->snapshot();
        require(snapshot.isFatal() && snapshot.failure() != nullptr &&
                    snapshot.failure()->kind ==
                        RegistryFailureKind::IntegrityFailure,
                "unknown connection key did not retain integrity failure");
    }
    {
        DeadlineFixture fixture;
        auto registry = fixture.registry;
        auto owner = target(501U, 5001U, 51U);
        require(static_cast<bool>(registry->registerConnection(owner)),
                "malformed-packet owner was rejected");
        registry->consume(Packet{0U, Key{501U}, nullptr}, ERROR_SUCCESS);
        require(registry->snapshot().isFatal() && owner->iocpCount() == 0U,
                "null connection operation was dispatched or not fatal");
    }
    {
        auto registry = take(Registry::create(DeadlineKey));
        registry->fatal(EventNotification{
            88'888U,
            EventFailure{EventFailureKind::IntegrityFailure, false}});
        require(registry->snapshot().isFatal(),
                "unknown event fatal owner was ignored");
    }
    {
        auto failFast = std::make_shared<RecordingRegistryFailFast>();
        auto registry = take(Registry::create(DeadlineKey, failFast));
        registry->fatal(ERROR_INVALID_HANDLE);
        const auto snapshot = registry->snapshot();
        require(snapshot.isFatal() && snapshot.failure() != nullptr &&
                    snapshot.failure()->nativeError ==
                        static_cast<DWORD>(ERROR_INVALID_HANDLE) &&
                    failFast->calls() == 1U,
                "kernel native fatal error was not retained exactly");
    }
}

void knownEventFatalShutsDownReentrantly()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto owner = target(601U, 6001U, 61U);
    owner->attachRegistry(registry);
    owner->reenterRegistryDuringShutdown();
    require(static_cast<bool>(registry->registerConnection(owner)),
            "event-fatal owner was rejected");
    registry->fatal(EventNotification{
        6001U,
        EventFailure{EventFailureKind::TransportClosed, true}});
    const auto snapshot = registry->snapshot();
    require(snapshot.isFatal() && snapshot.failure() != nullptr &&
                snapshot.failure()->kind ==
                    RegistryFailureKind::TransportClosed &&
                snapshot.failure()->retryable,
            "known event fatal classification changed");
    require(owner->shutdownCount() == 1U && owner->reenteredShutdown(),
            "known event fatal callback was missing or locked");
    require(snapshot.registeredConnectionCount() == 0U,
            "known event fatal retained a drained owner");
}

void destroysAutoRemovedTargetOnlyAfterUnlock()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    std::atomic<std::size_t> destructorCount{};
    std::atomic<bool> destructorReentered{};
    auto owner = target(701U, 7001U, 71U);
    owner->drainDuringIocp();
    owner->reenterRegistryOnDestruction(
        registry, destructorCount, destructorReentered);
    require(static_cast<bool>(registry->registerConnection(owner)),
            "destructor-reentry owner was rejected");
    owner.reset();
    OVERLAPPED operation{};
    registry->consume(Packet{0U, Key{701U}, &operation}, ERROR_SUCCESS);
    require(destructorCount.load() == 1U,
            "auto-removed target was not destroyed exactly once");
    require(destructorReentered.load(),
            "target destructor could not reenter the unlocked registry");
    require(registry->snapshot().registeredConnectionCount() == 0U,
            "destructor-reentry owner remained registered");
    registry->beginShutdown();
}

void shutdownAndConcurrentRoutingDoNotShareRegistryLock()
{
    DeadlineFixture fixture;
    auto registry = fixture.registry;
    auto owner = target(801U, 8001U, 81U);
    owner->retainDuringShutdown();
    owner->drainDuringIocp();
    owner->blockIocp();
    require(static_cast<bool>(registry->registerConnection(owner)),
            "concurrency owner was rejected");
    OVERLAPPED operation{};
    std::thread routeThread{[&] {
        registry->consume(
            Packet{3U, Key{801U}, &operation}, ERROR_SUCCESS);
    }};
    require(owner->waitForIocpEntry(),
            "concurrent dispatch did not enter");

    std::atomic<bool> shutdownReturned{};
    std::thread shutdownThread{[&] {
        registry->beginShutdown();
        shutdownReturned.store(true);
    }};
    require(owner->waitForShutdownEntry(),
            "concurrent shutdown did not reach the target");
    const auto during = registry->snapshot();
    require(during.isShuttingDown(),
            "concurrent shutdown did not publish closed state");
    owner->releaseIocp();
    routeThread.join();
    shutdownThread.join();
    require(shutdownReturned.load() && owner->shutdownCount() == 1U,
            "concurrent shutdown did not complete exactly once");
    require(registry->snapshot().registeredConnectionCount() == 0U,
            "concurrent drain did not remove the exact owner");
}

void validatesOneShotAndExactDeadlineBridgeBinding()
{
    {
        DeadlineFixture fixture{OtherDeadlineKey, false};
        auto mismatched = fixture.registry->bindDeadlineBridge(fixture.bridge);
        require(!mismatched &&
                    mismatched.error().code ==
                        Domain::ErrorCodes::InvalidRequest,
                "mismatched deadline bridge key was accepted");
        require(!fixture.registry->snapshot().deadlineBridgeBound() &&
                    fixture.registry->snapshot().isFatal(),
                "mismatched bridge was published or not retained fatally");
    }
    {
        DeadlineFixture fixture;
        require(fixture.bridge->completionKey() == DeadlineKey,
                "deadline bridge accessor changed its exact key");
        auto duplicate =
            fixture.registry->bindDeadlineBridge(fixture.bridge);
        require(!duplicate &&
                    duplicate.error().code == Domain::ErrorCodes::Conflict,
                "second deadline bridge binding was accepted");
        require(fixture.registry->snapshot().isFatal(),
                "duplicate deadline binding was not fatal");
    }
}

[[nodiscard]] Windows::WindowsDashboardDeadline deadline(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence) noexcept
{
    return Windows::WindowsDashboardDeadline{
        registrationId,
        armSequence,
        Windows::WindowsDashboardDeadlineKind::HeaderIngress,
        Domain::MonotonicTimePoint{std::chrono::seconds{30}}};
}

void routesLiveAndRetiredDeadlinePackets()
{
    DeadlineFixture fixture;
    auto owner = target(901U, 9001U, 91U);
    require(static_cast<bool>(fixture.registry->registerConnection(owner)),
            "deadline owner was rejected");
    fixture.bridge->signal(deadline(9001U, 1U));
    const auto live = fixture.takePacket();
    fixture.registry->consume(
        Packet{
            live.transferredBytes,
            Key{static_cast<std::uintptr_t>(live.completionKey)},
            live.operation},
        ERROR_SUCCESS);
    const auto delivered = owner->lastDeadline();
    require(owner->deadlineCount() == 1U && delivered.has_value() &&
                delivered->registrationId == 9001U &&
                delivered->armSequence == 1U,
            "live deadline did not reach its exact registration");
    require(fixture.registry->snapshot().deadlineDispatchCount() == 1U,
            "live deadline dispatch count changed");
    fixture.bridge->signal(deadline(9001U, 2U));
    const auto retired = fixture.takePacket();
    owner->markDrained();
    require(fixture.registry->removeIfDrained(owner),
            "posted deadline owner could not retire through registry removal");
    const auto awaitingReap = fixture.bridge->snapshot();
    require(awaitingReap.registeredOwnerCount() == 0U &&
                awaitingReap.postedOperationCount() == 1U &&
                awaitingReap.retiredAwaitingReapCount() == 1U,
            "signal-first retirement did not retain one bounded tombstone");
    fixture.registry->consume(
        Packet{
            retired.transferredBytes,
            Key{static_cast<std::uintptr_t>(retired.completionKey)},
            retired.operation},
        ERROR_SUCCESS);
    require(owner->deadlineCount() == 1U,
            "retired deadline tombstone reached the target");
    require(fixture.registry->snapshot().retiredDeadlineDrainCount() == 1U,
            "retired deadline drain was not counted");
    const auto reaped = fixture.bridge->snapshot();
    require(reaped.postedOperationCount() == 0U &&
                reaped.retiredAwaitingReapCount() == 0U &&
                reaped.registeredOwnerCount() == 0U,
            "retired notification did not release its fixed bridge slot");
}

void forceClosesOnlyTheExactListenerGeneration()
{
    DeadlineFixture fixture;
    auto observer =
        std::make_shared<RecordingGenerationDrainObserver>();
    observer->attachRegistry(fixture.registry);
    require(static_cast<bool>(
                fixture.registry->bindGenerationDrainObserver(observer)),
            "generation-drain observer could not bind");
    auto first = target(451U, 4'501U, 45U);
    auto second = target(452U, 4'502U, 45U);
    auto other = target(453U, 4'503U, 46U);
    require(static_cast<bool>(fixture.registry->registerConnection(first)) &&
                static_cast<bool>(
                    fixture.registry->registerConnection(second)) &&
                static_cast<bool>(
                    fixture.registry->registerConnection(other)),
            "generation-close fixtures were not registered");
    require(fixture.registry->connectionCountForGeneration(45U) == 2U &&
                fixture.registry->connectionCountForGeneration(46U) == 1U,
            "generation connection counts did not preserve ownership");

    require(fixture.registry->beginShutdownGeneration(45U) == 2U,
            "exact generation shutdown did not select both owners");
    require(first->shutdownCount() == 1U &&
                second->shutdownCount() == 1U &&
                other->shutdownCount() == 0U,
            "generation shutdown crossed its listener identity");
    require(fixture.registry->connectionCountForGeneration(45U) == 0U &&
                fixture.registry->connectionCountForGeneration(46U) == 1U &&
                fixture.registry->snapshot().registeredConnectionCount() ==
                    1U,
            "generation shutdown removed the wrong registry entries");
    require(observer->callbackCount() == 1U &&
                observer->lastGeneration() == 45U &&
                observer->reentered(),
            "zero-generation drain edge was not invoked once outside the registry lock");
    require(fixture.registry->beginShutdownGeneration(45U) == 0U &&
                first->shutdownCount() == 1U &&
                second->shutdownCount() == 1U &&
                observer->callbackCount() == 1U,
            "repeated generation shutdown redelivered stale ownership");
}

void offRegistryDrainEdgeRemovesTheExactOwnerWithoutPolling()
{
    DeadlineFixture fixture;
    auto observer =
        std::make_shared<RecordingGenerationDrainObserver>();
    observer->attachRegistry(fixture.registry);
    require(static_cast<bool>(
                fixture.registry->bindGenerationDrainObserver(observer)),
            "off-registry generation observer could not bind");

    auto owner = target(471U, 4'701U, 47U);
    require(static_cast<bool>(fixture.registry->registerConnection(owner)),
            "off-registry drain owner was rejected");
    owner->markDrainedAndNotify();

    require(fixture.registry->snapshot().registeredConnectionCount() == 0U &&
                fixture.bridge->snapshot().registeredOwnerCount() == 0U,
            "off-registry drain edge retained registry or deadline ownership");
    require(observer->callbackCount() == 1U &&
                observer->lastGeneration() == 47U && observer->reentered(),
            "off-registry drain did not publish the exact generation-zero edge outside locks");

    owner->markDrainedAndNotify();
    fixture.registry->connectionMayHaveDrained(
        Key{owner->completionKey().value() + 1U},
        owner->registrationId(),
        owner->generationId());
    require(fixture.registry->snapshot().registeredConnectionCount() == 0U &&
                observer->callbackCount() == 1U,
            "stale or foreign drain identity redelivered removal");
}

[[nodiscard]] Windows::WindowsDashboardDeadline listenerDeadline(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence) noexcept
{
    return Windows::WindowsDashboardDeadline{
        registrationId,
        armSequence,
        Windows::WindowsDashboardDeadlineKind::ListenerRetirement,
        Domain::MonotonicTimePoint{std::chrono::seconds{45}}};
}

[[nodiscard]] Windows::WindowsDashboardDeadline overloadDeadline(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence) noexcept
{
    return Windows::WindowsDashboardDeadline{
        registrationId,
        armSequence,
        Windows::WindowsDashboardDeadlineKind::OverloadResponse,
        Domain::MonotonicTimePoint{std::chrono::seconds{50}}};
}

void routesAndExactlyRetiresAuxiliaryDeadlineOwners()
{
    DeadlineFixture fixture;
    auto owner = std::make_shared<FakeAuxiliaryDeadlineTarget>(90'101U);
    require(static_cast<bool>(
                fixture.registry->registerAuxiliaryDeadlineTarget(owner)),
            "auxiliary deadline owner was rejected");
    require(
        fixture.registry->snapshot().
                registeredAuxiliaryDeadlineTargetCount() == 1U &&
            fixture.bridge->snapshot().registeredOwnerCount() == 1U,
        "auxiliary deadline ownership was not committed to both registries");

    fixture.bridge->signal(listenerDeadline(owner->registrationId(), 7U));
    const auto packet = fixture.takePacket();
    fixture.registry->consume(
        Packet{
            packet.transferredBytes,
            Key{static_cast<std::uintptr_t>(packet.completionKey)},
            packet.operation},
        ERROR_SUCCESS);
    const auto delivered = owner->lastDeadline();
    require(owner->deadlineCount() == 1U && delivered.has_value() &&
                delivered->registrationId == owner->registrationId() &&
                delivered->armSequence == 7U &&
                delivered->kind == Windows::
                    WindowsDashboardDeadlineKind::ListenerRetirement,
            "listener retirement deadline did not reach its exact owner");

    auto imposter =
        std::make_shared<FakeAuxiliaryDeadlineTarget>(
            owner->registrationId());
    require(!fixture.registry->unregisterAuxiliaryDeadlineTarget(imposter),
            "same-registration auxiliary imposter retired the live owner");
    require(fixture.registry->unregisterAuxiliaryDeadlineTarget(owner),
            "exact auxiliary deadline owner did not retire");
    require(
        fixture.registry->snapshot().
                registeredAuxiliaryDeadlineTargetCount() == 0U &&
            fixture.bridge->snapshot().registeredOwnerCount() == 0U,
        "auxiliary deadline retirement retained registry ownership");
}

void auxiliarySignalFirstRetirementDrainsATombstone()
{
    DeadlineFixture fixture;
    auto owner = std::make_shared<FakeAuxiliaryDeadlineTarget>(90'201U);
    require(static_cast<bool>(
                fixture.registry->registerAuxiliaryDeadlineTarget(owner)),
            "signal-first auxiliary owner was rejected");
    fixture.bridge->signal(listenerDeadline(owner->registrationId(), 9U));
    const auto packet = fixture.takePacket();
    require(fixture.registry->unregisterAuxiliaryDeadlineTarget(owner),
            "signal-first auxiliary owner did not retire");
    require(fixture.bridge->snapshot().retiredAwaitingReapCount() == 1U,
            "signal-first auxiliary retirement lost its posted tombstone");

    fixture.registry->consume(
        Packet{
            packet.transferredBytes,
            Key{static_cast<std::uintptr_t>(packet.completionKey)},
            packet.operation},
        ERROR_SUCCESS);
    require(owner->deadlineCount() == 0U,
            "retired auxiliary deadline reached its old owner");
    require(fixture.registry->snapshot().retiredDeadlineDrainCount() == 1U &&
                fixture.bridge->snapshot().postedOperationCount() == 0U,
            "auxiliary deadline tombstone did not drain exactly once");
}

void auxiliaryRoutingPinsTheOwnerAcrossConcurrentUnregistration()
{
    DeadlineFixture fixture;
    auto owner = std::make_shared<FakeAuxiliaryDeadlineTarget>(90'301U);
    owner->blockDeadline();
    require(static_cast<bool>(
                fixture.registry->registerAuxiliaryDeadlineTarget(owner)),
            "blocking auxiliary owner was rejected");
    fixture.bridge->signal(listenerDeadline(owner->registrationId(), 11U));
    const auto packet = fixture.takePacket();

    std::thread consumeThread{[&] {
        fixture.registry->consume(
            Packet{
                packet.transferredBytes,
                Key{static_cast<std::uintptr_t>(packet.completionKey)},
                packet.operation},
            ERROR_SUCCESS);
    }};
    require(owner->waitForDeadlineEntry(),
            "auxiliary deadline callback did not enter");
    require(fixture.registry->unregisterAuxiliaryDeadlineTarget(owner),
            "live auxiliary callback owner could not unregister");
    auto* const borrowed = owner.get();
    std::weak_ptr<FakeAuxiliaryDeadlineTarget> weakOwner{owner};
    owner.reset();
    require(!weakOwner.expired(),
            "unregistration destroyed a running auxiliary callback owner");
    borrowed->releaseDeadline();
    consumeThread.join();
    require(weakOwner.expired(),
            "auxiliary callback pin outlived completed dispatch");
}

void auxiliaryCapacityAndShutdownStayFixedAndNonfatal()
{
    DeadlineFixture fixture;
    std::array<
        std::shared_ptr<FakeAuxiliaryDeadlineTarget>,
        Registry::MaximumAuxiliaryDeadlineTargetCount> owners{
        std::make_shared<FakeAuxiliaryDeadlineTarget>(90'401U),
        std::make_shared<FakeAuxiliaryDeadlineTarget>(90'402U),
        std::make_shared<FakeAuxiliaryDeadlineTarget>(90'403U),
        std::make_shared<FakeAuxiliaryDeadlineTarget>(90'404U)};
    for (const auto& owner : owners) {
        require(static_cast<bool>(
                    fixture.registry->registerAuxiliaryDeadlineTarget(owner)),
                "fixed auxiliary deadline owner was rejected");
    }
    auto overflow =
        std::make_shared<FakeAuxiliaryDeadlineTarget>(90'405U);
    const auto rejected =
        fixture.registry->registerAuxiliaryDeadlineTarget(overflow);
    require(!rejected &&
                rejected.error().code ==
                    Domain::ErrorCodes::LimitExceeded &&
                rejected.error().retryable &&
                overflow->shutdownCount() == 1U,
            "fifth auxiliary owner was not rejected and closed safely");
    require(!fixture.registry->snapshot().isFatal(),
            "ordinary auxiliary capacity rejection became fatal");

    fixture.registry->beginShutdown();
    for (const auto& owner : owners) {
        require(owner->shutdownCount() == 1U,
                "registered auxiliary owner did not receive shutdown once");
    }
    require(
        fixture.registry->snapshot().
                registeredAuxiliaryDeadlineTargetCount() ==
                    Registry::MaximumAuxiliaryDeadlineTargetCount &&
            fixture.bridge->snapshot().registeredOwnerCount() ==
                Registry::MaximumAuxiliaryDeadlineTargetCount &&
            !fixture.bridge->snapshot().isShutdown() &&
            !fixture.registry->finalizeDeadlineRouting(),
        "registry shutdown retired live auxiliary watchdog routing");

    fixture.bridge->signal(
        overloadDeadline(owners[0U]->registrationId(), 21U));
    fixture.bridge->signal(
        listenerDeadline(owners[1U]->registrationId(), 22U));
    for (std::size_t index{}; index < 2U; ++index) {
        const auto packet = fixture.takePacket();
        fixture.registry->consume(
            Packet{
                packet.transferredBytes,
                Key{static_cast<std::uintptr_t>(packet.completionKey)},
                packet.operation},
            ERROR_SUCCESS);
    }
    require(owners[0U]->deadlineCount() == 1U &&
                owners[0U]->lastDeadline().has_value() &&
                owners[0U]->lastDeadline()->kind == Windows::
                    WindowsDashboardDeadlineKind::OverloadResponse &&
                owners[1U]->deadlineCount() == 1U &&
                owners[1U]->lastDeadline().has_value() &&
                owners[1U]->lastDeadline()->kind == Windows::
                    WindowsDashboardDeadlineKind::ListenerRetirement,
            "shutdown did not route retained overload and listener watchdogs");
    for (const auto& owner : owners) {
        require(
            fixture.registry->unregisterAuxiliaryDeadlineTarget(owner),
            "drained auxiliary owner could not unregister after shutdown");
    }
    require(fixture.registry->finalizeDeadlineRouting() &&
                fixture.bridge->snapshot().isShutdown(),
            "drained auxiliary routing did not finalize exactly once");
}

void deadlineRoutingSerializesLiveReapAndConcurrentRetirement()
{
    DeadlineFixture fixture;
    auto owner = target(925U, 9'251U, 92U);
    auto bridgeBlocker = target(926U, 9'252U, 92U);
    owner->blockDeadline();
    require(static_cast<bool>(fixture.registry->registerConnection(owner)),
            "deadline pin-race owner was rejected");
    require(static_cast<bool>(
                fixture.registry->registerConnection(bridgeBlocker)),
            "deadline route blocker was rejected");
    fixture.bridge->signal(deadline(owner->registrationId(), 1U));
    const auto packet = fixture.takePacket();

    fixture.api->blockNextDataPost();
    std::thread blockerThread{[&] {
        fixture.bridge->signal(deadline(bridgeBlocker->registrationId(), 1U));
    }};
    require(fixture.api->waitForBlockedDataPost(),
            "deadline route blocker did not retain the bridge lock");

    owner->markDrained();
    std::thread consumeThread{[&] {
        fixture.registry->consume(
            Packet{
                packet.transferredBytes,
                Key{static_cast<std::uintptr_t>(packet.completionKey)},
                packet.operation},
            ERROR_SUCCESS);
    }};
    require(waitUntil([&] {
                return fixture.registry->snapshot()
                    .deadlineRoutingInProgress();
            }),
            "deadline consumer did not own the routing critical section");

    owner->observeNextDrainCheck();
    std::atomic_bool removalReturned{};
    bool removalSucceeded{};
    std::thread removalThread{[&] {
        removalSucceeded = fixture.registry->removeIfDrained(owner);
        removalReturned.store(true, std::memory_order_release);
    }};
    require(waitUntil([&] { return owner->drainCheckObserved(); }),
            "concurrent retirement did not reach its terminal-state check");
    require(!removalReturned.load(std::memory_order_acquire),
            "retirement passed a live deadline routing critical section");

    fixture.api->releaseBlockedDataPost();
    blockerThread.join();
    require(owner->waitForDeadlineEntry(),
            "live deadline was not pinned before routing was released");
    require(waitUntil([&] {
                return removalReturned.load(std::memory_order_acquire);
            }),
            "retirement did not resume after the live target was pinned");
    removalThread.join();
    require(removalSucceeded,
            "concurrent retirement could not remove the pinned target");
    require(fixture.registry->snapshot().registeredConnectionCount() == 1U &&
                !fixture.registry->snapshot().isFatal() &&
                !fixture.registry->snapshot().deadlineRoutingInProgress(),
            "concurrent retirement lost or fatally misrouted the live owner");

    const std::weak_ptr<FakeTarget> weakOwner = owner;
    owner.reset();
    auto callbackOwner = weakOwner.lock();
    require(callbackOwner != nullptr,
            "registry removal destroyed a target with a live callback pin");
    callbackOwner->releaseDeadline();
    callbackOwner.reset();
    consumeThread.join();
    require(weakOwner.expired(),
            "completed deadline delivery retained a removed target");

    const auto blockerPacket = fixture.takePacket();
    bridgeBlocker->markDrained();
    require(fixture.registry->removeIfDrained(bridgeBlocker),
            "deadline route blocker could not retire");
    fixture.registry->consume(
        Packet{
            blockerPacket.transferredBytes,
            Key{static_cast<std::uintptr_t>(blockerPacket.completionKey)},
            blockerPacket.operation},
        ERROR_SUCCESS);
}

void retireFirstSuppressesAnExtractedSchedulerDeadline()
{
    DeadlineFixture fixture;
    auto owner = target(951U, 9'501U, 95U);
    require(static_cast<bool>(fixture.registry->registerConnection(owner)),
            "retire-first deadline owner was rejected");

    const auto sink = std::make_shared<BlockingForwardingDeadlineSink>(
        fixture.bridge);
    auto scheduledOwner = take(
        Windows::WindowsDashboardDeadlineScheduler::create(
            std::make_shared<TestClock>(), sink, 1U));
    const auto scheduled = take(scheduledOwner->schedule(
        Windows::WindowsDashboardDeadlineRequest{
            owner->registrationId(),
            Windows::WindowsDashboardDeadlineKind::HeaderIngress,
            std::chrono::steady_clock::now() -
                std::chrono::seconds{1}}));
    require(sink->waitUntilEntered(),
            "scheduler did not extract the due deadline into the race barrier");
    require(!scheduledOwner->cancel(
                scheduled.registrationId, scheduled.armSequence),
            "exact cancellation unexpectedly reclaimed an extracted deadline");

    owner->markDrained();
    require(fixture.registry->removeIfDrained(owner),
            "retire-first owner was not removed after exact bridge retirement");
    const auto retired = fixture.bridge->snapshot();
    require(retired.registeredOwnerCount() == 0U &&
                retired.postedOperationCount() == 0U &&
                retired.retiredAwaitingReapCount() == 0U,
            "retire-first ordering retained deadline bridge storage");

    sink->release();
    require(sink->waitUntilCompleted(),
            "late extracted deadline did not finish forwarding");
    const auto afterLateSignal = fixture.bridge->snapshot();
    require(!afterLateSignal.isFatal() &&
                afterLateSignal.successfulPostCount() == 0U &&
                afterLateSignal.registeredOwnerCount() == 0U &&
                afterLateSignal.postedOperationCount() == 0U &&
                afterLateSignal.retiredAwaitingReapCount() == 0U,
            "late scheduler publication was fatal or recreated a retired owner");

    scheduledOwner->shutdown();
    auto replacement = target(952U, 9'502U, 96U);
    require(static_cast<bool>(
                fixture.registry->registerConnection(replacement)),
            "retire-first slot was not reusable by a new unique owner");
    replacement->markDrained();
    require(fixture.registry->removeIfDrained(replacement),
            "replacement owner did not release the reused fixed slot");
}

void failedExactRetirementRetainsTheEntryAndHandle()
{
    DeadlineFixture fixture;
    auto owner = target(961U, 9'601U, 96U);
    require(static_cast<bool>(fixture.registry->registerConnection(owner)),
            "retirement-failure owner was rejected");
    owner->markDrained();

    fixture.bridge->shutdown();
    require(!fixture.registry->removeIfDrained(owner),
            "failed exact deadline retirement erased its target");
    const auto snapshot = fixture.registry->snapshot();
    require(snapshot.registeredConnectionCount() == 1U &&
                snapshot.isFatal() && snapshot.failure() != nullptr &&
                snapshot.failure()->kind ==
                    RegistryFailureKind::IntegrityFailure,
            "failed exact retirement did not retain its entry and fatal state");
    require(owner->shutdownCount() == 1U,
            "failed exact retirement did not close its retained target");
}

void malformedDeadlinePacketDrainsThenFailsFatally()
{
    DeadlineFixture fixture;
    auto owner = target(1'001U, 10'001U, 101U);
    require(static_cast<bool>(fixture.registry->registerConnection(owner)),
            "malformed-deadline owner was rejected");
    fixture.bridge->signal(deadline(10'001U, 1U));
    const auto packet = fixture.takePacket();
    fixture.registry->consume(
        Packet{
            1U,
            Key{static_cast<std::uintptr_t>(packet.completionKey)},
            packet.operation},
        ERROR_SUCCESS);
    const auto snapshot = fixture.registry->snapshot();
    require(snapshot.isFatal() && snapshot.failure() != nullptr &&
                snapshot.failure()->kind ==
                    RegistryFailureKind::IntegrityFailure,
            "malformed exact deadline packet was not fatal");
    require(fixture.bridge->snapshot().postedOperationCount() == 0U,
            "malformed exact deadline packet stranded bridge storage");
    require(owner->deadlineCount() == 0U,
            "malformed deadline packet reached its target");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array TestCases{
    TestCase{"fixed contracts", validatesFixedContractsAndReservedKey},
    TestCase{"deadline bridge required", rejectsConnectionUntilDeadlineBridgeIsBound},
    TestCase{"register before start", insertsBeforeStartAndRoutesImmediateCompletion},
    TestCase{"start failure drain", retainsStartFailureUntilExactDrain},
    TestCase{"out of order registration", acceptsOutOfOrderUniqueRegistrationsConcurrently},
    TestCase{"forty capacity", enforcesExactlyFortyWithoutFatalOverload},
    TestCase{"duplicate fatal", duplicateIdentityIsFatalAndClosesOutsideLock},
    TestCase{"connection route identity", routesConnectionAndRejectsStaleRemovalIdentity},
    TestCase{"generation force close", forceClosesOnlyTheExactListenerGeneration},
    TestCase{"off-registry drain edge", offRegistryDrainEdgeRemovesTheExactOwnerWithoutPolling},
    TestCase{"malformed routes", malformedAndUnknownRoutingRetainFatalState},
    TestCase{"event fatal", knownEventFatalShutsDownReentrantly},
    TestCase{"destructor reentry", destroysAutoRemovedTargetOnlyAfterUnlock},
    TestCase{"shutdown route race", shutdownAndConcurrentRoutingDoNotShareRegistryLock},
    TestCase{"deadline binding", validatesOneShotAndExactDeadlineBridgeBinding},
    TestCase{"deadline routing", routesLiveAndRetiredDeadlinePackets},
    TestCase{"auxiliary deadline routing", routesAndExactlyRetiresAuxiliaryDeadlineOwners},
    TestCase{"auxiliary deadline tombstone", auxiliarySignalFirstRetirementDrainsATombstone},
    TestCase{"auxiliary deadline callback pin", auxiliaryRoutingPinsTheOwnerAcrossConcurrentUnregistration},
    TestCase{"auxiliary deadline capacity", auxiliaryCapacityAndShutdownStayFixedAndNonfatal},
    TestCase{"deadline routing lock order", deadlineRoutingSerializesLiveReapAndConcurrentRetirement},
    TestCase{"deadline retire-first race", retireFirstSuppressesAnExtractedSchedulerDeadline},
    TestCase{"deadline retirement failure", failedExactRetirementRetainsTheEntryAndHandle},
    TestCase{"deadline malformed", malformedDeadlinePacketDrainsThenFailsFatally},
};

} // namespace

int main()
{
    try {
        for (const auto& test : TestCases) {
            test.run();
        }
        std::cout << "DashboardConnectionRegistryTests passed: "
                  << TestCases.size() << " cases, " << assertionCount
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DashboardConnectionRegistryTests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    }
}
