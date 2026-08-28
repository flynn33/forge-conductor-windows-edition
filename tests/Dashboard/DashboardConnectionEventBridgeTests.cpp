#include "Infrastructure/Windows/Detail/DashboardConnectionEventBridge.h"

#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"

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

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Api = Detail::IDashboardIoCompletionPortApi;
using Bridge = Detail::DashboardConnectionEventBridge;
using Completion = Windows::DashboardHandlerCompletion;
using FailureKind = Detail::DashboardConnectionEventFailureKind;
using FatalNotification =
    Detail::DashboardConnectionEventFatalNotification;
using FatalSink = Detail::IDashboardConnectionEventFatalSink;
using Key = Detail::DashboardIoCompletionKey;
using Kernel = Detail::DashboardIocpWorkerKernel;
using KernelSink = Detail::IDashboardIocpCompletionSink;
using Packet = Detail::DashboardIoCompletionPacket;
using Port = Detail::DashboardIoCompletionPort;
using ReapDisposition =
    Detail::DashboardConnectionEventReapDisposition;
using ReapResult = Detail::DashboardConnectionEventReapResult;

static_assert(std::is_final_v<Bridge>);
static_assert(std::is_base_of_v<Windows::IDashboardHandlerCompletionSink, Bridge>);
static_assert(std::is_base_of_v<Dashboard::IDashboardSseReadySink, Bridge>);
static_assert(!std::is_copy_constructible_v<Bridge>);
static_assert(!std::is_move_constructible_v<Bridge>);
static_assert(!std::is_copy_constructible_v<ReapResult>);
static_assert(std::is_nothrow_move_constructible_v<ReapResult>);
static_assert(noexcept(Bridge::create(
    std::declval<Kernel&>(), Key{1U}, 1U, std::weak_ptr<FatalSink>{})));
static_assert(noexcept(std::declval<Bridge&>().tryPost(
    std::declval<Completion>())));
static_assert(noexcept(std::declval<Bridge&>().signal()));
static_assert(noexcept(std::declval<Bridge&>().reap(
    Key{1U}, 0U, nullptr, ERROR_SUCCESS)));
static_assert(noexcept(std::declval<const Bridge&>().snapshot()));
static_assert(!noexcept(std::declval<const Bridge&>().fullFailure()));
static_assert(noexcept(std::declval<Bridge&>().shutdown()));

constexpr Key ConnectionKey{0x434F4E4E454354U};
constexpr std::uint64_t OwnerRegistrationId = 7001U;
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

[[nodiscard]] Completion handlerFailure(const std::string_view marker)
{
    return Completion::postDelivery(
        Completion::PostDeliveryResult::failure(Domain::makeError(
            marker, "move-only handler marker")));
}

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
        if (fileHandle == INVALID_HANDLE_VALUE) {
            return portHandle();
        }
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
            controls_.push_back(
                QueuedPacket{transferredBytes, completionKey, operation});
            changed_.notify_all();
            return TRUE;
        }

        ++dataPostAttemptCount_;
        if (failDataPosts_) {
            setThreadError(dataPostFailure_);
            return FALSE;
        }
        data_.push_back(
            QueuedPacket{transferredBytes, completionKey, operation});
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
        const auto ready = [this] {
            return closed_ || !controls_.empty() ||
                (!holdData_ && !data_.empty());
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

        std::optional<QueuedPacket> packet;
        if (!controls_.empty()) {
            packet.emplace(controls_.front());
            controls_.pop_front();
        } else if (!holdData_ && !data_.empty()) {
            packet.emplace(data_.front());
            data_.pop_front();
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
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override { return threadError_; }

    void failDataPosts(
        const DWORD error = ERROR_NOT_ENOUGH_MEMORY) noexcept
    {
        const std::lock_guard lock{mutex_};
        failDataPosts_ = true;
        dataPostFailure_ = error;
    }

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

    [[nodiscard]] std::size_t pendingDataCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return data_.size();
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
    DWORD dataPostFailure_{ERROR_NOT_ENOUGH_MEMORY};
    std::size_t dataPostAttemptCount_{};
    std::size_t dataPostSuccessCount_{};
    bool failDataPosts_{};
    bool holdData_{true};
    bool closed_{};
};

class NullKernelSink final : public KernelSink {
public:
    void consume(const Packet, const DWORD) noexcept override
    {
        ++unexpectedPackets_;
    }

    void fatal(const DWORD) noexcept override { ++fatalCount_; }

    [[nodiscard]] std::size_t unexpectedPackets() const noexcept
    {
        return unexpectedPackets_.load();
    }

private:
    std::atomic<std::size_t> unexpectedPackets_{};
    std::atomic<std::size_t> fatalCount_{};
};

class RecordingFatalSink final : public FatalSink {
public:
    void fatal(const FatalNotification notification) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            latest_ = notification;
            ++count_;
        }
        if (resetTarget_ != nullptr) {
            resetTarget_->reset();
        }
    }

    void releaseOuterOnFatal(std::shared_ptr<Bridge>& target) noexcept
    {
        resetTarget_ = std::addressof(target);
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return count_;
    }

    [[nodiscard]] std::optional<FatalNotification> latest() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return latest_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<FatalNotification> latest_;
    std::shared_ptr<Bridge>* resetTarget_{};
    std::size_t count_{};
};

