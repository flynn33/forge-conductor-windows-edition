#include "Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include <vector>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Api = Detail::IDashboardIoCompletionPortApi;
using Bridge = Detail::DashboardDeadlineIocpBridge;
using Deadline = Windows::WindowsDashboardDeadline;
using DeadlineKind = Windows::WindowsDashboardDeadlineKind;
using Failure = Detail::DashboardDeadlineIocpFailure;
using FailureKind = Detail::DashboardDeadlineIocpFailureKind;
using FailureObserver =
    Detail::IDashboardDeadlineIocpBridgeFailureObserver;
using Handle = Detail::DashboardDeadlineNotificationHandle;
using Key = Detail::DashboardIoCompletionKey;
using Kernel = Detail::DashboardIocpWorkerKernel;
using Packet = Detail::DashboardIoCompletionPacket;
using Port = Detail::DashboardIoCompletionPort;
using ReapDisposition = Detail::DashboardDeadlineIocpReapDisposition;
using Sink = Detail::IDashboardIocpCompletionSink;

static_assert(std::is_final_v<Bridge>);
static_assert(std::is_base_of_v<
              Windows::IWindowsDashboardDeadlineSink,
              Bridge>);
static_assert(!std::is_copy_constructible_v<Bridge>);
static_assert(!std::is_move_constructible_v<Bridge>);
static_assert(Bridge::SlotCount == 44U);
static_assert(
    Bridge::SlotCount ==
    Windows::WindowsDashboardDeadlineScheduler::HardMaximumScheduledCount);
static_assert(noexcept(Bridge::create(
    std::declval<Kernel&>(), Key{1U})));
static_assert(std::is_abstract_v<FailureObserver>);
static_assert(noexcept(
    std::declval<Bridge&>().bindFailureObserver({})));
static_assert(noexcept(
    std::declval<Bridge&>().deferFailureNotificationDispatch()));
static_assert(std::is_move_constructible_v<
              Bridge::FailureNotificationDeferral>);
static_assert(!std::is_copy_constructible_v<
              Bridge::FailureNotificationDeferral>);
static_assert(noexcept(std::declval<Bridge&>().registerOwner(1U)));
static_assert(noexcept(std::declval<Bridge&>().retireOwner({})));
static_assert(noexcept(std::declval<Bridge&>().signal({})));
static_assert(noexcept(std::declval<Bridge&>().reap(
    Key{1U}, 0U, nullptr, ERROR_SUCCESS)));
static_assert(noexcept(std::declval<const Bridge&>().snapshot()));
static_assert(!noexcept(std::declval<const Bridge&>().fullFailure()));
static_assert(noexcept(std::declval<Bridge&>().shutdown()));

constexpr auto TestTimeout = std::chrono::seconds{5};
constexpr Key DeadlineKey{0x444541444C494E45ULL};

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

[[nodiscard]] Deadline deadline(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence)
{
    return Deadline{
        registrationId,
        armSequence,
        DeadlineKind::SocketLifetime,
        std::chrono::steady_clock::time_point{
            std::chrono::milliseconds{
                static_cast<std::int64_t>(armSequence)}}};
}

