#include "Infrastructure/Windows/Detail/DashboardOverloadResponderSet.h"

#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"
#include "Infrastructure/Windows/Detail/DashboardListeningSocket.h"
#include "Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.h"
#include "Infrastructure/Windows/Detail/DashboardWinsockRuntime.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using AdmissionController = Detail::DashboardAdmissionController;
using Associator = Detail::IDashboardOverloadSocketAssociator;
using CompletionKey = Detail::DashboardIoCompletionKey;
using ConnectionApi = Detail::IDashboardConnectionSocketApi;
using Endpoint = Detail::DashboardLoopbackEndpoint;
using Handoff = Detail::DashboardAcceptedConnectionHandoff;
using HandoffDisposition =
    Detail::DashboardAcceptedConnectionHandoffDisposition;
using Kernel = Detail::DashboardIocpWorkerKernel;
using KernelSink = Detail::IDashboardIocpCompletionSink;
using Listener = Detail::DashboardListeningSocket;
using OverloadDisposition = Detail::DashboardOverloadAdmissionDisposition;
using Pool = Detail::DashboardOverloadResponderSet;
using Port = Detail::DashboardIoCompletionPort;
using ReapDisposition = Detail::DashboardOverloadReapDisposition;
using Router = Detail::DashboardIocpCompletionRouter;
using Runtime = Detail::DashboardWinsockRuntime;
using RuntimeServices = Detail::DashboardConnectionRuntimeServices;
using Set = Detail::DashboardAcceptSlotSet;
using Deadline =
    ForgeConductor::Infrastructure::Windows::WindowsDashboardDeadline;
using DeadlineScheduler =
    ForgeConductor::Infrastructure::Windows::
        WindowsDashboardDeadlineScheduler;
using DeadlineSink =
    ForgeConductor::Infrastructure::Windows::
        IWindowsDashboardDeadlineSink;

using namespace std::chrono_literals;

constexpr CompletionKey ListenerKey{0xA551U};
constexpr CompletionKey RetiringListenerKey{0xA552U};
constexpr CompletionKey ResponderKey{0x5030U};
constexpr CompletionKey DeadlineFallbackKey{0xD503U};
constexpr std::uint64_t GenerationId = 17U;
constexpr std::uint64_t RetiringGenerationId = 18U;
constexpr auto OperationTimeout = 5s;

static_assert(std::is_final_v<Pool>);
static_assert(std::is_base_of_v<
              Detail::IDashboardFixedIocpCompletionTarget, Pool>);
static_assert(std::is_base_of_v<
              Detail::IDashboardAdmissionOverloadResponder, Pool>);
static_assert(std::is_base_of_v<
              Detail::IDashboardAuxiliaryDeadlineTarget, Pool>);
static_assert(!std::is_copy_constructible_v<Pool>);
static_assert(!std::is_move_constructible_v<Pool>);
static_assert(Pool::Capacity == 2U * Set::SlotCount);
static_assert(Pool::CancellationReapLifetime == 5s);
static_assert(noexcept(std::declval<Pool&>().respond(
    std::declval<Detail::DashboardAdmissionOverloadWork>())));
static_assert(noexcept(std::declval<Pool&>().consume(
    std::declval<Detail::DashboardIoCompletionPacket>(), ERROR_SUCCESS)));
static_assert(noexcept(std::declval<Pool&>().fatal(ERROR_INVALID_HANDLE)));
static_assert(noexcept(std::declval<Pool&>().beginShutdown()));
static_assert(noexcept(std::declval<Pool&>().dispatchDeadline({})));

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

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const std::string_view context)
{
    require(!result, context);
    require(result.error().code == code,
            "failure used the wrong stable error code");
}

[[nodiscard]] std::chrono::milliseconds remainingTime(
    const std::chrono::steady_clock::time_point deadline,
    const std::string_view message)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        fail(message);
    }
    return (std::max)(
        1ms,
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
}

[[nodiscard]] SHORT waitForSocket(
    const SOCKET socket,
    const SHORT events,
    const std::chrono::steady_clock::time_point deadline,
    const std::string_view timeoutMessage)
{
    for (;;) {
        WSAPOLLFD descriptor{};
        descriptor.fd = socket;
        descriptor.events = events;
        const auto remaining = remainingTime(deadline, timeoutMessage);
        const int status = ::WSAPoll(
            std::addressof(descriptor),
            1U,
            static_cast<int>(remaining.count()));
        if (status > 0) {
            return descriptor.revents;
        }
        if (status == 0) {
            fail(timeoutMessage);
        }
        if (::WSAGetLastError() != WSAEINTR) {
            fail("bounded socket poll failed");
        }
    }
}

void connectWithDeadline(
    const SOCKET socket,
    const sockaddr* const address,
    const int addressLength)
{
    u_long nonblocking = 1U;
    require(
        ::ioctlsocket(socket, FIONBIO, &nonblocking) == 0,
        "overload client could not enter nonblocking mode");
    if (::connect(socket, address, addressLength) == 0) {
        return;
    }
    const int error = ::WSAGetLastError();
    require(
        error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
            error == WSAEALREADY,
        "overload client connect failed before its bounded wait");
    const auto deadline = std::chrono::steady_clock::now() + OperationTimeout;
    const SHORT events = waitForSocket(
        socket,
        POLLOUT,
        deadline,
        "overload client connect exceeded its deadline");
    require((events & POLLNVAL) == 0,
            "overload client connect polled an invalid socket");
    int finalError{};
    int finalErrorLength = static_cast<int>(sizeof(finalError));
    require(
        ::getsockopt(
            socket,
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(std::addressof(finalError)),
            std::addressof(finalErrorLength)) == 0 &&
            finalError == 0,
        "overload client connect completed with an error");
}

[[nodiscard]] std::vector<std::byte> receiveExact(
    const SOCKET socket,
    const std::size_t expectedLength)
{
    std::vector<std::byte> bytes(expectedLength);
    std::size_t offset{};
    const auto deadline = std::chrono::steady_clock::now() + OperationTimeout;
    while (offset < bytes.size()) {
        static_cast<void>(remainingTime(
            deadline, "fixed overload response exceeded its receive deadline"));
        const int received = ::recv(
            socket,
            reinterpret_cast<char*>(bytes.data() + offset),
            static_cast<int>(bytes.size() - offset),
            0);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) {
            fail("fixed overload response closed before all bytes arrived");
        }
        const int error = ::WSAGetLastError();
        if (error == WSAEINTR) {
            continue;
        }
        require(error == WSAEWOULDBLOCK,
                "fixed overload response receive failed");
        static_cast<void>(waitForSocket(
            socket,
            POLLIN,
            deadline,
            "fixed overload response exceeded its receive deadline"));
    }
    return bytes;
}

class CollectingSink final : public KernelSink {
public:
    struct Completion final {
        Detail::DashboardIoCompletionPacket packet;
        DWORD nativeError{};
    };

    void consume(
        const Detail::DashboardIoCompletionPacket packet,
        const DWORD nativeError) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (count_ == queue_.size()) {
            overflow_ = true;
            return;
        }
        const std::size_t tail = (head_ + count_) % queue_.size();
        queue_[tail] = Completion{packet, nativeError};
        ++count_;
        changed_.notify_one();
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        const std::lock_guard lock{mutex_};
        fatalError_ = nativeError;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitTake(
        Completion& completion,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        const bool ready = changed_.wait_for(
            lock,
            timeout,
            [this] { return count_ != 0U || fatalError_.has_value(); });
        if (!ready || count_ == 0U) {
            return false;
        }
        completion = queue_[head_];
        head_ = (head_ + 1U) % queue_.size();
        --count_;
        return true;
    }

    [[nodiscard]] bool healthy() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return !overflow_ && !fatalError_.has_value();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Completion, 128U> queue_{};
    std::size_t head_{};
    std::size_t count_{};
    bool overflow_{};
    std::optional<DWORD> fatalError_;
};

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
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
    std::atomic_int64_t milliseconds_{10'000};
};