struct Fixture final {
    Fixture()
        : api{std::make_shared<FakeCompletionPortApi>()},
          kernelSink{std::make_shared<NullKernelSink>()},
          fatalSink{std::make_shared<RecordingFatalSink>()}
    {
        auto port = take(Port::create(api));
        kernel = take(Kernel::create(std::move(port), kernelSink));
        bridge = take(Bridge::create(
            *kernel,
            ConnectionKey,
            OwnerRegistrationId,
            fatalSink));
    }

    ~Fixture() noexcept
    {
        if (bridge != nullptr) {
            bridge->shutdown();
            while (const auto packet = api->takeDataPacket()) {
                static_cast<void>(bridge->reap(
                    Key{static_cast<std::uintptr_t>(packet->completionKey)},
                    packet->transferredBytes,
                    packet->operation,
                    ERROR_SUCCESS));
            }
            bridge.reset();
        }
        if (kernel != nullptr) {
            kernel->shutdown();
        }
    }

    std::shared_ptr<FakeCompletionPortApi> api;
    std::shared_ptr<NullKernelSink> kernelSink;
    std::shared_ptr<RecordingFatalSink> fatalSink;
    std::unique_ptr<Kernel> kernel;
    std::shared_ptr<Bridge> bridge;
};

[[nodiscard]] ReapResult reapPacket(
    Bridge& bridge,
    const FakeCompletionPortApi::QueuedPacket& packet)
{
    return take(bridge.reap(
        Key{static_cast<std::uintptr_t>(packet.completionKey)},
        packet.transferredBytes,
        packet.operation,
        ERROR_SUCCESS));
}

void validatesConstructionAndFixedContracts()
{
    Fixture fixture;
    auto reserved = Bridge::create(
        *fixture.kernel,
        Key{Kernel::ShutdownKeyValue},
        OwnerRegistrationId,
        fixture.fatalSink);
    require(!reserved, "reserved shutdown key was accepted");
    require(reserved.error().code == Domain::ErrorCodes::InvalidRequest,
            "reserved key used the wrong error");

    auto zeroOwner = Bridge::create(
        *fixture.kernel, ConnectionKey, 0U, fixture.fatalSink);
    require(!zeroOwner, "zero owner registration was accepted");

    const auto snapshot = fixture.bridge->snapshot();
    require(snapshot.ownerRegistrationId() == OwnerRegistrationId,
            "snapshot lost owner registration");
    require(snapshot.postedOperationCount() == 0U,
            "new bridge reported a posted operation");
    require(snapshot.handlerPayloadCount() == 0U,
            "new bridge reported a handler payload");
    require(snapshot.fullyDrained(), "new bridge was not drained");
    require(!snapshot.isShutdown() && !snapshot.isFatal(),
            "new bridge started closed");
}