class FakeCompletionPortApi final : public Api {
public:
    struct NativePacket final {
        DWORD transferredBytes{};
        ULONG_PTR completionKey{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(0xD34D10C0U));
    }

    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE fileHandle,
        const HANDLE existingCompletionPort,
        const ULONG_PTR completionKey,
        const DWORD concurrentThreadCount) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (fileHandle == INVALID_HANDLE_VALUE) {
            ++createCount_;
            createConcurrency_ = concurrentThreadCount;
            createKey_ = completionKey;
            return portHandle();
        }
        ++associationCount_;
        return existingCompletionPort;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        const HANDLE completionPort,
        const DWORD transferredBytes,
        const ULONG_PTR completionKey,
        OVERLAPPED* const operation) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (completionPort != portHandle()) {
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        if (completionKey == Kernel::ShutdownKeyValue) {
            controlPackets_.push_back(NativePacket{
                transferredBytes, completionKey, operation});
            ++controlPostCount_;
            changed_.notify_all();
            return TRUE;
        }

        ++dataPostAttemptCount_;
        if (failDataPosts_) {
            setThreadError(dataPostFailure_);
            return FALSE;
        }
        dataPackets_.push_back(NativePacket{
            transferredBytes, completionKey, operation});
        ++dataPostSuccessCount_;
        changed_.notify_all();
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
        ++dequeueCallCount_;
        allWaitsBounded_ = allWaitsBounded_ &&
            timeoutMilliseconds == Kernel::WorkerWaitMilliseconds;
        const auto ready = [this] {
            return closed_ || !controlPackets_.empty() ||
                (!holdDataCompletions_ && !dataPackets_.empty());
        };
        if (!ready()) {
            static_cast<void>(changed_.wait_for(
                lock,
                std::chrono::milliseconds{timeoutMilliseconds},
                ready));
        }

        if (completionPort != portHandle() || closed_) {
            transferredBytes = 0U;
            completionKey = 0U;
            operation = nullptr;
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        std::optional<NativePacket> packet;
        if (!controlPackets_.empty()) {
            packet.emplace(controlPackets_.front());
            controlPackets_.pop_front();
        } else if (!holdDataCompletions_ && !dataPackets_.empty()) {
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
        changed_.notify_all();
        return TRUE;
    }

    [[nodiscard]] BOOL closeHandle(const HANDLE handle) noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++closeCount_;
        if (handle != portHandle()) {
            setThreadError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        closed_ = true;
        changed_.notify_all();
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override
    {
        return threadError_;
    }

    void setFailDataPosts(
        const bool fail,
        const DWORD error = ERROR_NOT_ENOUGH_MEMORY) noexcept
    {
        const std::lock_guard lock{mutex_};
        failDataPosts_ = fail;
        dataPostFailure_ = error;
    }

    void releaseDataCompletions() noexcept
    {
        const std::lock_guard lock{mutex_};
        holdDataCompletions_ = false;
        changed_.notify_all();
    }

    void holdDataCompletions() noexcept
    {
        const std::lock_guard lock{mutex_};
        holdDataCompletions_ = true;
    }

    [[nodiscard]] bool waitForPendingData(
        const std::size_t count) const noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock,
                TestTimeout,
                [this, count] { return dataPackets_.size() >= count; });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::optional<NativePacket> takeDataPacket() noexcept
    {
        const std::lock_guard lock{mutex_};
        if (dataPackets_.empty()) {
            return std::nullopt;
        }
        auto packet = dataPackets_.front();
        dataPackets_.pop_front();
        return packet;
    }

    [[nodiscard]] std::vector<NativePacket> takeAllDataPackets()
    {
        const std::lock_guard lock{mutex_};
        std::vector<NativePacket> packets;
        packets.reserve(dataPackets_.size());
        while (!dataPackets_.empty()) {
            packets.push_back(dataPackets_.front());
            dataPackets_.pop_front();
        }
        return packets;
    }

    [[nodiscard]] std::size_t pendingDataCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return dataPackets_.size();
    }

    [[nodiscard]] std::size_t dataPostAttemptCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return dataPostAttemptCount_;
    }

    [[nodiscard]] std::size_t dataPostSuccessCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return dataPostSuccessCount_;
    }

    [[nodiscard]] std::size_t controlPostCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return controlPostCount_;
    }

    [[nodiscard]] bool allWaitsBounded() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return allWaitsBounded_;
    }

private:
    static void setThreadError(const DWORD error) noexcept
    {
        threadError_ = error;
    }

    inline static thread_local DWORD threadError_{ERROR_SUCCESS};

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::deque<NativePacket> dataPackets_;
    std::deque<NativePacket> controlPackets_;
    DWORD createConcurrency_{};
    DWORD dataPostFailure_{ERROR_NOT_ENOUGH_MEMORY};
    ULONG_PTR createKey_{};
    std::size_t createCount_{};
    std::size_t associationCount_{};
    std::size_t dataPostAttemptCount_{};
    std::size_t dataPostSuccessCount_{};
    std::size_t controlPostCount_{};
    std::size_t dequeueCallCount_{};
    std::size_t closeCount_{};
    bool holdDataCompletions_{true};
    bool failDataPosts_{};
    bool allWaitsBounded_{true};
    bool closed_{};
};

class RoutingSink final : public Sink {
public:
    void attach(Bridge& bridge) noexcept
    {
        const std::lock_guard lock{mutex_};
        bridge_ = std::addressof(bridge);
    }

    void detachAndWait() noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            static_cast<void>(changed_.wait_for(
                lock,
                TestTimeout,
                [this] { return inFlight_ == 0U; }));
            bridge_ = nullptr;
        } catch (...) {
            std::terminate();
        }
    }

    void consume(
        const Packet packet,
        const DWORD nativeError) noexcept override
    {
        Bridge* bridge{};
        {
            const std::lock_guard lock{mutex_};
            bridge = bridge_;
            ++inFlight_;
        }

        bool succeeded{};
        bool delivered{};
        std::optional<Deadline> value;
        if (bridge != nullptr) {
            auto reaped = bridge->reap(
                packet.completionKey,
                packet.transferredBytes,
                packet.operation,
                nativeError);
            succeeded = static_cast<bool>(reaped);
            if (reaped) {
                const auto& result = reaped.value();
                delivered = result.disposition() ==
                    ReapDisposition::DeadlineDelivered;
                if (result.deadline() != nullptr) {
                    value.emplace(*result.deadline());
                }
            }
        }

        {
            const std::lock_guard lock{mutex_};
            ++completionCount_;
            if (!succeeded) {
                ++failureCount_;
            }
            if (delivered && value.has_value()) {
                latestDeadline_ = value;
                ++deliveryCount_;
            }
            if (inFlight_ == 0U) {
                std::terminate();
            }
            --inFlight_;
        }
        changed_.notify_all();
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++fatalCount_;
        fatalError_ = nativeError;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitForLatestSequence(
        const std::uint64_t sequence) const noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock,
                TestTimeout,
                [this, sequence] {
                    return latestDeadline_.has_value() &&
                        latestDeadline_->armSequence == sequence;
                });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::optional<Deadline> latestDeadline() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return latestDeadline_;
    }

    [[nodiscard]] std::size_t completionCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return completionCount_;
    }

    [[nodiscard]] std::size_t failureCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return failureCount_;
    }

    [[nodiscard]] std::size_t deliveryCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return deliveryCount_;
    }

    [[nodiscard]] std::size_t fatalCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return fatalCount_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    Bridge* bridge_{};
    std::optional<Deadline> latestDeadline_;
    DWORD fatalError_{};
    std::size_t inFlight_{};
    std::size_t completionCount_{};
    std::size_t failureCount_{};
    std::size_t deliveryCount_{};
    std::size_t fatalCount_{};
};