class ForwardingDeadlineSink final : public DeadlineSink {
public:
    void attach(std::weak_ptr<Pool> pool) noexcept
    {
        const std::lock_guard lock{mutex_};
        pool_ = std::move(pool);
    }

    void signal(Deadline deadline) noexcept override
    {
        std::shared_ptr<Pool> pool;
        {
            const std::lock_guard lock{mutex_};
            pool = pool_.lock();
        }
        if (pool != nullptr) {
            pool->dispatchDeadline(std::move(deadline));
        }
        {
            const std::lock_guard lock{mutex_};
            ++signalCount_;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool waitForSignals(
        const std::size_t count,
        const std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this, count] { return signalCount_ >= count; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::weak_ptr<Pool> pool_;
    std::size_t signalCount_{};
};

class FixedUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        return Domain::Uuid::parse(
            "10000000-0000-4000-8000-000000000001");
    }
};

class FixedOperationalState final
    : public Detail::IDashboardOperationalStateSource {
public:
    [[nodiscard]] bool operationalServiceActive() const noexcept override
    {
        return true;
    }
};

class UnexpectedOwnerFactory final
    : public Detail::IDashboardAcceptedConnectionOwnerFactory {
public:
    [[nodiscard]] const void* applicationPolicyIdentity()
        const noexcept override
    {
        return this;
    }

    [[nodiscard]] Domain::Result<
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget>>
    createOwner(
        std::uint64_t,
        Detail::DashboardConnectionRuntimeIdentity,
        Domain::MonotonicTimePoint,
        AdmissionController::Lease,
        Detail::DashboardAcceptedConnection) noexcept override
    {
        ++calls;
        return Domain::Result<std::shared_ptr<
            Detail::IDashboardConnectionDispatchTarget>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Overload test unexpectedly created a connection owner."));
    }

    std::size_t calls{};
};

class UnexpectedRegistrar final : public Detail::IDashboardConnectionRegistrar {
public:
    [[nodiscard]] Domain::Result<void> registerConnection(
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget>)
        noexcept override
    {
        ++calls;
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Overload test unexpectedly registered a connection."));
    }

    std::size_t calls{};
};

class FakeSocketApi final : public ConnectionApi {
public:
    struct SendCall final {
        SOCKET socket{INVALID_SOCKET};
        const char* bytes{};
        ULONG length{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] int receive(
        SOCKET,
        WSABUF*,
        DWORD,
        DWORD&,
        DWORD&,
        OVERLAPPED*) noexcept override
    {
        return SOCKET_ERROR;
    }

    [[nodiscard]] int send(
        const SOCKET socket,
        WSABUF* const buffers,
        const DWORD bufferCount,
        DWORD& transferredBytes,
        DWORD,
        OVERLAPPED* const operation) noexcept override
    {
        const std::lock_guard lock{mutex_};
        transferredBytes = 0U;
        if (bufferCount != 1U || buffers == nullptr ||
            operation == nullptr || sendCallCount_ == sendCalls_.size()) {
            sendError_ = WSAEINVAL;
            return SOCKET_ERROR;
        }
        sendCalls_[sendCallCount_++] = SendCall{
            socket,
            buffers[0U].buf,
            buffers[0U].len,
            operation};
        rememberOperationLocked(operation);
        lastSocketError_ = sendError_;
        return sendStatus_;
    }

    [[nodiscard]] BOOL cancel(
        SOCKET,
        OVERLAPPED* const operation) noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++cancelCalls_;
        lastCancelledOperation_ = operation;
        return cancelResult_;
    }

    [[nodiscard]] int shutdownSocket(SOCKET, int) noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++shutdownCalls_;
        lastSocketError_ = shutdownError_;
        return shutdownStatus_;
    }

    [[nodiscard]] int lastSocketError() noexcept override
    {
        const std::lock_guard lock{mutex_};
        return lastSocketError_;
    }

    [[nodiscard]] DWORD lastSystemError() noexcept override
    {
        const std::lock_guard lock{mutex_};
        return cancelError_;
    }

    void failSend(const int nativeError) noexcept
    {
        const std::lock_guard lock{mutex_};
        sendStatus_ = SOCKET_ERROR;
        sendError_ = nativeError;
    }

    void failCancel(const DWORD nativeError) noexcept
    {
        const std::lock_guard lock{mutex_};
        cancelResult_ = FALSE;
        cancelError_ = nativeError;
    }

    void failShutdown(const int nativeError) noexcept
    {
        const std::lock_guard lock{mutex_};
        shutdownStatus_ = SOCKET_ERROR;
        shutdownError_ = nativeError;
    }

    [[nodiscard]] std::size_t sendCallCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return sendCallCount_;
    }

    [[nodiscard]] SendCall sendCall(const std::size_t index) const noexcept
    {
        const std::lock_guard lock{mutex_};
        return sendCalls_[index];
    }

    [[nodiscard]] std::array<OVERLAPPED*, Pool::Capacity>
    uniqueOperations() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return uniqueOperations_;
    }

    [[nodiscard]] std::size_t uniqueOperationCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return uniqueOperationCount_;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return cancelCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return shutdownCalls_;
    }

private:
    void rememberOperationLocked(OVERLAPPED* const operation) noexcept
    {
        for (std::size_t index{}; index < uniqueOperationCount_; ++index) {
            if (uniqueOperations_[index] == operation) {
                return;
            }
        }
        if (uniqueOperationCount_ < uniqueOperations_.size()) {
            uniqueOperations_[uniqueOperationCount_++] = operation;
        }
    }

    mutable std::mutex mutex_;
    std::array<SendCall, 32U> sendCalls_{};
    std::array<OVERLAPPED*, Pool::Capacity> uniqueOperations_{};
    std::size_t sendCallCount_{};
    std::size_t uniqueOperationCount_{};
    std::size_t cancelCalls_{};
    std::size_t shutdownCalls_{};
    int sendStatus_{SOCKET_ERROR};
    int sendError_{WSA_IO_PENDING};
    int shutdownError_{};
    int lastSocketError_{WSA_IO_PENDING};
    BOOL cancelResult_{TRUE};
    DWORD cancelError_{ERROR_NOT_FOUND};
    int shutdownStatus_{};
    OVERLAPPED* lastCancelledOperation_{};
};