void postsAndMovesOneHandlerCompletionExactlyOnce()
{
    Fixture fixture;
    require(fixture.bridge->tryPost(handlerFailure("handler_marker")),
            "handler completion was rejected");
    const auto posted = fixture.bridge->snapshot();
    require(posted.postedOperationCount() == 1U,
            "handler did not own one posted packet");
    require(posted.handlerPayloadCount() == 1U,
            "handler payload was not retained");
    require(fixture.api->dataPostAttemptCount() == 1U &&
                fixture.api->dataPostSuccessCount() == 1U,
            "handler did not make exactly one successful native post");

    const auto packet = fixture.api->takeDataPacket();
    require(packet.has_value(), "handler packet was absent");
    auto delivered = reapPacket(*fixture.bridge, *packet);
    require(delivered.disposition() == ReapDisposition::PayloadDelivered,
            "handler packet used the wrong disposition");
    require(delivered.hasHandlerCompletion(),
            "handler packet lost its move-only result");
    require(!delivered.sseReady(), "handler-only packet invented SSE readiness");

    auto completion = delivered.takeHandlerCompletion();
    require(completion.has_value(), "handler result could not be taken");
    require(!delivered.takeHandlerCompletion().has_value(),
            "handler result could be taken twice");
    require(completion->kind() ==
                Windows::DashboardHandlerCompletionKind::PostDelivery,
            "handler result changed kind");
    const auto* result = completion->postDeliveryResult();
    require(result != nullptr && !*result,
            "handler failure payload was not preserved");
    require(result->error().code == "handler_marker",
            "handler failure marker changed while moving");
    require(fixture.bridge->snapshot().fullyDrained(),
            "handler packet remained a drain obligation");
}