class RecordingFailureObserver final : public FailureObserver {
public:
    void attach(Bridge& bridge) noexcept
    {
        bridge_ = std::addressof(bridge);
    }

    void dashboardDeadlineIocpBridgeFailed(
        const Failure failure) noexcept override
    {
        const bool observedFatal = bridge_ != nullptr &&
            bridge_->snapshot().isFatal();
        const std::lock_guard lock{mutex_};
        ++callbackCount_;
        if (!firstFailure_.has_value()) {
            firstFailure_ = failure;
        }
        reenteredBridge_ = bridge_ != nullptr;
        observedFatal_ = observedFatal;
    }

    [[nodiscard]] std::size_t callbackCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return callbackCount_;
    }

    [[nodiscard]] std::optional<Failure> firstFailure() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return firstFailure_;
    }

    [[nodiscard]] bool reenteredBridge() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return reenteredBridge_;
    }

    [[nodiscard]] bool observedFatal() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return observedFatal_;
    }

private:
    Bridge* bridge_{};
    mutable std::mutex mutex_;
    std::optional<Failure> firstFailure_;
    std::size_t callbackCount_{};
    bool reenteredBridge_{};
    bool observedFatal_{};
};

class Harness final {
public:
    Harness()
        : api_{std::make_shared<FakeCompletionPortApi>()},
          router_{std::make_shared<RoutingSink>()}
    {
        auto port = take(Port::create(api_));
        kernel_ = take(Kernel::create(std::move(port), router_));
        bridge_ = take(Bridge::create(*kernel_, DeadlineKey));
        router_->attach(*bridge_);
    }

    ~Harness() noexcept
    {
        try {
            api_->holdDataCompletions();
            bridge_->shutdown();
            for (const auto& packet : api_->takeAllDataPackets()) {
                auto drained = bridge_->reap(
                    Key{static_cast<std::uintptr_t>(packet.completionKey)},
                    packet.transferredBytes,
                    packet.operation,
                    ERROR_SUCCESS);
                if (!drained) {
                    std::terminate();
                }
            }
            if (bridge_->snapshot().postedOperationCount() != 0U) {
                std::terminate();
            }
            router_->detachAndWait();
            bridge_.reset();
            kernel_->shutdown();
        } catch (...) {
            std::terminate();
        }
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] FakeCompletionPortApi& api() noexcept { return *api_; }
    [[nodiscard]] RoutingSink& router() noexcept { return *router_; }
    [[nodiscard]] Kernel& kernel() noexcept { return *kernel_; }
    [[nodiscard]] Bridge& bridge() noexcept { return *bridge_; }

private:
    std::shared_ptr<FakeCompletionPortApi> api_;
    std::shared_ptr<RoutingSink> router_;
    std::unique_ptr<Kernel> kernel_;
    std::shared_ptr<Bridge> bridge_;
};

[[nodiscard]] FakeCompletionPortApi::NativePacket onePacket(Harness& owner)
{
    require(owner.api().waitForPendingData(1U),
            "the synthetic IOCP packet was not posted");
    auto packet = owner.api().takeDataPacket();
    require(packet.has_value(),
            "the posted synthetic IOCP packet could not be claimed");
    return *packet;
}