class RecordingDrainObserver final
    : public Detail::IDashboardAdmissionOverloadDrainObserver {
public:
    void overloadOwnerBeganShutdown() noexcept override
    {
        std::unique_lock lock{mutex_};
        ++ownerShutdownCount_;
        ownerShutdownEntered_ = true;
        ownerShutdownChanged_.notify_all();
        ownerShutdownChanged_.wait(lock, [this]() noexcept {
            return !blockOwnerShutdown_ || releaseOwnerShutdown_;
        });
    }

    void overloadOwnerBecameTerminal() noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++ownerTerminalCount_;
    }

    void blockOwnerShutdown() noexcept
    {
        const std::lock_guard lock{mutex_};
        blockOwnerShutdown_ = true;
        releaseOwnerShutdown_ = false;
        ownerShutdownEntered_ = false;
    }

    [[nodiscard]] bool waitForOwnerShutdownEntered() noexcept
    {
        std::unique_lock lock{mutex_};
        return ownerShutdownChanged_.wait_for(
            lock, OperationTimeout,
            [this]() noexcept { return ownerShutdownEntered_; });
    }

    void releaseOwnerShutdown() noexcept
    {
        {
            const std::lock_guard lock{mutex_};
            releaseOwnerShutdown_ = true;
        }
        ownerShutdownChanged_.notify_all();
    }

    void overloadGenerationMayHaveDrained(
        const std::uint64_t generationId) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (notificationCount_ < generationIds_.size()) {
            generationIds_[notificationCount_] = generationId;
        }
        ++notificationCount_;
    }

    void overloadGenerationBecameTerminal(
        const std::uint64_t generationId) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (terminalNotificationCount_ < terminalGenerationIds_.size()) {
            terminalGenerationIds_[terminalNotificationCount_] =
                generationId;
        }
        ++terminalNotificationCount_;
    }

    void overloadGenerationTerminalPending(
        const std::uint64_t generationId) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (terminalPendingCount_ < terminalPendingGenerationIds_.size()) {
            terminalPendingGenerationIds_[terminalPendingCount_] =
                generationId;
        }
        ++terminalPendingCount_;
    }

    void overloadGenerationCompletionPending(
        const std::uint64_t generationId) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (completionPendingCount_ <
            completionPendingGenerationIds_.size()) {
            completionPendingGenerationIds_[completionPendingCount_] =
                generationId;
        }
        ++completionPendingCount_;
    }

    void overloadGenerationCompletionSettled(
        const std::uint64_t generationId) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (completionSettledCount_ <
            completionSettledGenerationIds_.size()) {
            completionSettledGenerationIds_[completionSettledCount_] =
                generationId;
        }
        ++completionSettledCount_;
    }

    [[nodiscard]] std::size_t notificationCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return notificationCount_;
    }

    [[nodiscard]] std::size_t notificationCountFor(
        const std::uint64_t generationId) const noexcept
    {
        const std::lock_guard lock{mutex_};
        std::size_t count{};
        const auto retained =
            (std::min)(notificationCount_, generationIds_.size());
        for (std::size_t index{}; index < retained; ++index) {
            if (generationIds_[index] == generationId) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t terminalNotificationCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return terminalNotificationCount_;
    }

    [[nodiscard]] std::size_t ownerShutdownCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return ownerShutdownCount_;
    }

    [[nodiscard]] std::size_t ownerTerminalCount() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return ownerTerminalCount_;
    }

    [[nodiscard]] std::size_t terminalNotificationCountFor(
        const std::uint64_t generationId) const noexcept
    {
        const std::lock_guard lock{mutex_};
        std::size_t count{};
        const auto retained = (std::min)(
            terminalNotificationCount_, terminalGenerationIds_.size());
        for (std::size_t index{}; index < retained; ++index) {
            if (terminalGenerationIds_[index] == generationId) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t terminalPendingCountFor(
        const std::uint64_t generationId) const noexcept
    {
        const std::lock_guard lock{mutex_};
        std::size_t count{};
        const auto retained = (std::min)(
            terminalPendingCount_, terminalPendingGenerationIds_.size());
        for (std::size_t index{}; index < retained; ++index) {
            if (terminalPendingGenerationIds_[index] == generationId) {
                ++count;
            }
        }
        return count;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ownerShutdownChanged_;
    std::array<std::uint64_t, 32U> generationIds_{};
    std::array<std::uint64_t, 32U> terminalGenerationIds_{};
    std::array<std::uint64_t, 32U> terminalPendingGenerationIds_{};
    std::array<std::uint64_t, 32U> completionPendingGenerationIds_{};
    std::array<std::uint64_t, 32U> completionSettledGenerationIds_{};
    std::size_t notificationCount_{};
    std::size_t ownerShutdownCount_{};
    std::size_t ownerTerminalCount_{};
    std::size_t terminalNotificationCount_{};
    std::size_t terminalPendingCount_{};
    std::size_t completionPendingCount_{};
    std::size_t completionSettledCount_{};
    bool blockOwnerShutdown_{};
    bool releaseOwnerShutdown_{};
    bool ownerShutdownEntered_{};
};

class RecordingFailFast final : public Detail::IDashboardOverloadFailFast {
public:
    void failFast() noexcept override
    {
        calls_.fetch_add(1U);
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        return calls_.load();
    }

private:
    std::atomic_size_t calls_{};
};

class BlockingSocketAssociator final : public Associator {
public:
    [[nodiscard]] Domain::Result<void> associateSocket(
        const SOCKET socket,
        const CompletionKey completionKey) noexcept override
    {
        std::unique_lock lock{mutex_};
        socket_ = socket;
        completionKey_ = completionKey;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });

        int socketType{};
        int socketTypeLength = static_cast<int>(sizeof(socketType));
        socketWasValidAtReturn_ = ::getsockopt(
            socket,
            SOL_SOCKET,
            SO_TYPE,
            reinterpret_cast<char*>(std::addressof(socketType)),
            std::addressof(socketTypeLength)) == 0;
        returned_ = true;
        changed_.notify_all();
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool waitUntilEntered(
        const std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this] { return entered_; });
    }

    void release() noexcept
    {
        const std::lock_guard lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool returnedWithValidSocket() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return returned_ && socketWasValidAtReturn_ &&
            socket_ != INVALID_SOCKET && completionKey_ == ResponderKey;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    SOCKET socket_{INVALID_SOCKET};
    CompletionKey completionKey_{0U};
    bool entered_{};
    bool released_{};
    bool returned_{};
    bool socketWasValidAtReturn_{};
};

class Harness final {
public:
    explicit Harness(
        std::shared_ptr<FakeSocketApi> fakeSocketApi = nullptr,
        const bool includeRetiringGeneration = false,
        std::shared_ptr<Associator> socketAssociator = nullptr)
        : fakeSocketApi_{std::move(fakeSocketApi)},
          includeRetiringGeneration_{includeRetiringGeneration},
          socketAssociator_{std::move(socketAssociator)}
    {
        runtime = take(Runtime::create());
        fallback = std::make_shared<CollectingSink>();
        router = take(Router::create(DeadlineFallbackKey, fallback));
        kernel = take(Kernel::create(take(Port::create()), router));

        std::optional<Listener> selectedListener;
        static std::atomic_uint32_t nextPort{27100U};
        const std::uint32_t firstPort = nextPort.fetch_add(128U);
        for (std::uint32_t offset{}; offset < 64U; ++offset) {
            const auto candidatePort = static_cast<std::uint16_t>(
                firstPort + offset);
            auto candidateEndpoint = Endpoint::create(
                "127.0.0.1", candidatePort);
            if (!candidateEndpoint) {
                continue;
            }
            auto candidateListener = Listener::create(
                *runtime, candidateEndpoint.value());
            if (candidateListener) {
                endpoint.emplace(std::move(candidateEndpoint).value());
                selectedListener.emplace(
                    std::move(candidateListener).value());
                break;
            }
        }
        if (!endpoint.has_value() || !selectedListener.has_value()) {
            fail("overload responder test could not bind a bounded port");
        }

        acceptSet = take(Set::create(
            *runtime, std::move(*selectedListener), ListenerKey));
        require(static_cast<bool>(acceptSet->start(*kernel)),
                "overload accept set did not start");
        if (includeRetiringGeneration_) {
            std::optional<Listener> selectedRetiringListener;
            for (std::uint32_t offset{}; offset < 64U; ++offset) {
                const auto candidatePort = static_cast<std::uint16_t>(
                    firstPort + 64U + offset);
                auto candidateEndpoint = Endpoint::create(
                    "127.0.0.1", candidatePort);
                if (!candidateEndpoint) {
                    continue;
                }
                auto candidateListener = Listener::create(
                    *runtime, candidateEndpoint.value());
                if (candidateListener) {
                    retiringEndpoint.emplace(
                        std::move(candidateEndpoint).value());
                    selectedRetiringListener.emplace(
                        std::move(candidateListener).value());
                    break;
                }
            }
            if (!retiringEndpoint.has_value() ||
                !selectedRetiringListener.has_value()) {
                fail("overload responder test could not bind its retiring generation");
            }
            retiringAcceptSet = take(Set::create(
                *runtime,
                std::move(*selectedRetiringListener),
                RetiringListenerKey));
            require(static_cast<bool>(retiringAcceptSet->start(*kernel)),
                    "retiring overload accept set did not start");
        }
        admission = take(AdmissionController::create({1U, 1U, 2U}));
        heldAdmission.emplace(take(admission->tryAccept()));
        clock = std::make_shared<FixedClock>();
        uuidGenerator = std::make_shared<FixedUuidGenerator>();
        operationalState = std::make_shared<FixedOperationalState>();
        const std::array fixedKeys{
            ListenerKey,
            RetiringListenerKey,
            ResponderKey,
            DeadlineFallbackKey};
        runtimeServices = take(RuntimeServices::create(
            clock, uuidGenerator, operationalState, fixedKeys));
        deadlineSink = std::make_shared<ForwardingDeadlineSink>();
        deadlineScheduler = take(DeadlineScheduler::create(
            clock, deadlineSink));
        deadlineRegistrationId = take(
            runtimeServices->allocateConnectionIdentity()).registrationId;
        responseCatalog = take(Detail::DashboardConnectionResponseCatalog::
                                   create());
        failFast = std::make_shared<RecordingFailFast>();
        if (fakeSocketApi_ == nullptr) {
            pool = take(Pool::create(
                *kernel,
                ResponderKey,
                deadlineRegistrationId,
                *deadlineScheduler,
                *runtimeServices,
                *responseCatalog));
        } else if (socketAssociator_ == nullptr) {
            pool = take(Pool::create(
                *kernel,
                ResponderKey,
                deadlineRegistrationId,
                *deadlineScheduler,
                *runtimeServices,
                *responseCatalog,
                fakeSocketApi_,
                failFast));
        } else {
            pool = take(Pool::create(
                ResponderKey,
                deadlineRegistrationId,
                *deadlineScheduler,
                *runtimeServices,
                *responseCatalog,
                fakeSocketApi_,
                failFast,
                socketAssociator_));
        }
        deadlineSink->attach(pool);
        drainObserver = std::make_shared<RecordingDrainObserver>();
        require(static_cast<bool>(pool->bindDrainObserver(drainObserver)),
                "overload drain observer was not bound");
        require(static_cast<bool>(router->registerFixedTarget(pool)),
                "overload pool was not registered with the IOCP router");
        ownerFactory = std::make_shared<UnexpectedOwnerFactory>();
        registrar = std::make_shared<UnexpectedRegistrar>();
        handoff = take(Handoff::create(
            GenerationId,
            *acceptSet,
            *admission,
            *runtimeServices,
            ownerFactory,
            registrar,
            pool));
        if (includeRetiringGeneration_) {
            retiringHandoff = take(Handoff::create(
                RetiringGenerationId,
                *retiringAcceptSet,
                *admission,
                *runtimeServices,
                ownerFactory,
                registrar,
                pool));
        }
    }