void coalescesSseSignalsAndSupportsSynchronousAttach()
{
    class ImmediateSubscription final : public Dashboard::IDashboardSseSubscription {
    public:
        [[nodiscard]] double deliveryHz() const noexcept override { return 1.0; }
        void attachReadySink(
            std::weak_ptr<Dashboard::IDashboardSseReadySink> sink) noexcept override
        {
            if (auto ready = sink.lock(); ready != nullptr) {
                ready->signal();
            }
        }
        [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame
        takeLatest() noexcept override { return {}; }
        [[nodiscard]] std::size_t pendingCount() const noexcept override
        {
            return 0U;
        }
        void close() noexcept override {}
    } subscription;

    Fixture fixture;
    subscription.attachReadySink(
        std::weak_ptr<Dashboard::IDashboardSseReadySink>{fixture.bridge});
    fixture.bridge->signal();
    fixture.bridge->signal();

    const auto snapshot = fixture.bridge->snapshot();
    require(snapshot.postedOperationCount() == 1U,
            "coalesced SSE signals posted more than one packet");
    require(snapshot.sseReadyLatched(), "SSE-ready bit was not latched");
    require(snapshot.coalescedSignalCount() == 2U,
            "SSE coalescing count was incorrect");
    require(fixture.api->dataPostSuccessCount() == 1U,
            "SSE coalescing reached the native queue more than once");

    const auto packet = fixture.api->takeDataPacket();
    require(packet.has_value(), "SSE packet was absent");
    auto delivered = reapPacket(*fixture.bridge, *packet);
    require(delivered.sseReady(), "SSE-ready bit was not delivered");
    require(!delivered.hasHandlerCompletion(),
            "SSE-only packet invented a handler completion");
}

void sharesOnePacketForSimultaneousBoundedPayload()
{
    for (const bool handlerFirst : {false, true}) {
        Fixture fixture;
        if (handlerFirst) {
            require(fixture.bridge->tryPost(handlerFailure("both")),
                    "handler-first payload was rejected");
            fixture.bridge->signal();
        } else {
            fixture.bridge->signal();
            require(fixture.bridge->tryPost(handlerFailure("both")),
                    "SSE-first handler payload was rejected");
        }

        require(fixture.api->dataPostSuccessCount() == 1U,
                "simultaneous payload posted two packets");
        const auto packet = fixture.api->takeDataPacket();
        require(packet.has_value(), "simultaneous payload packet was absent");
        auto delivered = reapPacket(*fixture.bridge, *packet);
        require(delivered.sseReady(), "simultaneous payload lost SSE readiness");
        require(delivered.takeHandlerCompletion().has_value(),
                "simultaneous payload lost handler completion");
        require(fixture.bridge->snapshot().fullyDrained(),
                "simultaneous payload did not drain");
    }
}

void duplicateHandlerIsFatalAndRetainsTombstone()
{
    Fixture fixture;
    require(fixture.bridge->tryPost(handlerFailure("first")),
            "first handler payload was rejected");
    require(!fixture.bridge->tryPost(handlerFailure("duplicate")),
            "duplicate handler payload was accepted");

    const auto snapshot = fixture.bridge->snapshot();
    require(snapshot.isFatal() && snapshot.isShutdown(),
            "duplicate handler did not close fatally");
    require(snapshot.tombstoneAwaitingReap(),
            "duplicate handler discarded a queued operation");
    require(snapshot.handlerPayloadCount() == 0U,
            "duplicate fatal retained a deliverable handler");
    require(snapshot.failure() != nullptr &&
                snapshot.failure()->kind == FailureKind::IntegrityFailure,
            "duplicate handler used the wrong fatal classification");
    require(fixture.fatalSink->count() == 1U,
            "duplicate handler did not notify fatal exactly once");

    const auto packet = fixture.api->takeDataPacket();
    require(packet.has_value(), "duplicate handler tombstone packet was absent");
    auto drained = reapPacket(*fixture.bridge, *packet);
    require(drained.disposition() ==
                ReapDisposition::RetiredNotificationDrained,
            "duplicate handler delivered discarded payload");
    require(fixture.bridge->snapshot().fullyDrained(),
            "duplicate handler tombstone did not drain");
}

void postFailureConsumesPayloadAndNotifiesFatal()
{
    Fixture fixture;
    fixture.api->failDataPosts();
    require(!fixture.bridge->tryPost(handlerFailure("post_failure")),
            "failed native post reported success");
    const auto snapshot = fixture.bridge->snapshot();
    require(snapshot.isFatal() && snapshot.isShutdown(),
            "post failure did not close fatally");
    require(snapshot.fullyDrained(), "failed post invented a drain obligation");
    require(snapshot.handlerPayloadCount() == 0U,
            "failed post retained a handler payload");
    require(fixture.api->pendingDataCount() == 0U,
            "failed post queued a packet");
    require(fixture.fatalSink->count() == 1U,
            "failed post did not notify fatal");
    const auto notification = fixture.fatalSink->latest();
    require(notification.has_value() &&
                notification->ownerRegistrationId == OwnerRegistrationId,
            "fatal notification lost owner registration");
    require(notification->failure.kind == FailureKind::LimitExceeded,
            "native resource failure used the wrong classification");
    const auto full = fixture.bridge->fullFailure();
    require(full.has_value() &&
                full->code == Domain::ErrorCodes::LimitExceeded,
            "full post failure diagnostic was not retained");
}

void foreignPointerIsNonmutating()
{
    Fixture fixture;
    fixture.bridge->signal();
    const auto packet = fixture.api->takeDataPacket();
    require(packet.has_value(), "foreign-pointer setup packet was absent");
    const auto before = fixture.bridge->snapshot();
    OVERLAPPED foreign{};
    auto rejected = fixture.bridge->reap(
        Key{ConnectionKey.value() + 1U}, 9U, &foreign, ERROR_INVALID_DATA);
    require(!rejected, "foreign pointer was accepted");
    const auto after = fixture.bridge->snapshot();
    require(after.postedOperationCount() == before.postedOperationCount() &&
                after.sseReadyLatched() == before.sseReadyLatched() &&
                after.isFatal() == before.isFatal(),
            "foreign pointer mutated bridge state");
    require(fixture.fatalSink->count() == 0U,
            "foreign pointer notified this unrelated owner as fatal");

    auto delivered = reapPacket(*fixture.bridge, *packet);
    require(delivered.sseReady(),
            "exact packet could not be reaped after foreign pointer");
}

void exactMalformedPacketsConsumeBeforeFatal()
{
    enum class Corruption { Key, Bytes, NativeError };
    for (const auto corruption :
         {Corruption::Key, Corruption::Bytes, Corruption::NativeError}) {
        Fixture fixture;
        fixture.bridge->signal();
        const auto packet = fixture.api->takeDataPacket();
        require(packet.has_value(), "malformed setup packet was absent");

        auto key = ConnectionKey;
        DWORD bytes{};
        DWORD error{ERROR_SUCCESS};
        if (corruption == Corruption::Key) {
            key = Key{ConnectionKey.value() + 1U};
        } else if (corruption == Corruption::Bytes) {
            bytes = 1U;
        } else {
            error = ERROR_OPERATION_ABORTED;
        }
        auto reaped = fixture.bridge->reap(
            key, bytes, packet->operation, error);
        require(!reaped, "exact malformed packet was accepted");
        require(reaped.error().code == Domain::ErrorCodes::IntegrityFailure,
                "exact malformed packet used the wrong error");
        const auto snapshot = fixture.bridge->snapshot();
        require(snapshot.fullyDrained(),
                "exact malformed packet stranded its dequeued operation");
        require(snapshot.isFatal() && snapshot.handlerPayloadCount() == 0U &&
                    !snapshot.sseReadyLatched(),
                "exact malformed packet did not discard and close");
        require(fixture.fatalSink->count() == 1U,
                "exact malformed packet did not notify fatal");
    }
}

void shutdownRetainsOnlyAnExactTombstone()
{
    Fixture fixture;
    fixture.bridge->signal();
    require(fixture.bridge->tryPost(handlerFailure("shutdown")),
            "shutdown setup handler was rejected");
    const auto packet = fixture.api->takeDataPacket();
    require(packet.has_value(), "shutdown tombstone packet was absent");

    fixture.bridge->shutdown();
    fixture.bridge->shutdown();
    require(!fixture.bridge->tryPost(handlerFailure("late")),
            "shutdown bridge accepted a later handler");
    fixture.bridge->signal();
    const auto closed = fixture.bridge->snapshot();
    require(closed.isShutdown() && !closed.isFatal(),
            "ordinary shutdown became fatal");
    require(closed.tombstoneAwaitingReap() &&
                closed.postedOperationCount() == 1U,
            "shutdown released its queued operation early");
    require(closed.handlerPayloadCount() == 0U &&
                !closed.sseReadyLatched(),
            "shutdown retained deliverable payload");
    require(fixture.api->dataPostSuccessCount() == 1U,
            "shutdown publication created another packet");

    auto drained = reapPacket(*fixture.bridge, *packet);
    require(drained.disposition() ==
                ReapDisposition::RetiredNotificationDrained,
            "shutdown tombstone delivered payload");
    require(fixture.bridge->snapshot().fullyDrained(),
            "shutdown tombstone remained posted");
}

class ReentrantCloseSubscription final
    : public Dashboard::IDashboardSseSubscription {
public:
    explicit ReentrantCloseSubscription(
        std::weak_ptr<Bridge> bridge,
        std::atomic<std::size_t>& closeCount,
        std::atomic<bool>& observedUnlocked) noexcept
        : bridge_{std::move(bridge)},
          closeCount_{closeCount},
          observedUnlocked_{observedUnlocked}
    {
    }

    ~ReentrantCloseSubscription() noexcept override { close(); }

    [[nodiscard]] double deliveryHz() const noexcept override { return 1.0; }
    void attachReadySink(
        std::weak_ptr<Dashboard::IDashboardSseReadySink>) noexcept override
    {
    }
    [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame
    takeLatest() noexcept override { return {}; }
    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return 0U;
    }

    void close() noexcept override
    {
        if (closed_.exchange(true)) {
            return;
        }
        ++closeCount_;
        if (auto owner = bridge_.lock(); owner != nullptr) {
            const auto snapshot = owner->snapshot();
            observedUnlocked_.store(snapshot.isShutdown());
            owner->signal();
        }
    }

private:
    std::weak_ptr<Bridge> bridge_;
    std::atomic<std::size_t>& closeCount_;
    std::atomic<bool>& observedUnlocked_;
    std::atomic<bool> closed_{};
};

[[nodiscard]] Completion reentrantSseCompletion(
    const std::shared_ptr<Bridge>& bridge,
    std::atomic<std::size_t>& closeCount,
    std::atomic<bool>& observedUnlocked)
{
    auto encoded = Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap();
    auto exchange = take(Dashboard::DashboardPreparedExchange::createSse(
        std::move(encoded),
        std::make_unique<ReentrantCloseSubscription>(
            bridge, closeCount, observedUnlocked)));
    return Completion::prepared(
        Completion::PreparedResult::success(std::move(exchange)));
}

void destroysDiscardedCompletionOnlyAfterUnlock()
{
    enum class DiscardPath { Shutdown, PostFailure, Malformed, Duplicate };
    for (const auto path : {
             DiscardPath::Shutdown,
             DiscardPath::PostFailure,
             DiscardPath::Malformed,
             DiscardPath::Duplicate}) {
        Fixture fixture;
        std::atomic<std::size_t> closeCount{};
        std::atomic<bool> observedUnlocked{};
        if (path == DiscardPath::PostFailure) {
            fixture.api->failDataPosts();
        }

        const bool accepted = fixture.bridge->tryPost(reentrantSseCompletion(
            fixture.bridge, closeCount, observedUnlocked));
        require(
            accepted == (path != DiscardPath::PostFailure),
            "reentrant completion used the wrong post disposition");

        std::optional<FakeCompletionPortApi::QueuedPacket> packet;
        if (path != DiscardPath::PostFailure) {
            packet = fixture.api->takeDataPacket();
            require(packet.has_value(), "reentrant SSE packet was absent");
        }

        if (path == DiscardPath::Shutdown) {
            fixture.bridge->shutdown();
        } else if (path == DiscardPath::Malformed) {
            auto malformed = fixture.bridge->reap(
                ConnectionKey,
                1U,
                packet->operation,
                ERROR_SUCCESS);
            require(!malformed,
                    "reentrant malformed completion was accepted");
            packet.reset();
        } else if (path == DiscardPath::Duplicate) {
            require(!fixture.bridge->tryPost(handlerFailure("duplicate")),
                    "reentrant duplicate completion was accepted");
        }

        require(closeCount.load() == 1U,
                "discarded SSE completion did not close exactly once");
        require(observedUnlocked.load(),
                "SSE completion destructor could not reenter after unlock");
        require(
            fixture.api->dataPostSuccessCount() ==
                (path == DiscardPath::PostFailure ? 0U : 1U),
            "reentrant close changed the bounded native post count");

        if (packet.has_value()) {
            auto drained = reapPacket(*fixture.bridge, *packet);
            require(drained.disposition() ==
                        ReapDisposition::RetiredNotificationDrained,
                    "reentrant completion tombstone delivered payload");
        }
    }
}

void toleratesExpiredAndReentrantFatalSinks()
{
    {
        Fixture fixture;
        fixture.fatalSink.reset();
        fixture.api->failDataPosts();
        require(!fixture.bridge->tryPost(handlerFailure("expired_sink")),
                "expired fatal-sink post failure reported success");
        require(fixture.bridge->snapshot().isFatal(),
                "expired fatal sink prevented retained fatal state");
    }

    {
        Fixture fixture;
        fixture.api->failDataPosts();
        fixture.fatalSink->releaseOuterOnFatal(fixture.bridge);
        std::weak_ptr<Bridge> weakBridge{fixture.bridge};
        require(!fixture.bridge->tryPost(handlerFailure("reentrant_release")),
                "reentrant-release post failure reported success");
        require(fixture.bridge == nullptr,
                "fatal callback did not release the outer bridge");
        require(weakBridge.expired(),
                "bridge did not survive exactly through its fatal callback");
        require(fixture.fatalSink->count() == 1U,
                "reentrant fatal callback count was incorrect");
    }
}

void racesHandlerPostAgainstExactReapWithoutLoss()
{
    Fixture fixture;
    fixture.bridge->signal();
    const auto firstPacket = fixture.api->takeDataPacket();
    require(firstPacket.has_value(), "post-vs-reap setup packet was absent");

    std::atomic<bool> start{};
    std::optional<Domain::Result<ReapResult>> firstReap;
    bool postAccepted{};
    std::thread reaper{[&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        firstReap.emplace(fixture.bridge->reap(
            ConnectionKey,
            0U,
            firstPacket->operation,
            ERROR_SUCCESS));
    }};
    std::thread poster{[&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        postAccepted = fixture.bridge->tryPost(handlerFailure("raced"));
    }};
    start.store(true, std::memory_order_release);
    reaper.join();
    poster.join();

    require(postAccepted, "racing handler post was rejected");
    require(firstReap.has_value() && *firstReap,
            "racing first packet did not reap");
    auto first = std::move(*firstReap).value();
    std::size_t handlerCount =
        first.takeHandlerCompletion().has_value() ? 1U : 0U;
    if (const auto successor = fixture.api->takeDataPacket()) {
        auto second = reapPacket(*fixture.bridge, *successor);
        handlerCount +=
            second.takeHandlerCompletion().has_value() ? 1U : 0U;
    }
    require(handlerCount == 1U,
            "post-vs-reap lost or duplicated handler ownership");
    require(fixture.api->dataPostSuccessCount() >= 1U &&
                fixture.api->dataPostSuccessCount() <= 2U,
            "post-vs-reap exceeded the one-successor bound");
    require(fixture.api->pendingDataCount() == 0U &&
                fixture.bridge->snapshot().fullyDrained(),
            "post-vs-reap left a packet or owner state behind");
}

void racesSignalAgainstShutdownWithinTombstoneBound()
{
    Fixture fixture;
    std::atomic<bool> start{};
    std::thread signaler{[&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        fixture.bridge->signal();
    }};
    std::thread closer{[&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        fixture.bridge->shutdown();
    }};
    start.store(true, std::memory_order_release);
    signaler.join();
    closer.join();

    require(fixture.api->pendingDataCount() <= 1U,
            "signal-vs-shutdown exceeded one packet");
    if (const auto packet = fixture.api->takeDataPacket()) {
        auto drained = reapPacket(*fixture.bridge, *packet);
        require(drained.disposition() ==
                    ReapDisposition::RetiredNotificationDrained,
                "signal-vs-shutdown did not produce a tombstone");
    }
    fixture.bridge->signal();
    const auto snapshot = fixture.bridge->snapshot();
    require(snapshot.isShutdown() && !snapshot.isFatal(),
            "signal-vs-shutdown became fatal");
    require(snapshot.fullyDrained() &&
                fixture.api->pendingDataCount() == 0U,
            "signal-vs-shutdown did not fully drain");
}

using TestFunction = void (*)();

constexpr std::array<TestFunction, 13U> Tests{
    validatesConstructionAndFixedContracts,
    postsAndMovesOneHandlerCompletionExactlyOnce,
    coalescesSseSignalsAndSupportsSynchronousAttach,
    sharesOnePacketForSimultaneousBoundedPayload,
    duplicateHandlerIsFatalAndRetainsTombstone,
    postFailureConsumesPayloadAndNotifiesFatal,
    foreignPointerIsNonmutating,
    exactMalformedPacketsConsumeBeforeFatal,
    shutdownRetainsOnlyAnExactTombstone,
    destroysDiscardedCompletionOnlyAfterUnlock,
    toleratesExpiredAndReentrantFatalSinks,
    racesHandlerPostAgainstExactReapWithoutLoss,
    racesSignalAgainstShutdownWithinTombstoneBound,
};

} // namespace

int main()
{
    try {
        for (const auto test : Tests) {
            test();
        }
        std::cout << "DashboardConnectionEventBridgeTests passed: "
                  << Tests.size() << " cases, " << assertionCount
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DashboardConnectionEventBridgeTests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    }
}