void constructionAndAllFortyFourSlotsAreBounded()
{
    Harness owner;
    const auto reserved = Bridge::create(
        owner.kernel(), Key{Kernel::ShutdownKeyValue});
    require(!reserved, "the reserved kernel shutdown key was accepted");
    require(reserved.error().code == Domain::ErrorCodes::InvalidRequest,
            "the reserved key used the wrong typed error");

    auto snapshot = owner.bridge().snapshot();
    require(snapshot.maximumOwnerCount() == Bridge::SlotCount,
            "the bridge did not retain its exact hard capacity");
    require(snapshot.registeredOwnerCount() == 0U &&
                snapshot.postedOperationCount() == 0U &&
                snapshot.retiredAwaitingReapCount() == 0U,
            "a new bridge was not empty");
    require(!snapshot.isShutdown() && !snapshot.isFatal() &&
                snapshot.failure() == nullptr,
            "a new bridge reported a terminal state");

    std::array<Handle, Bridge::SlotCount> handles{};
    std::array<bool, Bridge::SlotCount> observedSlots{};
    for (std::size_t index{}; index < Bridge::SlotCount; ++index) {
        handles[index] = take(owner.bridge().registerOwner(index + 1U));
        require(handles[index].slotIndex < Bridge::SlotCount,
                "registration returned an out-of-range slot");
        require(!observedSlots[handles[index].slotIndex],
                "two live owners shared one bridge slot");
        observedSlots[handles[index].slotIndex] = true;
        require(handles[index].generation != 0U,
                "registration returned a zero generation");
    }
    snapshot = owner.bridge().snapshot();
    require(snapshot.registeredOwnerCount() == Bridge::SlotCount,
            "the bridge did not register all bounded owners");

    const auto overflow = owner.bridge().registerOwner(
        Bridge::SlotCount + 1U);
    require(!overflow, "an owner exceeded fixed capacity");
    require(overflow.error().code == Domain::ErrorCodes::LimitExceeded &&
                overflow.error().retryable,
            "capacity exhaustion used the wrong typed failure");

    for (const auto& handle : handles) {
        take(owner.bridge().retireOwner(handle));
    }
    snapshot = owner.bridge().snapshot();
    require(snapshot.registeredOwnerCount() == 0U &&
                snapshot.postedOperationCount() == 0U,
            "idle retirement did not vacate every fixed slot");

    const auto replacement = take(owner.bridge().registerOwner(
        Bridge::SlotCount + 1U));
    require(replacement.slotIndex == handles.front().slotIndex,
            "the first available stable slot was not reused");
    require(replacement.generation != handles.front().generation,
            "a reused stable slot repeated its mailbox generation");
    const auto stale = owner.bridge().retireOwner(handles.front());
    require(!stale, "a stale generation retired its replacement owner");
    require(stale.error().code == Domain::ErrorCodes::Conflict,
            "stale generation retirement used the wrong error");
    take(owner.bridge().retireOwner(replacement));
}

void onePostCoalescesAndDeliversTheLatestArm()
{
    Harness owner;
    const auto handle = take(owner.bridge().registerOwner(100U));
    owner.bridge().signal(deadline(100U, 1U));
    owner.bridge().signal(deadline(100U, 2U));

    require(owner.api().dataPostAttemptCount() == 1U &&
                owner.api().dataPostSuccessCount() == 1U,
            "coalescing posted more than one synthetic completion");
    require(owner.api().pendingDataCount() == 1U,
            "coalescing changed the one-packet kernel obligation");
    auto snapshot = owner.bridge().snapshot();
    require(snapshot.postedOperationCount() == 1U &&
                snapshot.successfulPostCount() == 1U &&
                snapshot.coalescedSignalCount() == 1U,
            "coalescing counters were incorrect");

    const auto firstPacket = onePacket(owner);
    require(firstPacket.transferredBytes == 0U &&
                firstPacket.completionKey == DeadlineKey.value() &&
                firstPacket.operation != nullptr,
            "the synthetic packet had the wrong native shape");
    const auto first = take(owner.bridge().reap(
        DeadlineKey,
        firstPacket.transferredBytes,
        firstPacket.operation,
        ERROR_SUCCESS));
    require(first.disposition() == ReapDisposition::DeadlineDelivered,
            "a live notification was not delivered");
    require(first.deadline() != nullptr &&
                first.deadline()->registrationId == 100U &&
                first.deadline()->armSequence == 2U,
            "the reaped notification did not retain the latest arm");

    owner.bridge().signal(deadline(100U, 3U));
    const auto secondPacket = onePacket(owner);
    require(secondPacket.operation == firstPacket.operation,
            "a mailbox generation changed its heap-stable OVERLAPPED address");
    const auto second = take(owner.bridge().reap(
        DeadlineKey,
        secondPacket.transferredBytes,
        secondPacket.operation,
        ERROR_SUCCESS));
    require(second.deadline() != nullptr &&
                second.deadline()->armSequence == 3U,
            "the successor notification was not delivered");
    snapshot = owner.bridge().snapshot();
    require(snapshot.registeredOwnerCount() == 1U &&
                snapshot.postedOperationCount() == 0U &&
                snapshot.deliveredDeadlineCount() == 2U &&
                !snapshot.isFatal(),
            "successful delivery left the bridge inconsistent");
    take(owner.bridge().retireOwner(handle));
}

void retirementAndReuseValidateTheExactMailboxGeneration()
{
    Harness owner;
    const auto firstHandle = take(owner.bridge().registerOwner(200U));
    owner.bridge().signal(deadline(200U, 1U));
    const auto firstPacket = onePacket(owner);
    take(owner.bridge().retireOwner(firstHandle));
    auto snapshot = owner.bridge().snapshot();
    require(snapshot.registeredOwnerCount() == 0U &&
                snapshot.postedOperationCount() == 1U &&
                snapshot.retiredAwaitingReapCount() == 1U,
            "pending retirement did not retain exactly one tombstone");
    const auto retired = take(owner.bridge().reap(
        DeadlineKey,
        firstPacket.transferredBytes,
        firstPacket.operation,
        ERROR_SUCCESS));
    require(retired.disposition() ==
                ReapDisposition::RetiredNotificationDrained &&
                retired.deadline() == nullptr,
            "a retired generation exposed its discarded deadline");
    snapshot = owner.bridge().snapshot();
    require(snapshot.postedOperationCount() == 0U &&
                snapshot.retiredAwaitingReapCount() == 0U &&
                snapshot.drainedRetiredCount() == 1U,
            "reaping did not release the exact generation tombstone");

    const auto secondHandle = take(owner.bridge().registerOwner(201U));
    require(secondHandle.slotIndex == firstHandle.slotIndex &&
                secondHandle.generation != firstHandle.generation,
            "a recycled operation did not receive a successor generation");
    owner.bridge().signal(deadline(201U, 1U));
    const auto secondPacket = onePacket(owner);
    require(secondPacket.operation == firstPacket.operation,
            "slot reuse changed the permanent operation address");
    require(!owner.bridge().retireOwner(firstHandle),
            "the stale handle mutated the successor generation");
    const auto delivered = take(owner.bridge().reap(
        DeadlineKey,
        secondPacket.transferredBytes,
        secondPacket.operation,
        ERROR_SUCCESS));
    require(delivered.deadline() != nullptr &&
                delivered.deadline()->registrationId == 201U,
            "the successor operation reaped the wrong mailbox generation");
    take(owner.bridge().retireOwner(secondHandle));
}