    ~Harness() noexcept
    {
        drainNoThrow();
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] Detail::UniqueDashboardSocket overloadOne(
        const bool retiring = false)
    {
        auto* const selectedEndpoint = retiring
            ? std::addressof(*retiringEndpoint)
            : std::addressof(*endpoint);
        auto* const selectedAcceptSet = retiring
            ? retiringAcceptSet.get()
            : acceptSet.get();
        auto* const selectedHandoff = retiring
            ? retiringHandoff.get()
            : handoff.get();
        const auto selectedKey = retiring
            ? RetiringListenerKey
            : ListenerKey;
        require(selectedAcceptSet != nullptr && selectedHandoff != nullptr,
                "requested overload generation was not prepared");
        auto client = take(runtime->createOverlappedTcpSocket(AF_INET));
        connectWithDeadline(
            client.get(),
            selectedEndpoint->nativeAddress(),
            selectedEndpoint->nativeAddressLength());

        CollectingSink::Completion completion{};
        require(fallback->waitTake(completion, 5s),
                "AcceptEx completion did not reach the fallback router");
        require(completion.packet.completionKey == selectedKey,
                "accepted overload connection used the wrong key");
        auto accepted = take(selectedAcceptSet->reap(
            completion.packet.completionKey,
            completion.packet.transferredBytes,
            completion.packet.operation,
            completion.nativeError));
        const auto transferred = selectedHandoff->consume(std::move(accepted));
        require(static_cast<bool>(transferred) &&
                    transferred.value() ==
                        HandoffDisposition::OverloadResponseTransferred,
                "admission rejection was not transferred to overload send");
        require(ownerFactory->calls == 0U && registrar->calls == 0U,
                "overload path entered ordinary connection ownership");
        return client;
    }

    [[nodiscard]] const std::vector<std::byte>& responseBytes() const noexcept
    {
        return *responseCatalog->genericServiceUnavailable();
    }

    void drainNoThrow() noexcept
    {
        if (pool != nullptr) {
            pool->beginShutdown();
            if (fakeSocketApi_ != nullptr) {
                const auto operations = fakeSocketApi_->uniqueOperations();
                const auto count = fakeSocketApi_->uniqueOperationCount();
                for (std::size_t index{};
                     index < count && pool->snapshot().activeCount() != 0U;
                     ++index) {
                    static_cast<void>(pool->reap(
                        ResponderKey,
                        0U,
                        operations[index],
                        ERROR_OPERATION_ABORTED));
                }
            } else {
                const auto deadline =
                    std::chrono::steady_clock::now() + OperationTimeout;
                while (pool->snapshot().activeCount() != 0U &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(5ms);
                }
            }
        }

        handoff.reset();
        retiringHandoff.reset();
        if (acceptSet != nullptr || retiringAcceptSet != nullptr) {
            if (acceptSet != nullptr) {
                static_cast<void>(
                    acceptSet->closeAdmissionAndRequestCancellation());
            }
            if (retiringAcceptSet != nullptr) {
                static_cast<void>(
                    retiringAcceptSet->closeAdmissionAndRequestCancellation());
            }
            const auto deadline =
                std::chrono::steady_clock::now() + OperationTimeout;
            while (((acceptSet != nullptr &&
                     !acceptSet->snapshot().fullyDrained()) ||
                    (retiringAcceptSet != nullptr &&
                     !retiringAcceptSet->snapshot().fullyDrained())) &&
                   std::chrono::steady_clock::now() < deadline) {
                CollectingSink::Completion completion{};
                if (!fallback->waitTake(completion, 50ms)) {
                    continue;
                }
                if (completion.packet.completionKey == ListenerKey) {
                    static_cast<void>(acceptSet->reap(
                        completion.packet.completionKey,
                        completion.packet.transferredBytes,
                        completion.packet.operation,
                        completion.nativeError));
                } else if (completion.packet.completionKey ==
                           RetiringListenerKey &&
                           retiringAcceptSet != nullptr) {
                    static_cast<void>(retiringAcceptSet->reap(
                        completion.packet.completionKey,
                        completion.packet.transferredBytes,
                        completion.packet.operation,
                        completion.nativeError));
                }
            }
        }
        heldAdmission.reset();
        if (pool != nullptr && router != nullptr) {
            static_cast<void>(router->unregisterFixedTarget(pool));
        }
        pool.reset();
        if (deadlineScheduler != nullptr) {
            deadlineScheduler->shutdown();
            deadlineScheduler.reset();
        }
        deadlineSink.reset();
        acceptSet.reset();
        retiringAcceptSet.reset();
        runtimeServices.reset();
        responseCatalog.reset();
        if (kernel != nullptr) {
            kernel->shutdown();
            kernel.reset();
        }
        router.reset();
        fallback.reset();
        drainObserver.reset();
        admission.reset();
        runtime.reset();
    }

    std::shared_ptr<FakeSocketApi> fakeSocketApi_;
    bool includeRetiringGeneration_{};
    std::shared_ptr<Associator> socketAssociator_;
    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<CollectingSink> fallback;
    std::shared_ptr<Router> router;
    std::unique_ptr<Kernel> kernel;
    std::optional<Endpoint> endpoint;
    std::optional<Endpoint> retiringEndpoint;
    std::unique_ptr<Set> acceptSet;
    std::unique_ptr<Set> retiringAcceptSet;
    std::unique_ptr<AdmissionController> admission;
    std::optional<AdmissionController::Lease> heldAdmission;
    std::shared_ptr<FixedClock> clock;
    std::shared_ptr<FixedUuidGenerator> uuidGenerator;
    std::shared_ptr<FixedOperationalState> operationalState;
    std::unique_ptr<RuntimeServices> runtimeServices;
    std::shared_ptr<ForwardingDeadlineSink> deadlineSink;
    std::unique_ptr<DeadlineScheduler> deadlineScheduler;
    std::uint64_t deadlineRegistrationId{};
    std::unique_ptr<Detail::DashboardConnectionResponseCatalog>
        responseCatalog;
    std::shared_ptr<RecordingFailFast> failFast;
    std::shared_ptr<Pool> pool;
    std::shared_ptr<RecordingDrainObserver> drainObserver;
    std::shared_ptr<UnexpectedOwnerFactory> ownerFactory;
    std::shared_ptr<UnexpectedRegistrar> registrar;
    std::unique_ptr<Handoff> handoff;
    std::unique_ptr<Handoff> retiringHandoff;
};

void invalidFixedOwnerIsRejected()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    requireError(
        Pool::create(
            *harness.kernel,
            CompletionKey{0U},
            harness.deadlineRegistrationId,
            *harness.deadlineScheduler,
            *harness.runtimeServices,
            *harness.responseCatalog,
            fake,
            harness.failFast),
        Domain::ErrorCodes::InvalidRequest,
        "zero overload completion key was accepted");
    requireError(
        Pool::create(
            *harness.kernel,
            CompletionKey{Kernel::ShutdownKeyValue},
            harness.deadlineRegistrationId,
            *harness.deadlineScheduler,
            *harness.runtimeServices,
            *harness.responseCatalog,
            fake,
            harness.failFast),
        Domain::ErrorCodes::InvalidRequest,
        "reserved overload shutdown key was accepted");
    requireError(
        Pool::create(
            ResponderKey,
            harness.deadlineRegistrationId,
            *harness.deadlineScheduler,
            *harness.runtimeServices,
            *harness.responseCatalog,
            fake,
            harness.failFast,
            nullptr),
        Domain::ErrorCodes::InvalidRequest,
        "null overload socket associator was accepted");
}

void partialSendRetainsAndReissuesTheExactSuffix()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto before = harness.pool->snapshot();
    require(before.activeCount() == 1U && before.sendingCount() == 1U,
            "overload response did not occupy one fixed sender");
    require(harness.acceptSet->snapshot().pausedCount() == 1U,
            "overload response returned its accept token before send reap");
    require(fake->sendCallCount() == 1U,
            "overload response did not issue one initial send");
    const auto first = fake->sendCall(0U);
    require(first.length == harness.responseBytes().size() &&
                first.bytes == reinterpret_cast<const char*>(
                    harness.responseBytes().data()),
            "initial overload send did not borrow the immutable fixed 503");

    constexpr DWORD FirstTransfer = 11U;
    const auto partial = harness.pool->reap(
        ResponderKey,
        FirstTransfer,
        first.operation,
        ERROR_SUCCESS);
    require(static_cast<bool>(partial) &&
                partial.value() ==
                    ReapDisposition::PartialSendReissued,
            "partial overload completion did not reissue its suffix");
    require(fake->sendCallCount() == 2U,
            "partial overload completion did not issue exactly one suffix");
    const auto second = fake->sendCall(1U);
    require(second.operation == first.operation &&
                second.bytes == first.bytes + FirstTransfer &&
                second.length == first.length - FirstTransfer,
            "partial overload send lost exact operation or suffix ownership");
    require(harness.acceptSet->snapshot().pausedCount() == 1U,
            "partial overload send resumed acceptance early");

    const auto completed = harness.pool->reap(
        ResponderKey,
        second.length,
        second.operation,
        ERROR_SUCCESS);
    require(static_cast<bool>(completed) &&
                completed.value() == ReapDisposition::ResponseDelivered,
            "final overload suffix did not complete delivery");
    const auto after = harness.pool->snapshot();
    require(after.activeCount() == 0U && after.deliveredCount() == 1U &&
                after.abandonedCount() == 0U,
            "delivered overload response did not release its fixed sender");
    require(harness.acceptSet->snapshot().pausedCount() == 0U,
            "delivered overload response did not return its accept token");
}

void routerDispatchesTheOwnedCompletionKey()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    harness.router->consume(
        Detail::DashboardIoCompletionPacket{
            send.length, ResponderKey, send.operation},
        ERROR_SUCCESS);
    require(harness.pool->snapshot().deliveredCount() == 1U &&
                harness.pool->snapshot().activeCount() == 0U,
            "process router did not dispatch the fixed overload key");
    const auto routerSnapshot = harness.router->snapshot();
    require(routerSnapshot.fixedDispatchCount() == 1U &&
                routerSnapshot.fallbackDispatchCount() >= 1U,
            "router counters did not distinguish overload and accept keys");
}

void peerFailureAbandonsOnlyTheExactResponder()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    const auto abandoned = harness.pool->reap(
        ResponderKey,
        0U,
        send.operation,
        ERROR_NETNAME_DELETED);
    require(static_cast<bool>(abandoned) &&
                abandoned.value() == ReapDisposition::ResponseAbandoned,
            "peer failure was not a bounded best-effort abandonment");
    const auto snapshot = harness.pool->snapshot();
    require(snapshot.activeCount() == 0U &&
                snapshot.abandonedCount() == 1U &&
                snapshot.lifecycleFailure() == nullptr,
            "ordinary peer failure contaminated global responder health");
    require(harness.acceptSet->snapshot().pausedCount() == 0U,
            "peer failure stranded the exact accept token");
}

void immediateSendFailureClosesAndReturnsWithoutNativeObligation()
{
    auto fake = std::make_shared<FakeSocketApi>();
    fake->failSend(WSAECONNRESET);
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto snapshot = harness.pool->snapshot();
    require(snapshot.activeCount() == 0U &&
                snapshot.abandonedCount() == 1U &&
                snapshot.lifecycleFailure() == nullptr,
            "immediate peer loss retained a nonexistent send obligation");
    require(harness.acceptSet->snapshot().pausedCount() == 0U,
            "immediate send failure did not return the exact accept token");
}

void activeAndRetiringGenerationsFillEightBoundedResponders()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake, true};
    std::array<Detail::UniqueDashboardSocket, Pool::Capacity> clients{
        harness.overloadOne(),
        harness.overloadOne(),
        harness.overloadOne(),
        harness.overloadOne(),
        harness.overloadOne(true),
        harness.overloadOne(true),
        harness.overloadOne(true),
        harness.overloadOne(true)};
    auto snapshot = harness.pool->snapshot();
    require(snapshot.activeCount() == Pool::Capacity &&
                snapshot.availableCount() == 0U &&
                fake->uniqueOperationCount() == Pool::Capacity,
            "active and retiring work did not fill eight fixed responders");
    require(harness.acceptSet->snapshot().pausedCount() == Set::SlotCount,
            "active overload responses lost their four accept tokens");
    require(
        harness.retiringAcceptSet->snapshot().pausedCount() == Set::SlotCount,
        "retiring overload responses lost their four accept tokens");

    const auto cancelled = harness.pool->cancelGeneration(GenerationId);
    snapshot = harness.pool->snapshot();
    require(cancelled == Set::SlotCount &&
                snapshot.cancellationRequestedCount() == Set::SlotCount &&
                !snapshot.fullyDrained(),
            "generation cancellation released native obligations before reap");
    require(fake->cancelCalls() == Set::SlotCount &&
                fake->shutdownCalls() == Set::SlotCount,
            "generation cancellation did not target four active senders");
    require(harness.acceptSet->snapshot().awaitingReturnCount() ==
                Set::SlotCount,
            "generation cancellation did not close its exact origin");
    require(
        harness.retiringAcceptSet->snapshot().pausedCount() == Set::SlotCount,
        "active-generation cancellation changed retiring responders");

    const auto operations = fake->uniqueOperations();
    const auto sendCountBeforePartial = fake->sendCallCount();
    const auto partialAfterCancellation = harness.pool->reap(
        ResponderKey,
        1U,
        operations[0U],
        ERROR_SUCCESS);
    require(static_cast<bool>(partialAfterCancellation) &&
                partialAfterCancellation.value() ==
                    ReapDisposition::ResponseAbandoned &&
                fake->sendCallCount() == sendCountBeforePartial,
            "generation cancellation reissued a partial 503 suffix");

    for (std::size_t index{1U}; index < Set::SlotCount; ++index) {
        const auto reaped = harness.pool->reap(
            ResponderKey,
            0U,
            operations[index],
            ERROR_OPERATION_ABORTED);
        require(static_cast<bool>(reaped) &&
                    reaped.value() == ReapDisposition::ResponseAbandoned,
                "cancelled overload send did not reap as abandoned");
        require(harness.pool->snapshot().activeCount() ==
                    Pool::Capacity - index - 1U,
                "generation cancellation released more than one exact reap");
    }
    require(harness.pool->snapshot().activeCount() == Set::SlotCount,
            "active-generation reaps changed retiring-generation ownership");
    require(harness.acceptSet->snapshot().fullyDrained(),
            "cancelled origin did not drain after all four exact token returns");
    require(
        harness.drainObserver->notificationCountFor(GenerationId) ==
            Set::SlotCount &&
            harness.drainObserver->notificationCountFor(
                RetiringGenerationId) == 0U,
        "exact cancellation reaps emitted the wrong generation drain edge");

    for (std::size_t index{Set::SlotCount}; index < Pool::Capacity; ++index) {
        const auto reaped = harness.pool->reap(
            ResponderKey,
            static_cast<DWORD>(harness.responseBytes().size()),
            operations[index],
            ERROR_SUCCESS);
        require(static_cast<bool>(reaped) &&
                    reaped.value() == ReapDisposition::ResponseDelivered,
                "retiring fixed 503 did not complete after active cancellation");
    }
    require(harness.pool->snapshot().activeCount() == 0U &&
                harness.retiringAcceptSet->snapshot().pausedCount() == 0U,
            "eight bounded responders did not drain after exact reaps");
    require(
        harness.drainObserver->notificationCountFor(
            RetiringGenerationId) == Set::SlotCount,
        "normal overload completion omitted the retiring generation edge");
}