void allFortyThreePendingOwnersRemainFixedAndCoalesced()
{
    Harness owner;
    std::array<Handle, Bridge::SlotCount> handles{};
    for (std::size_t index{}; index < Bridge::SlotCount; ++index) {
        const auto registrationId = 300U + index;
        handles[index] = take(
            owner.bridge().registerOwner(registrationId));
        owner.bridge().signal(deadline(registrationId, 1U));
        owner.bridge().signal(deadline(registrationId, 2U));
    }
    require(owner.api().pendingDataCount() == Bridge::SlotCount &&
                owner.api().dataPostSuccessCount() == Bridge::SlotCount,
            "all owners did not produce exactly one pending packet");
    auto snapshot = owner.bridge().snapshot();
    require(snapshot.registeredOwnerCount() == Bridge::SlotCount &&
                snapshot.postedOperationCount() == Bridge::SlotCount &&
                snapshot.coalescedSignalCount() == Bridge::SlotCount,
            "the fully occupied bridge exceeded or lost fixed capacity");

    const auto packets = owner.api().takeAllDataPackets();
    require(packets.size() == Bridge::SlotCount,
            "the fixed native packet set had the wrong size");
    std::vector<OVERLAPPED*> addresses;
    addresses.reserve(packets.size());
    for (const auto& packet : packets) {
        require(std::find(
                    addresses.begin(), addresses.end(), packet.operation) ==
                    addresses.end(),
                "two owners posted the same OVERLAPPED address");
        addresses.push_back(packet.operation);
        const auto reaped = take(owner.bridge().reap(
            DeadlineKey,
            packet.transferredBytes,
            packet.operation,
            ERROR_SUCCESS));
        require(reaped.deadline() != nullptr &&
                    reaped.deadline()->armSequence == 2U,
                "a fixed slot did not deliver its latest coalesced value");
    }
    snapshot = owner.bridge().snapshot();
    require(snapshot.postedOperationCount() == 0U &&
                snapshot.deliveredDeadlineCount() == Bridge::SlotCount &&
                !snapshot.isFatal(),
            "draining all fixed slots left a pending operation");
    for (const auto& handle : handles) {
        take(owner.bridge().retireOwner(handle));
    }
}

void failedPostRemovesItsExactTombstoneBeforeFatalStatus()
{
    Harness owner;
    owner.api().setFailDataPosts(true, ERROR_NOT_ENOUGH_MEMORY);
    const auto failedHandle = take(owner.bridge().registerOwner(400U));
    const auto idleHandle = take(owner.bridge().registerOwner(401U));
    owner.bridge().signal(deadline(400U, 1U));

    require(owner.api().dataPostAttemptCount() == 1U &&
                owner.api().dataPostSuccessCount() == 0U &&
                owner.api().pendingDataCount() == 0U,
            "a failed post created a kernel packet");
    const auto snapshot = owner.bridge().snapshot();
    require(snapshot.isFatal() && snapshot.isShutdown(),
            "post failure did not close the deadline bridge");
    require(snapshot.registeredOwnerCount() == 0U &&
                snapshot.postedOperationCount() == 0U &&
                snapshot.retiredAwaitingReapCount() == 0U,
            "post failure left an exact mailbox tombstone behind");
    require(snapshot.failure() != nullptr &&
                snapshot.failure()->kind == FailureKind::LimitExceeded &&
                snapshot.failure()->retryable,
            "post failure did not retain its allocation-free classification");
    const auto full = owner.bridge().fullFailure();
    require(full.has_value() &&
                full->code == Domain::ErrorCodes::LimitExceeded &&
                full->retryable,
            "post failure did not retain its full typed diagnostic");
    require(!owner.bridge().retireOwner(failedHandle) &&
                !owner.bridge().retireOwner(idleHandle),
            "fatal shutdown retained a mutable owner handle");
}