void shutdownCompletionDoesNotReissueAPartialResponse()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    harness.pool->beginShutdown();
    harness.pool->beginShutdown();
    harness.pool->drainTerminalGenerationNotifications();
    require(harness.drainObserver->ownerShutdownCount() == 1U &&
                harness.drainObserver->terminalNotificationCount() == 0U,
            "ordinary overload shutdown duplicated its owner edge or was misclassified as terminal");
    const auto reaped = harness.pool->reap(
        ResponderKey,
        1U,
        send.operation,
        ERROR_SUCCESS);
    require(static_cast<bool>(reaped) &&
                reaped.value() == ReapDisposition::ResponseAbandoned,
            "shutdown-winning partial completion was not abandoned");
    require(fake->sendCallCount() == 1U,
            "shutdown-winning partial completion issued another suffix");
    require(harness.pool->snapshot().fullyDrained(),
            "shutdown-winning partial completion retained responder work");
    require(harness.acceptSet->snapshot().awaitingReturnCount() == 0U,
            "shutdown-winning partial completion stranded its token");
}

void concurrentShutdownWaitsForCoordinatorPublication()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    harness.drainObserver->blockOwnerShutdown();

    std::thread first{[pool = harness.pool]() noexcept {
        pool->beginShutdown();
    }};
    const bool publicationEntered =
        harness.drainObserver->waitForOwnerShutdownEntered();

    std::mutex secondMutex;
    std::condition_variable secondChanged;
    bool secondStarted{};
    bool secondReturned{};
    std::thread second{[&]() noexcept {
        {
            const std::lock_guard lock{secondMutex};
            secondStarted = true;
        }
        secondChanged.notify_all();
        harness.pool->beginShutdown();
        {
            const std::lock_guard lock{secondMutex};
            secondReturned = true;
        }
        secondChanged.notify_all();
    }};

    bool returnedBeforePublication{};
    {
        std::unique_lock lock{secondMutex};
        static_cast<void>(secondChanged.wait_for(
            lock, OperationTimeout,
            [&]() noexcept { return secondStarted; }));
        returnedBeforePublication = secondChanged.wait_for(
            lock, 100ms,
            [&]() noexcept { return secondReturned; });
    }
    const auto cancellationsBeforePublication = fake->cancelCalls();
    harness.drainObserver->releaseOwnerShutdown();
    first.join();
    second.join();

    require(publicationEntered && !returnedBeforePublication &&
                cancellationsBeforePublication == 0U,
            "a concurrent shutdown cancelled or returned before coordinator publication");
    require(harness.drainObserver->ownerShutdownCount() == 1U &&
                fake->cancelCalls() == 1U &&
                fake->shutdownCalls() == 1U,
            "serialized shutdown duplicated publication or native cancellation");

    const auto reaped = harness.pool->reap(
        ResponderKey,
        0U,
        send.operation,
        ERROR_OPERATION_ABORTED);
    require(static_cast<bool>(reaped) &&
                harness.pool->snapshot().fullyDrained(),
            "serialized shutdown did not retain and reap its exact native send");
}

void sharedDeadlineExpiresFourStalledActiveResponses()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    std::array<Detail::UniqueDashboardSocket, Set::SlotCount> clients{
        harness.overloadOne(),
        harness.overloadOne(),
        harness.overloadOne(),
        harness.overloadOne()};
    auto snapshot = harness.pool->snapshot();
    require(snapshot.activeCount() == Set::SlotCount &&
                snapshot.deadlineArmed() &&
                harness.deadlineScheduler->snapshot().scheduledCount() == 1U,
            "four stalled overload sends did not share one bounded deadline arm");

    harness.clock->advance(std::chrono::duration_cast<std::chrono::milliseconds>(
        Pool::ResponseLifetime));
    require(harness.deadlineSink->waitForSignals(1U, OperationTimeout),
            "stalled overload response deadline was not delivered");

    snapshot = harness.pool->snapshot();
    require(snapshot.expiredCount() == Set::SlotCount &&
                snapshot.cancellationRequestedCount() == Set::SlotCount &&
                snapshot.deadlineArmed() &&
                !snapshot.shutdownRequested(),
            "one exact overload deadline did not arm bounded cancellation reap");
    require(fake->cancelCalls() == Set::SlotCount &&
                fake->shutdownCalls() == Set::SlotCount,
            "overload expiry did not cancel and shut down each due client");
    require(harness.acceptSet->snapshot().admissionOpen() &&
                harness.acceptSet->snapshot().pausedCount() == Set::SlotCount,
            "per-client overload expiry closed its healthy listener generation");

    const auto operations = fake->uniqueOperations();
    for (std::size_t index{}; index < Set::SlotCount; ++index) {
        const auto reaped = harness.pool->reap(
            ResponderKey,
            0U,
            operations[index],
            ERROR_OPERATION_ABORTED);
        require(static_cast<bool>(reaped) &&
                    reaped.value() == ReapDisposition::ResponseAbandoned,
                "expired overload sender did not await its exact cancellation reap");
    }
    snapshot = harness.pool->snapshot();
    require(snapshot.activeCount() == 0U &&
                snapshot.abandonedCount() == Set::SlotCount &&
                !snapshot.deadlineArmed() &&
                snapshot.lifecycleFailure() == nullptr &&
                harness.acceptSet->snapshot().pausedCount() == 0U &&
                harness.acceptSet->snapshot().admissionOpen(),
            "expired overload senders did not resume bounded listener admission");
}