void managedFailureObserverPublishesFirstFatalOutsideBridgeMutex()
{
    Harness owner;
    std::weak_ptr<FailureObserver> missingObserver;
    const auto missing =
        owner.bridge().bindFailureObserver(missingObserver);
    require(!missing &&
                missing.error().code ==
                    Domain::ErrorCodes::InvalidRequest &&
                !owner.bridge().snapshot().isFatal(),
            "an absent bridge failure observer was not rejected nonfatally");

    auto observer = std::make_shared<RecordingFailureObserver>();
    observer->attach(owner.bridge());
    take(owner.bridge().bindFailureObserver(observer));
    const auto duplicate =
        owner.bridge().bindFailureObserver(observer);
    require(!duplicate &&
                duplicate.error().code ==
                    Domain::ErrorCodes::Conflict &&
                !owner.bridge().snapshot().isFatal(),
            "duplicate bridge failure-observer binding was not rejected nonfatally");

    auto dispatchDeferral =
        owner.bridge().deferFailureNotificationDispatch();
    owner.api().setFailDataPosts(true, ERROR_NOT_ENOUGH_MEMORY);
    static_cast<void>(take(owner.bridge().registerOwner(450U)));
    owner.bridge().signal(deadline(450U, 1U));
    require(observer->callbackCount() == 0U &&
                owner.bridge().snapshot().isFatal(),
            "a deferred managed bridge failure escaped before its external routing transaction completed");
    dispatchDeferral.release();

    const auto failure = observer->firstFailure();
    require(observer->callbackCount() == 1U &&
                failure.has_value() &&
                failure->kind == FailureKind::LimitExceeded &&
                failure->retryable &&
                observer->reenteredBridge() &&
                observer->observedFatal(),
            "first fatal bridge transition was not published once outside the bridge mutex");

    OVERLAPPED alien{};
    const auto repeated = owner.bridge().reap(
        DeadlineKey, 0U, std::addressof(alien), ERROR_SUCCESS);
    require(!repeated && observer->callbackCount() == 1U,
            "a later fatal bridge observation republished the one-shot failure edge");
    auto lateObserver = std::make_shared<RecordingFailureObserver>();
    const auto late =
        owner.bridge().bindFailureObserver(lateObserver);
    require(!late &&
                late.error().code ==
                    Domain::ErrorCodes::TransportClosed,
            "fatal bridge accepted a late failure observer");
}

enum class MalformedPacketKind : std::uint8_t {
    WrongKey,
    NonzeroBytes,
    NativeFailure,
};

void malformedExactPacketDrainsBeforeIntegrityFailure(
    const MalformedPacketKind kind)
{
    Harness owner;
    static_cast<void>(take(owner.bridge().registerOwner(500U)));
    static_cast<void>(take(owner.bridge().registerOwner(501U)));
    owner.bridge().signal(deadline(500U, 1U));
    owner.bridge().signal(deadline(501U, 1U));
    const auto malformed = onePacket(owner);

    Key key = DeadlineKey;
    DWORD bytes = malformed.transferredBytes;
    DWORD nativeError = ERROR_SUCCESS;
    if (kind == MalformedPacketKind::WrongKey) {
        key = Key{DeadlineKey.value() + 1U};
    } else if (kind == MalformedPacketKind::NonzeroBytes) {
        bytes = 1U;
    } else {
        nativeError = ERROR_OPERATION_ABORTED;
    }

    const auto result = owner.bridge().reap(
        key, bytes, malformed.operation, nativeError);
    require(!result, "a malformed exact packet was accepted");
    require(result.error().code == Domain::ErrorCodes::IntegrityFailure,
            "malformed exact packet used the wrong typed error");
    auto snapshot = owner.bridge().snapshot();
    require(snapshot.isFatal() && snapshot.isShutdown(),
            "malformed exact packet did not close the bridge");
    require(snapshot.postedOperationCount() == 1U &&
                snapshot.retiredAwaitingReapCount() == 1U,
            "the malformed exact packet was not consumed before other posts became tombstones");
    require(snapshot.failure() != nullptr &&
                snapshot.failure()->kind ==
                    FailureKind::IntegrityFailure,
            "malformed packet did not retain compact integrity state");

    const auto survivor = onePacket(owner);
    const auto drained = take(owner.bridge().reap(
        DeadlineKey,
        survivor.transferredBytes,
        survivor.operation,
        ERROR_SUCCESS));
    require(drained.disposition() ==
                ReapDisposition::RetiredNotificationDrained,
            "fatal shutdown did not leave the other exact packet drainable");
    snapshot = owner.bridge().snapshot();
    require(snapshot.postedOperationCount() == 0U &&
                snapshot.retiredAwaitingReapCount() == 0U,
            "malformed packet handling stranded a mailbox tombstone");
}

void nullAlienAndDuplicatePacketsAreRejected()
{
    {
        Harness owner;
        const auto nullPacket = owner.bridge().reap(
            DeadlineKey, 0U, nullptr, ERROR_SUCCESS);
        require(!nullPacket &&
                    nullPacket.error().code ==
                        Domain::ErrorCodes::IntegrityFailure,
                "a null synthetic operation was accepted");
        require(owner.bridge().snapshot().isFatal(),
                "a null synthetic operation did not close the bridge");
    }
    {
        Harness owner;
        OVERLAPPED alien{};
        const auto alienPacket = owner.bridge().reap(
            DeadlineKey, 0U, std::addressof(alien), ERROR_SUCCESS);
        require(!alienPacket &&
                    alienPacket.error().code ==
                        Domain::ErrorCodes::IntegrityFailure,
                "an alien synthetic operation was accepted");
    }
    {
        Harness owner;
        static_cast<void>(take(owner.bridge().registerOwner(600U)));
        owner.bridge().signal(deadline(600U, 1U));
        const auto packet = onePacket(owner);
        static_cast<void>(take(owner.bridge().reap(
            DeadlineKey,
            packet.transferredBytes,
            packet.operation,
            ERROR_SUCCESS)));
        const auto duplicate = owner.bridge().reap(
            DeadlineKey,
            packet.transferredBytes,
            packet.operation,
            ERROR_SUCCESS);
        require(!duplicate &&
                    duplicate.error().code ==
                        Domain::ErrorCodes::IntegrityFailure,
                "one synthetic packet was reaped twice");
        require(owner.bridge().snapshot().postedOperationCount() == 0U,
                "duplicate rejection created a pending obligation");
    }
}

void shutdownRetainsOnlyTheFortyThreePostedObligations()
{
    Harness owner;
    std::array<Handle, Bridge::SlotCount> handles{};
    for (std::size_t index{}; index < Bridge::SlotCount; ++index) {
        const auto registrationId = 700U + index;
        handles[index] = take(
            owner.bridge().registerOwner(registrationId));
        if ((index % 2U) == 0U) {
            owner.bridge().signal(deadline(registrationId, 1U));
        }
    }
    const auto expectedPosted = (Bridge::SlotCount + 1U) / 2U;
    const auto start = std::chrono::steady_clock::now();
    owner.bridge().shutdown();
    owner.bridge().shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    auto snapshot = owner.bridge().snapshot();
    require(elapsed < std::chrono::seconds{1},
            "fixed bridge shutdown exceeded its bounded local work");
    require(snapshot.isShutdown() && !snapshot.isFatal(),
            "ordinary shutdown was not idempotent or became fatal");
    require(snapshot.registeredOwnerCount() == 0U &&
                snapshot.postedOperationCount() == expectedPosted &&
                snapshot.retiredAwaitingReapCount() == expectedPosted,
            "shutdown retained more than its already-posted obligations");
    require(!owner.bridge().registerOwner(800U),
            "shutdown bridge accepted a new owner");

    const auto packets = owner.api().takeAllDataPackets();
    require(packets.size() == expectedPosted,
            "shutdown retained the wrong number of kernel packets");
    for (const auto& packet : packets) {
        const auto reaped = take(owner.bridge().reap(
            DeadlineKey,
            packet.transferredBytes,
            packet.operation,
            ERROR_SUCCESS));
        require(reaped.disposition() ==
                    ReapDisposition::RetiredNotificationDrained,
                "shutdown tombstone exposed a deadline");
    }
    snapshot = owner.bridge().snapshot();
    require(snapshot.postedOperationCount() == 0U &&
                snapshot.retiredAwaitingReapCount() == 0U &&
                snapshot.drainedRetiredCount() == expectedPosted,
            "shutdown tombstones did not drain exactly once");
}

void realWorkerRoutingDeliversThroughTheKernel()
{
    Harness owner;
    const auto handle = take(owner.bridge().registerOwner(900U));
    owner.api().releaseDataCompletions();
    owner.bridge().signal(deadline(900U, 1U));
    owner.bridge().signal(deadline(900U, 2U));
    require(owner.router().waitForLatestSequence(2U),
            "the real four-worker kernel did not route the latest deadline");
    const auto latest = owner.router().latestDeadline();
    require(latest.has_value() && latest->registrationId == 900U &&
                latest->armSequence == 2U,
            "kernel routing delivered the wrong deadline token");
    require(owner.router().failureCount() == 0U &&
                owner.router().fatalCount() == 0U,
            "ordinary kernel routing reported a failure");
    require(owner.bridge().snapshot().postedOperationCount() == 0U,
            "kernel routing did not reap its stable operation");
    require(owner.api().allWaitsBounded(),
            "a bridge test kernel worker used an unbounded wait");
    take(owner.bridge().retireOwner(handle));
}