void associationPinsBorrowedSocketAgainstConcurrentCancellation()
{
    auto fake = std::make_shared<FakeSocketApi>();
    fake->failShutdown(WSAENOTSOCK);
    auto associator = std::make_shared<BlockingSocketAssociator>();
    Harness harness{fake, false, associator};

    std::optional<Detail::UniqueDashboardSocket> client;
    std::exception_ptr admissionFailure;
    std::thread admissionThread{[&]() noexcept {
        try {
            client.emplace(harness.overloadOne());
        } catch (...) {
            admissionFailure = std::current_exception();
        }
    }};

    require(associator->waitUntilEntered(OperationTimeout),
            "overload association did not enter its deterministic barrier");

    std::mutex cancellationMutex;
    std::condition_variable cancellationChanged;
    bool cancellationStarted{};
    bool cancellationReturned{};
    std::thread cancellationThread{[&]() noexcept {
        {
            const std::lock_guard lock{cancellationMutex};
            cancellationStarted = true;
        }
        cancellationChanged.notify_all();
        harness.pool->beginShutdown();
        {
            const std::lock_guard lock{cancellationMutex};
            cancellationReturned = true;
        }
        cancellationChanged.notify_all();
    }};

    {
        std::unique_lock lock{cancellationMutex};
        require(cancellationChanged.wait_for(
                    lock,
                    OperationTimeout,
                    [&] { return cancellationStarted; }),
                "concurrent overload cancellation did not start");
        require(!cancellationChanged.wait_for(
                    lock,
                    100ms,
                    [&] { return cancellationReturned; }),
                "cancellation closed a borrowed socket while association was pinned");
    }

    associator->release();
    admissionThread.join();
    cancellationThread.join();
    if (admissionFailure != nullptr) {
        std::rethrow_exception(admissionFailure);
    }

    require(cancellationReturned &&
                associator->returnedWithValidSocket(),
            "overload association observed a closed or reused numeric socket handle");
    const auto pinnedSnapshot = harness.pool->snapshot();
    require(pinnedSnapshot.shutdownRequested() &&
                fake->shutdownCalls() == 1U,
            "association-pinned cancellation did not resume after association returned");

    if (harness.pool->snapshot().activeCount() != 0U) {
        require(fake->sendCallCount() == 1U &&
                    harness.pool->snapshot().cancellationRequestedCount() ==
                        1U,
                "post-association cancellation lost its exact native send");
        const auto send = fake->sendCall(0U);
        const auto reaped = harness.pool->reap(
            ResponderKey,
            0U,
            send.operation,
            ERROR_OPERATION_ABORTED);
        require(static_cast<bool>(reaped),
                "post-association cancellation did not accept its exact reap");
    } else {
        require(fake->sendCallCount() == 0U,
                "association-time cancellation released an issued send early");
    }
    require(harness.pool->snapshot().fullyDrained() &&
                harness.acceptSet->snapshot().awaitingReturnCount() == 0U &&
                harness.failFast->calls() == 0U,
            "association-pinned cancellation did not drain without fail-fast");
}

enum class NativeCancellationFailureMode : std::uint8_t {
    CancelOnly,
    ShutdownOnly,
    CancelAndShutdown,
};

void nativeCancellationFailureRetainsUntilBoundedReapWatchdog(
    const NativeCancellationFailureMode mode)
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);

    if (mode != NativeCancellationFailureMode::ShutdownOnly) {
        fake->failCancel(ERROR_ACCESS_DENIED);
    }
    if (mode != NativeCancellationFailureMode::CancelOnly) {
        fake->failShutdown(WSAENOTSOCK);
    }

    harness.clock->advance(std::chrono::duration_cast<std::chrono::milliseconds>(
        Pool::ResponseLifetime));
    require(harness.deadlineSink->waitForSignals(1U, OperationTimeout),
            "native cancellation failure did not reach response expiry");

    auto snapshot = harness.pool->snapshot();
    require(snapshot.shutdownRequested() &&
                snapshot.activeCount() == 1U &&
                snapshot.cancellationRequestedCount() == 1U &&
                snapshot.deadlineArmed() &&
                snapshot.expiredCount() == 1U &&
                snapshot.failFastCount() == 0U,
            "native cancellation failure released ownership or omitted its reap watchdog");
    require(snapshot.lifecycleFailure() != nullptr &&
                snapshot.lifecycleFailure()->kind !=
                    Detail::DashboardOverloadResponderFailureKind::Cancelled,
            "native cancellation failure retained no terminal diagnostic");
    require(harness.failFast->calls() == 0U &&
                fake->cancelCalls() == 1U &&
                fake->shutdownCalls() == 1U &&
                fake->sendCallCount() == 1U,
            "native cancellation failure duplicated work before its watchdog");
    require(!harness.acceptSet->snapshot().admissionOpen() &&
                harness.acceptSet->snapshot().awaitingReturnCount() == 1U,
            "native cancellation failure did not close its exact origin while retaining the token");
    require(harness.drainObserver->terminalNotificationCountFor(
                GenerationId) == 1U,
            "native cancellation failure omitted its exact terminal generation edge");
    require(harness.drainObserver->terminalPendingCountFor(GenerationId) ==
                1U,
            "native cancellation failure omitted its pre-drain collection latch");

    harness.clock->advance(std::chrono::duration_cast<std::chrono::milliseconds>(
        Pool::CancellationReapLifetime));
    require(harness.deadlineSink->waitForSignals(2U, OperationTimeout),
            "native cancellation reap watchdog was not delivered");

    snapshot = harness.pool->snapshot();
    require(snapshot.shutdownRequested() &&
                snapshot.activeCount() == 1U &&
                snapshot.cancellationRequestedCount() == 1U &&
                !snapshot.deadlineArmed() &&
                snapshot.failFastCount() == 1U &&
                harness.failFast->calls() == 1U,
            "native cancellation reap watchdog did not fail fast with exact ownership retained");
    require(fake->sendCallCount() == 1U &&
                fake->cancelCalls() == 1U &&
                fake->shutdownCalls() == 1U,
            "native cancellation reap watchdog reissued or duplicated native cancellation");
    require(harness.drainObserver->terminalNotificationCountFor(
                GenerationId) == 1U,
            "native cancellation watchdog duplicated its terminal generation edge");

    const auto exactReap = harness.pool->reap(
        ResponderKey,
        0U,
        send.operation,
        ERROR_OPERATION_ABORTED);
    require(static_cast<bool>(exactReap) &&
                exactReap.value() == ReapDisposition::ResponseAbandoned &&
                harness.pool->snapshot().fullyDrained() &&
                harness.acceptSet->snapshot().awaitingReturnCount() == 0U,
            "late exact cancellation reap did not safely release retained ownership");
}

void nativeCancellationFailuresAreBoundedByReapWatchdog()
{
    nativeCancellationFailureRetainsUntilBoundedReapWatchdog(
        NativeCancellationFailureMode::CancelOnly);
    nativeCancellationFailureRetainsUntilBoundedReapWatchdog(
        NativeCancellationFailureMode::ShutdownOnly);
    nativeCancellationFailureRetainsUntilBoundedReapWatchdog(
        NativeCancellationFailureMode::CancelAndShutdown);
}

void unsolicitedOperationAbortIsTerminalIntegrityFailure()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    const auto reaped = harness.pool->reap(
        ResponderKey,
        0U,
        send.operation,
        ERROR_OPERATION_ABORTED);
    requireError(
        reaped,
        Domain::ErrorCodes::IntegrityFailure,
        "an unsolicited overload abort was treated as owner cancellation");
    require(harness.drainObserver->terminalPendingCountFor(GenerationId) ==
                1U &&
                harness.drainObserver->terminalNotificationCount() == 0U &&
                harness.drainObserver->notificationCountFor(GenerationId) ==
                    0U,
            "an ordinary drain edge escaped its terminal collection latch");
    harness.pool->drainTerminalGenerationNotifications();
    require(harness.drainObserver->terminalNotificationCountFor(
                GenerationId) == 1U &&
                harness.drainObserver->notificationCountFor(GenerationId) ==
                    1U,
            "the exact terminal generation edge did not causally precede its drain recheck");
    harness.pool->drainTerminalGenerationNotifications();
    require(harness.drainObserver->terminalNotificationCountFor(
                GenerationId) == 1U,
            "the exact terminal generation edge was delivered more than once");
    const auto snapshot = harness.pool->snapshot();
    require(snapshot.shutdownRequested() && snapshot.fullyDrained() &&
                snapshot.lifecycleFailure() != nullptr &&
                snapshot.lifecycleFailure()->kind ==
                    Detail::DashboardOverloadResponderFailureKind::
                        IntegrityFailure,
            "an unsolicited overload abort did not fail the fixed owner closed");
    require(!harness.acceptSet->snapshot().admissionOpen() &&
                harness.acceptSet->snapshot().pausedCount() == 0U &&
                harness.acceptSet->snapshot().awaitingReturnCount() == 0U,
            "an unsolicited overload abort reissued admission on a corrupted owner");
}

void unsolicitedInitialIssueAbortIsTerminalIntegrityFailure()
{
    auto fake = std::make_shared<FakeSocketApi>();
    fake->failSend(WSA_OPERATION_ABORTED);
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto snapshot = harness.pool->snapshot();
    require(snapshot.shutdownRequested() && snapshot.fullyDrained() &&
                snapshot.lifecycleFailure() != nullptr &&
                snapshot.lifecycleFailure()->kind ==
                    Detail::DashboardOverloadResponderFailureKind::
                        IntegrityFailure,
            "an unsolicited initial issue abort did not fail the owner closed");
    require(!harness.acceptSet->snapshot().admissionOpen() &&
                harness.acceptSet->snapshot().pausedCount() == 0U &&
                harness.acceptSet->snapshot().awaitingReturnCount() == 0U,
            "an unsolicited initial issue abort reissued listener admission");
}

void unsolicitedPartialReissueAbortIsTerminalIntegrityFailure()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto first = fake->sendCall(0U);
    fake->failSend(WSA_OPERATION_ABORTED);
    const auto reaped = harness.pool->reap(
        ResponderKey,
        1U,
        first.operation,
        ERROR_SUCCESS);
    requireError(
        reaped,
        Domain::ErrorCodes::IntegrityFailure,
        "an unsolicited partial reissue abort was treated as client close");
    const auto snapshot = harness.pool->snapshot();
    require(fake->sendCallCount() == 2U &&
                snapshot.shutdownRequested() && snapshot.fullyDrained() &&
                snapshot.lifecycleFailure() != nullptr &&
                snapshot.lifecycleFailure()->kind ==
                    Detail::DashboardOverloadResponderFailureKind::
                        IntegrityFailure,
            "an unsolicited partial reissue abort did not fail the owner closed");
    require(!harness.acceptSet->snapshot().admissionOpen() &&
                harness.acceptSet->snapshot().pausedCount() == 0U,
            "an unsolicited partial reissue abort reissued listener admission");
}

void foreignCompletionTriggersTerminalCancellationWithoutDetaching()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    OVERLAPPED foreign{};
    harness.pool->consume(
        Detail::DashboardIoCompletionPacket{
            1U, ResponderKey, std::addressof(foreign)},
        ERROR_SUCCESS);
    const auto failed = harness.pool->snapshot();
    require(failed.shutdownRequested() && failed.activeCount() == 1U &&
                failed.cancellationRequestedCount() == 1U,
            "foreign completion detached or ignored the live exact operation");
    require(failed.lifecycleFailure() != nullptr &&
                failed.lifecycleFailure()->kind ==
                    Detail::DashboardOverloadResponderFailureKind::
                        IntegrityFailure,
            "foreign completion did not retain an integrity failure");
    const auto exact = harness.pool->reap(
        ResponderKey,
        0U,
        send.operation,
        ERROR_OPERATION_ABORTED);
    require(static_cast<bool>(exact) &&
                harness.pool->snapshot().fullyDrained(),
            "foreign completion shutdown did not retain for exact reap");
}

void emptyPoolTerminalFailuresPublishProcessOwnerEdge()
{
    const auto runCompletionCase = [](const bool nullOperation) {
        auto fake = std::make_shared<FakeSocketApi>();
        Harness harness{fake};
        OVERLAPPED foreign{};
        harness.pool->consume(
            Detail::DashboardIoCompletionPacket{
                0U,
                ResponderKey,
                nullOperation ? nullptr : std::addressof(foreign)},
            ERROR_INVALID_DATA);
        const auto snapshot = harness.pool->snapshot();
        require(snapshot.shutdownRequested() && snapshot.fullyDrained() &&
                    snapshot.lifecycleFailure() != nullptr &&
                    snapshot.lifecycleFailure()->kind ==
                        Detail::DashboardOverloadResponderFailureKind::
                            IntegrityFailure,
                "an empty-pool malformed completion did not make the fixed owner terminal");
        require(harness.drainObserver->ownerTerminalCount() == 1U &&
                    harness.drainObserver->terminalNotificationCount() == 0U,
                "an empty-pool malformed completion omitted or misattributed its process-owner terminal edge");
    };

    runCompletionCase(true);
    runCompletionCase(false);

    auto fake = std::make_shared<FakeSocketApi>();
    Harness fatalHarness{fake};
    fatalHarness.pool->fatal(ERROR_GEN_FAILURE);
    const auto fatalSnapshot = fatalHarness.pool->snapshot();
    require(fatalSnapshot.shutdownRequested() &&
                fatalSnapshot.fullyDrained() &&
                fatalSnapshot.lifecycleFailure() != nullptr &&
                fatalHarness.drainObserver->ownerTerminalCount() == 1U &&
                fatalHarness.drainObserver->terminalNotificationCount() ==
                    0U,
            "an empty-pool IOCP fatal omitted its process-owner terminal edge");
}

void fatalKernelFailureClosesOriginAndAwaitsExactReap()
{
    auto fake = std::make_shared<FakeSocketApi>();
    Harness harness{fake};
    auto client = harness.overloadOne();
    const auto send = fake->sendCall(0U);
    harness.pool->fatal(ERROR_INVALID_HANDLE);
    const auto failed = harness.pool->snapshot();
    require(failed.shutdownRequested() &&
                failed.cancellationRequestedCount() == 1U &&
                !failed.fullyDrained(),
            "fatal IOCP failure released a live send before reap");
    require(failed.lifecycleFailure() != nullptr &&
                failed.lifecycleFailure()->kind ==
                    Detail::DashboardOverloadResponderFailureKind::
                        InternalFailure,
            "fatal IOCP failure did not retain bounded diagnostics");
    static_cast<void>(harness.pool->reap(
        ResponderKey,
        0U,
        send.operation,
        ERROR_OPERATION_ABORTED));
    require(harness.pool->snapshot().fullyDrained(),
            "fatal IOCP failure did not drain after exact cancellation reap");
}

void realWinsockIocpRouteDeliversTheImmutable503()
{
    Harness harness;
    auto client = harness.overloadOne();
    const auto received = receiveExact(
        client.get(), harness.responseBytes().size());
    require(received == harness.responseBytes(),
            "real Winsock overload route changed the immutable fixed 503");

    const auto deadline = std::chrono::steady_clock::now() + OperationTimeout;
    while (harness.pool->snapshot().activeCount() != 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    const auto snapshot = harness.pool->snapshot();
    require(snapshot.activeCount() == 0U &&
                snapshot.deliveredCount() == 1U &&
                snapshot.lifecycleFailure() == nullptr,
            "real Winsock/IOCP 503 did not complete and drain cleanly");
    require(harness.router->snapshot().fixedDispatchCount() >= 1U,
            "real overload completion bypassed the process fixed-key router");
    require(harness.fallback->healthy(),
            "real Winsock/IOCP route overflowed or failed its fallback sink");
}

} // namespace

int main()
{
    try {
        invalidFixedOwnerIsRejected();
        partialSendRetainsAndReissuesTheExactSuffix();
        routerDispatchesTheOwnedCompletionKey();
        peerFailureAbandonsOnlyTheExactResponder();
        immediateSendFailureClosesAndReturnsWithoutNativeObligation();
        activeAndRetiringGenerationsFillEightBoundedResponders();
        shutdownCompletionDoesNotReissueAPartialResponse();
        concurrentShutdownWaitsForCoordinatorPublication();
        sharedDeadlineExpiresFourStalledActiveResponses();
        associationPinsBorrowedSocketAgainstConcurrentCancellation();
        nativeCancellationFailuresAreBoundedByReapWatchdog();
        unsolicitedOperationAbortIsTerminalIntegrityFailure();
        unsolicitedInitialIssueAbortIsTerminalIntegrityFailure();
        unsolicitedPartialReissueAbortIsTerminalIntegrityFailure();
        foreignCompletionTriggersTerminalCancellationWithoutDetaching();
        emptyPoolTerminalFailuresPublishProcessOwnerEdge();
        fatalKernelFailureClosesOriginAndAwaitsExactReap();
        realWinsockIocpRouteDeliversTheImmutable503();
        std::cout
            << "Dashboard overload-responder tests passed with "
            << assertionCount << " assertions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "Dashboard overload-responder tests failed after "
            << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    }
}