void concurrentPublicationAndKernelReapingPreserveTheLatestValue()
{
    Harness owner;
    constexpr std::uint64_t PublicationCount = 20'000U;
    const auto handle = take(owner.bridge().registerOwner(1'000U));
    owner.api().releaseDataCompletions();

    std::jthread producer{[&](std::stop_token) noexcept {
        for (std::uint64_t sequence = 1U;
             sequence <= PublicationCount;
             ++sequence) {
            owner.bridge().signal(deadline(1'000U, sequence));
        }
    }};
    producer.join();
    require(owner.router().waitForLatestSequence(PublicationCount),
            "concurrent IOCP reaping lost the latest deadline value");
    const auto snapshot = owner.bridge().snapshot();
    require(!snapshot.isFatal() &&
                snapshot.postedOperationCount() == 0U &&
                snapshot.deliveredDeadlineCount() >= 1U &&
                snapshot.successfulPostCount() ==
                    snapshot.deliveredDeadlineCount(),
            "concurrent publication broke the one-post/one-reap invariant");
    require(snapshot.successfulPostCount() +
                snapshot.coalescedSignalCount() ==
                PublicationCount,
            "concurrent publication lost or duplicated a signal");
    require(owner.router().failureCount() == 0U,
            "concurrent routing returned a typed reap failure");
    take(owner.bridge().retireOwner(handle));
}

void retirementRacingSignalNeverLeaksAnOperation()
{
    Harness owner;
    constexpr std::size_t IterationCount = 1'000U;
    for (std::size_t iteration{}; iteration < IterationCount; ++iteration) {
        const std::uint64_t registrationId = 2'000U + iteration;
        const auto handle = take(
            owner.bridge().registerOwner(registrationId));
        std::atomic_bool start{};
        std::atomic_bool retirementFailed{};
        std::jthread publisher{[&](std::stop_token) noexcept {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            owner.bridge().signal(deadline(registrationId, 1U));
        }};
        std::jthread retiree{[&](std::stop_token) noexcept {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (!owner.bridge().retireOwner(handle)) {
                retirementFailed.store(true, std::memory_order_release);
            }
        }};
        start.store(true, std::memory_order_release);
        publisher.join();
        retiree.join();
        require(!retirementFailed.load(std::memory_order_acquire),
                "signal-versus-retire race lost the live handle");

        for (const auto& packet : owner.api().takeAllDataPackets()) {
            const auto drained = take(owner.bridge().reap(
                DeadlineKey,
                packet.transferredBytes,
                packet.operation,
                ERROR_SUCCESS));
            require(drained.disposition() ==
                        ReapDisposition::RetiredNotificationDrained,
                    "signal-versus-retire exposed a retired deadline");
        }
        const auto snapshot = owner.bridge().snapshot();
        require(!snapshot.isFatal() &&
                    snapshot.registeredOwnerCount() == 0U &&
                    snapshot.postedOperationCount() == 0U &&
                    snapshot.retiredAwaitingReapCount() == 0U,
                "signal-versus-retire left a slot or tombstone behind");
    }
}

void shutdownRacingSignalsRetainsOnlyDrainablePackets()
{
    Harness owner;
    constexpr std::size_t OwnerCount = Bridge::SlotCount;
    std::array<Handle, OwnerCount> handles{};
    for (std::size_t index{}; index < OwnerCount; ++index) {
        handles[index] = take(owner.bridge().registerOwner(4'000U + index));
    }
    std::atomic_bool start{};
    std::jthread publisher{[&](std::stop_token) noexcept {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t index{}; index < OwnerCount; ++index) {
            owner.bridge().signal(deadline(4'000U + index, 1U));
        }
    }};
    std::jthread closer{[&](std::stop_token) noexcept {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        owner.bridge().shutdown();
    }};
    start.store(true, std::memory_order_release);
    publisher.join();
    closer.join();

    auto snapshot = owner.bridge().snapshot();
    require(snapshot.isShutdown() && !snapshot.isFatal() &&
                snapshot.registeredOwnerCount() == 0U &&
                snapshot.postedOperationCount() ==
                    snapshot.retiredAwaitingReapCount(),
            "signal-versus-shutdown retained a non-drainable owner");
    const auto packets = owner.api().takeAllDataPackets();
    require(packets.size() == snapshot.postedOperationCount(),
            "signal-versus-shutdown lost an exact kernel obligation");
    for (const auto& packet : packets) {
        const auto drained = take(owner.bridge().reap(
            DeadlineKey,
            packet.transferredBytes,
            packet.operation,
            ERROR_SUCCESS));
        require(drained.disposition() ==
                    ReapDisposition::RetiredNotificationDrained,
                "signal-versus-shutdown returned a live deadline");
    }
    snapshot = owner.bridge().snapshot();
    require(snapshot.postedOperationCount() == 0U &&
                snapshot.retiredAwaitingReapCount() == 0U,
            "signal-versus-shutdown did not fully drain");
}

} // namespace

int main()
{
    try {
        constructionAndAllFortyFourSlotsAreBounded();
        onePostCoalescesAndDeliversTheLatestArm();
        retirementAndReuseValidateTheExactMailboxGeneration();
        allFortyThreePendingOwnersRemainFixedAndCoalesced();
        failedPostRemovesItsExactTombstoneBeforeFatalStatus();
        managedFailureObserverPublishesFirstFatalOutsideBridgeMutex();
        malformedExactPacketDrainsBeforeIntegrityFailure(
            MalformedPacketKind::WrongKey);
        malformedExactPacketDrainsBeforeIntegrityFailure(
            MalformedPacketKind::NonzeroBytes);
        malformedExactPacketDrainsBeforeIntegrityFailure(
            MalformedPacketKind::NativeFailure);
        nullAlienAndDuplicatePacketsAreRejected();
        shutdownRetainsOnlyTheFortyThreePostedObligations();
        realWorkerRoutingDeliversThroughTheKernel();
        concurrentPublicationAndKernelReapingPreserveTheLatestValue();
        retirementRacingSignalNeverLeaksAnOperation();
        shutdownRacingSignalsRetainsOnlyDrainablePackets();
        std::cout << "Dashboard deadline IOCP bridge tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard deadline IOCP bridge tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard deadline IOCP bridge tests failed with an unknown error.\n";
        return 1;
    }
}
