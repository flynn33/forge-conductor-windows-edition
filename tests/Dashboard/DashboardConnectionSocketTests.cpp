#include "Infrastructure/Windows/Detail/DashboardConnectionSocket.h"

#include "Infrastructure/Windows/Detail/DashboardAcceptSlotSet.h"
#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"
#include "Infrastructure/Windows/Detail/DashboardListeningSocket.h"
#include "Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.h"
#include "Infrastructure/Windows/Detail/DashboardWinsockRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using AcceptedConnection = Detail::DashboardAcceptedConnection;
using CancellationDisposition =
    Detail::DashboardConnectionSocketCancellationDisposition;
using CompletionKey = Detail::DashboardIoCompletionKey;
using ConnectionSocket = Detail::DashboardConnectionSocket;
using ConnectionState = Detail::DashboardConnectionSocketState;
using Endpoint = Detail::DashboardLoopbackEndpoint;
using IssueDisposition =
    Detail::DashboardConnectionSocketIssueDisposition;
using Kernel = Detail::DashboardIocpWorkerKernel;
using KernelSink = Detail::IDashboardIocpCompletionSink;
using Listener = Detail::DashboardListeningSocket;
using NativeApi = Detail::IDashboardConnectionSocketApi;
using NativeSystemApi = Detail::DashboardConnectionSocketSystemApi;
using OperationKind = Detail::DashboardConnectionSocketOperationKind;
using Port = Detail::DashboardIoCompletionPort;
using Runtime = Detail::DashboardWinsockRuntime;
using Set = Detail::DashboardAcceptSlotSet;
using ShutdownDisposition =
    Detail::DashboardConnectionSocketShutdownDisposition;

using namespace std::chrono_literals;

constexpr CompletionKey ListenerKey{0x4110U};
constexpr CompletionKey ConnectionKey{0x5110U};
constexpr auto NetworkOperationTimeout = 5s;

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
    const bool retryable,
    const std::string_view context)
{
    require(!result, context);
    require(result.error().code == code, "failure used the wrong stable code");
    require(
        result.error().retryable == retryable,
        "failure used the wrong retryability");
}

[[nodiscard]] std::chrono::milliseconds remainingNetworkTime(
    const std::chrono::steady_clock::time_point deadline,
    const std::string_view timeoutMessage)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        fail(timeoutMessage);
    }
    return (std::max)(
        1ms,
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
}

[[nodiscard]] SHORT waitForSocketEvent(
    const SOCKET socket,
    const SHORT events,
    const std::chrono::steady_clock::time_point deadline,
    const std::string_view timeoutMessage,
    const std::string_view failureMessage)
{
    for (;;) {
        WSAPOLLFD descriptor{};
        descriptor.fd = socket;
        descriptor.events = events;
        const auto remaining = remainingNetworkTime(deadline, timeoutMessage);
        require(
            remaining.count() <=
                static_cast<std::int64_t>(
                    (std::numeric_limits<int>::max)()),
            "network poll timeout exceeded the native integer bound");
        const int status = ::WSAPoll(
            &descriptor, 1U, static_cast<int>(remaining.count()));
        if (status > 0) {
            return descriptor.revents;
        }
        if (status == 0) {
            fail(timeoutMessage);
        }
        if (::WSAGetLastError() != WSAEINTR) {
            fail(failureMessage);
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
        "real client could not enter nonblocking mode");

    if (::connect(socket, address, addressLength) == 0) {
        return;
    }
    const int connectError = ::WSAGetLastError();
    require(
        connectError == WSAEWOULDBLOCK ||
            connectError == WSAEINPROGRESS ||
            connectError == WSAEALREADY,
        "real client connect failed before its bounded wait");

    const auto deadline =
        std::chrono::steady_clock::now() + NetworkOperationTimeout;
    const SHORT revents = waitForSocketEvent(
        socket,
        POLLOUT,
        deadline,
        "real client connect exceeded its fixed deadline",
        "real client connect poll failed");
    require((revents & POLLNVAL) == 0,
            "real client connect polled an invalid socket");

    int finalError{};
    int finalErrorLength = static_cast<int>(sizeof(finalError));
    require(
        ::getsockopt(
            socket,
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&finalError),
            &finalErrorLength) == 0,
        "real client could not inspect bounded connect completion");
    require(finalError == 0,
            "real client bounded connect completed with a socket error");
}

void sendExactWithDeadline(
    const SOCKET socket,
    const std::span<const std::byte> bytes)
{
    u_long nonblocking = 1U;
    require(
        ::ioctlsocket(socket, FIONBIO, &nonblocking) == 0,
        "bounded peer send could not enter nonblocking mode");
    const auto deadline =
        std::chrono::steady_clock::now() + NetworkOperationTimeout;
    std::size_t offset{};
    while (offset < bytes.size()) {
        static_cast<void>(remainingNetworkTime(
            deadline, "peer send exceeded its fixed deadline"));
        const std::size_t remaining = bytes.size() - offset;
        require(
            remaining <= static_cast<std::size_t>(
                (std::numeric_limits<int>::max)()),
            "bounded peer send exceeded the native integer bound");
        const int sent = ::send(
            socket,
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<int>(remaining),
            0);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        require(sent == SOCKET_ERROR,
                "bounded peer send returned zero bytes");
        const int code = ::WSAGetLastError();
        if (code == WSAEINTR) {
            continue;
        }
        require(code == WSAEWOULDBLOCK,
                "bounded peer send failed with a socket error");
        const SHORT revents = waitForSocketEvent(
            socket,
            POLLOUT,
            deadline,
            "peer send exceeded its fixed deadline",
            "peer send poll failed");
        require((revents & POLLNVAL) == 0,
                "peer send polled an invalid socket");
    }
}

[[nodiscard]] bool bytesAreZero(
    const void* const storage,
    const std::size_t length) noexcept
{
    const auto* const bytes = static_cast<const unsigned char*>(storage);
    return std::all_of(
        bytes,
        bytes + length,
        [](const unsigned char value) noexcept { return value == 0U; });
}

static_assert(std::is_abstract_v<NativeApi>);
static_assert(std::is_final_v<NativeSystemApi>);
static_assert(std::is_final_v<ConnectionSocket>);
static_assert(!std::is_copy_constructible_v<ConnectionSocket>);
static_assert(!std::is_move_constructible_v<ConnectionSocket>);
static_assert(std::is_nothrow_destructible_v<ConnectionSocket>);
static_assert(ConnectionSocket::ReceiveBufferLength == 16U * 1024U);
static_assert(noexcept(std::declval<ConnectionSocket&>().issueReceive()));
static_assert(noexcept(std::declval<ConnectionSocket&>().issueSend(
    std::declval<std::span<const std::byte>>())));
static_assert(noexcept(
    std::declval<ConnectionSocket&>().requestCancellation()));
static_assert(noexcept(std::declval<ConnectionSocket&>().shutdownBoth()));

class CollectingKernelSink final : public KernelSink {
public:
    struct Completion final {
        Detail::DashboardIoCompletionPacket packet;
        DWORD nativeError{};
    };

    void consume(
        const Detail::DashboardIoCompletionPacket packet,
        const DWORD nativeError) noexcept override
    {
        try {
            const std::lock_guard lock{mutex_};
            if (count_ == completions_.size()) {
                overflow_ = true;
                return;
            }
            const std::size_t tail = (head_ + count_) % completions_.size();
            completions_[tail] = Completion{packet, nativeError};
            ++count_;
            changed_.notify_one();
        } catch (...) {
            std::terminate();
        }
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        try {
            const std::lock_guard lock{mutex_};
            fatalError_ = nativeError;
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
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
        completion = completions_[head_];
        head_ = (head_ + 1U) % completions_.size();
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
    std::array<Completion, 64U> completions_{};
    std::size_t head_{};
    std::size_t count_{};
    bool overflow_{};
    std::optional<DWORD> fatalError_;
};

class ConnectionPair final {
public:
    ConnectionPair(
        AcceptedConnection acceptedValue,
        Detail::UniqueDashboardSocket clientValue) noexcept
        : accepted{std::move(acceptedValue)}, client{std::move(clientValue)}
    {
    }

    ConnectionPair(const ConnectionPair&) = delete;
    ConnectionPair& operator=(const ConnectionPair&) = delete;
    ConnectionPair(ConnectionPair&&) noexcept = default;
    ConnectionPair& operator=(ConnectionPair&&) = delete;

    AcceptedConnection accepted;
    Detail::UniqueDashboardSocket client;
};

class RealHarness final {
public:
    RealHarness(
        const std::string_view host,
        const int addressFamily,
        const std::uint16_t firstPort)
        : addressFamily_{addressFamily},
          runtime{take(Runtime::create())},
          sink{std::make_shared<CollectingKernelSink>()},
          kernel{take(Kernel::create(take(Port::create()), sink))}
    {
        std::optional<Listener> listener;
        for (std::uint16_t offset{}; offset < 32U; ++offset) {
            const auto candidatePort = static_cast<std::uint16_t>(
                firstPort + offset);
            auto candidateEndpoint = Endpoint::create(host, candidatePort);
            if (!candidateEndpoint) {
                continue;
            }
            auto candidateListener = Listener::create(
                *runtime, candidateEndpoint.value());
            if (candidateListener) {
                endpoint.emplace(std::move(candidateEndpoint).value());
                listener.emplace(std::move(candidateListener).value());
                break;
            }
        }
        if (!endpoint.has_value() || !listener.has_value()) {
            fail("bounded connection-socket port list had no free listener");
        }
        set = take(Set::create(
            *runtime, std::move(*listener), ListenerKey));
        auto started = set->start(*kernel);
        if (!started) {
            fail(started.error().code + ": " + started.error().message);
        }
    }

    ~RealHarness() noexcept
    {
        try {
            retireListener(false, false);
            if (kernel != nullptr) {
                kernel->shutdown();
                kernel.reset();
            }
            runtime.reset();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] ConnectionPair acceptOne()
    {
        require(runtime != nullptr, "accept requested after runtime facade reset");
        require(set != nullptr, "accept requested after listener retirement");
        auto client = take(
            runtime->createOverlappedTcpSocket(addressFamily_));
        connectWithDeadline(
            client.get(),
            endpoint->nativeAddress(),
            endpoint->nativeAddressLength());

        CollectingKernelSink::Completion completion{};
        require(
            sink->waitTake(completion, 5s),
            "AcceptEx completion did not arrive");
        require(
            completion.packet.completionKey == ListenerKey,
            "accepted socket used the wrong listener key");
        require(
            completion.nativeError == ERROR_SUCCESS,
            "AcceptEx completed with a native error");
        auto reaped = take(set->reap(
            completion.packet.completionKey,
            completion.packet.transferredBytes,
            completion.packet.operation,
            completion.nativeError));
        require(
            reaped.disposition() ==
                Detail::DashboardAcceptReapDisposition::AcceptedAndPaused,
            "real accept did not pause its fixed slot");
        auto accepted = reaped.takeAcceptedConnection();
        require(accepted.has_value(), "real accept omitted its connection");
        auto token = reaped.takeResumeToken();
        require(token.has_value(), "real accept omitted its resume token");
        auto resumed = set->resume(std::move(*token));
        require(static_cast<bool>(resumed), "real accept slot did not resume");
        return ConnectionPair{
            std::move(*accepted), std::move(client)};
    }

    void retireListener(
        const bool stopKernel,
        const bool resetRuntimeFacade)
    {
        if (set != nullptr) {
            const auto cancellationFailure =
                set->closeAdmissionAndRequestCancellation();
            require(
                !cancellationFailure.has_value(),
                "listener cancellation returned a retained failure");
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!set->snapshot().fullyDrained() &&
                   std::chrono::steady_clock::now() < deadline) {
                CollectingKernelSink::Completion completion{};
                if (!sink->waitTake(completion, 250ms)) {
                    continue;
                }
                auto reaped = set->reap(
                    completion.packet.completionKey,
                    completion.packet.transferredBytes,
                    completion.packet.operation,
                    completion.nativeError);
                require(
                    static_cast<bool>(reaped),
                    "listener retirement could not reap a completion");
            }
            require(
                set->snapshot().fullyDrained(),
                "listener retirement did not drain four slots");
            set.reset();
        }
        if (stopKernel && kernel != nullptr) {
            kernel->shutdown();
        }
        if (resetRuntimeFacade) {
            runtime.reset();
        }
    }

    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<CollectingKernelSink> sink;
    std::unique_ptr<Kernel> kernel;
    std::optional<Endpoint> endpoint;
    std::unique_ptr<Set> set;

private:
    int addressFamily_{};
};

class FakeNativeApi final : public NativeApi {
public:
    [[nodiscard]] int receive(
        const SOCKET socket,
        WSABUF* const buffers,
        const DWORD bufferCount,
        DWORD& transferredBytes,
        DWORD& flags,
        OVERLAPPED* const operation) noexcept override
    {
        ++receiveCalls;
        lastSocket = socket;
        lastBuffers = buffers;
        lastBufferCount = bufferCount;
        lastOperation = operation;
        lastFlagsPointer = &flags;
        transferredBytes = immediateTransferredBytes;
        if (observer != nullptr) {
            observedPublishedState = observer->state();
            observedStableOperation =
                operation == observer->borrowedOperation();
        }
        return receiveStatus;
    }

    [[nodiscard]] int send(
        const SOCKET socket,
        WSABUF* const buffers,
        const DWORD bufferCount,
        DWORD& transferredBytes,
        const DWORD flags,
        OVERLAPPED* const operation) noexcept override
    {
        ++sendCalls;
        lastSocket = socket;
        lastBuffers = buffers;
        lastBufferCount = bufferCount;
        lastOperation = operation;
        lastSendFlags = flags;
        transferredBytes = immediateTransferredBytes;
        if (observer != nullptr) {
            observedPublishedState = observer->state();
            observedStableOperation =
                operation == observer->borrowedOperation();
        }
        return sendStatus;
    }

    [[nodiscard]] BOOL cancel(
        const SOCKET socket,
        OVERLAPPED* const operation) noexcept override
    {
        ++cancelCalls;
        lastSocket = socket;
        lastOperation = operation;
        if (blockCancellation) {
            try {
                std::unique_lock lock{cancellationMutex_};
                cancellationEntered_ = true;
                cancellationChanged_.notify_all();
                cancellationChanged_.wait(
                    lock, [this] { return cancellationReleased_; });
            } catch (...) {
                std::terminate();
            }
        }
        return cancelResult;
    }

    [[nodiscard]] int shutdownSocket(
        const SOCKET socket,
        const int how) noexcept override
    {
        ++shutdownCalls;
        lastSocket = socket;
        lastShutdownHow = how;
        return shutdownStatus;
    }

    [[nodiscard]] int lastSocketError() noexcept override
    {
        ++lastSocketErrorCalls;
        return socketError;
    }

    [[nodiscard]] DWORD lastSystemError() noexcept override
    {
        ++lastSystemErrorCalls;
        return systemError;
    }

    void beginBlockingCancellation() noexcept
    {
        const std::lock_guard lock{cancellationMutex_};
        blockCancellation = true;
        cancellationEntered_ = false;
        cancellationReleased_ = false;
    }

    [[nodiscard]] bool waitForCancellationEntry()
    {
        std::unique_lock lock{cancellationMutex_};
        return cancellationChanged_.wait_for(
            lock, 5s, [this] { return cancellationEntered_; });
    }

    void releaseCancellation() noexcept
    {
        try {
            {
                const std::lock_guard lock{cancellationMutex_};
                cancellationReleased_ = true;
                blockCancellation = false;
            }
            cancellationChanged_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    ConnectionSocket* observer{};
    WSABUF* lastBuffers{};
    DWORD* lastFlagsPointer{};
    OVERLAPPED* lastOperation{};
    SOCKET lastSocket{INVALID_SOCKET};
    DWORD lastBufferCount{};
    DWORD lastSendFlags{};
    int lastShutdownHow{};
    ConnectionState observedPublishedState{ConnectionState::Idle};
    bool observedStableOperation{};
    std::size_t receiveCalls{};
    std::size_t sendCalls{};
    std::size_t cancelCalls{};
    std::size_t shutdownCalls{};
    std::size_t lastSocketErrorCalls{};
    std::size_t lastSystemErrorCalls{};
    int receiveStatus{};
    int sendStatus{};
    int shutdownStatus{};
    int socketError{WSA_IO_PENDING};
    DWORD systemError{ERROR_OPERATION_ABORTED};
    DWORD immediateTransferredBytes{};
    BOOL cancelResult{TRUE};

private:
    std::mutex cancellationMutex_;
    std::condition_variable cancellationChanged_;
    bool blockCancellation{};
    bool cancellationEntered_{};
    bool cancellationReleased_{};
};

[[nodiscard]] std::unique_ptr<ConnectionSocket> owner(
    ConnectionPair& pair,
    RealHarness& harness,
    const std::shared_ptr<FakeNativeApi>& api)
{
    const SOCKET acceptedSocket = pair.accepted.borrowedNativeSocket();
    auto created = ConnectionSocket::create(
        std::move(pair.accepted),
        *harness.kernel,
        ConnectionKey,
        api);
    require(created.hasValue(), "connection socket creation failed");
    auto value = std::move(created).value();
    require(
        value->borrowedNativeSocket() == acceptedSocket,
        "connection socket changed accepted ownership");
    require(value->completionKey() == ConnectionKey, "connection key changed");
    api->observer = value.get();
    return value;
}

[[nodiscard]] std::uint16_t portBase(
    const std::uint16_t familyBase) noexcept
{
    static std::atomic_uint32_t sequence{};
    const auto offset = sequence.fetch_add(37U, std::memory_order_relaxed);
    return static_cast<std::uint16_t>(
        familyBase + (::GetCurrentProcessId() % 400U) + (offset % 400U));
}

void immediatePendingMutualExclusionAndPartialSend()
{
    RealHarness harness{"127.0.0.1", AF_INET, portBase(50000U)};
    auto pair = harness.acceptOne();
    const auto api = std::make_shared<FakeNativeApi>();
    auto socket = owner(pair, harness, api);

    api->receiveStatus = 0;
    api->immediateTransferredBytes = 9'999U;
    auto receive = socket->issueReceive();
    require(
        static_cast<bool>(receive) &&
            receive.value() == IssueDisposition::CompletedSynchronously,
        "immediate receive did not report synchronous native success");
    require(
        socket->state() == ConnectionState::ReceiveIssued,
        "synchronous receive was treated as already completed");
    require(
        api->observedPublishedState == ConnectionState::ReceiveIssued &&
            api->observedStableOperation,
        "receive state/storage was not published before the native call");
    require(
        api->lastBuffers != nullptr && api->lastBufferCount == 1U &&
            api->lastBuffers->len == ConnectionSocket::ReceiveBufferLength &&
            api->lastFlagsPointer != nullptr &&
            *api->lastFlagsPointer == 0U,
        "receive did not retain its exact fixed buffer and flags");
    require(socket->receivedBytes().empty(),
            "immediate native count was exposed before IOCP reap");
    const std::array<std::byte, 2U> blockedSend{};
    requireError(
        socket->issueSend(blockedSend),
        Domain::ErrorCodes::Conflict,
        false,
        "send overlapped an outstanding receive");

    constexpr std::array<std::byte, 5U> Payload{
        std::byte{'h'},
        std::byte{'e'},
        std::byte{'l'},
        std::byte{'l'},
        std::byte{'o'}};
    std::memcpy(api->lastBuffers->buf, Payload.data(), Payload.size());
    auto received = socket->reap(
        ConnectionKey,
        static_cast<DWORD>(Payload.size()),
        socket->borrowedOperation(),
        ERROR_SUCCESS);
    require(static_cast<bool>(received), "successful receive did not reap");
    require(
        received.value().operationKind() == OperationKind::Receive &&
            received.value().transferredBytes() == Payload.size(),
        "receive reap returned wrong operation metadata");
    const auto view = socket->receivedBytes();
    require(view.size() == Payload.size(), "receive view used wrong length");
    require(
        std::equal(view.begin(), view.end(), Payload.begin()),
        "receive view did not expose exact owned bytes");
    require(
        socket->snapshot().receivedByteCount() == Payload.size(),
        "receive snapshot omitted completed bytes");

    std::array<std::byte, 6U> sendBytes{
        std::byte{1},
        std::byte{2},
        std::byte{3},
        std::byte{4},
        std::byte{5},
        std::byte{6}};
    api->sendStatus = SOCKET_ERROR;
    api->socketError = WSA_IO_PENDING;
    api->immediateTransferredBytes = 4'444U;
    auto send = socket->issueSend(sendBytes);
    require(
        static_cast<bool>(send) && send.value() == IssueDisposition::Pending,
        "pending send did not retain its operation");
    require(socket->receivedBytes().empty(),
            "new send issue did not clear the prior receive view");
    require(
        socket->state() == ConnectionState::SendIssued &&
            api->observedPublishedState == ConnectionState::SendIssued &&
            api->observedStableOperation,
        "send state/storage was not published before the native call");
    require(
        api->lastBuffers->buf == reinterpret_cast<char*>(sendBytes.data()) &&
            api->lastBuffers->len == sendBytes.size() &&
            api->lastSendFlags == 0U,
        "send did not retain the exact borrowed span");
    requireError(
        socket->issueReceive(),
        Domain::ErrorCodes::Conflict,
        false,
        "receive overlapped an outstanding send");

    auto partial = socket->reap(
        ConnectionKey,
        2U,
        socket->borrowedOperation(),
        ERROR_SUCCESS);
    require(static_cast<bool>(partial), "partial send did not reap");
    require(
        partial.value().operationKind() == OperationKind::Send &&
            partial.value().transferredBytes() == 2U,
        "partial send count was not returned to the caller state machine");
    require(socket->state() == ConnectionState::Idle,
            "partial send did not restore idle state");

    api->sendStatus = 0;
    api->immediateTransferredBytes = 3'333U;
    auto synchronousSend = socket->issueSend(sendBytes);
    require(
        static_cast<bool>(synchronousSend) &&
            synchronousSend.value() ==
                IssueDisposition::CompletedSynchronously,
        "immediate send did not report synchronous native success");
    require(
        socket->state() == ConnectionState::SendIssued &&
            socket->snapshot().activeBufferLength() == sendBytes.size(),
        "synchronous send released borrowed storage before IOCP reap");
    requireError(
        socket->issueReceive(),
        Domain::ErrorCodes::Conflict,
        false,
        "receive overlapped a synchronously completed native send");
    auto synchronousSendReap = socket->reap(
        ConnectionKey,
        static_cast<DWORD>(sendBytes.size()),
        socket->borrowedOperation(),
        ERROR_SUCCESS);
    require(
        static_cast<bool>(synchronousSendReap) &&
            synchronousSendReap.value().operationKind() ==
                OperationKind::Send &&
            synchronousSendReap.value().transferredBytes() ==
                sendBytes.size(),
        "synchronous send used its immediate count instead of IOCP reap");
    require(socket->state() == ConnectionState::Idle,
            "synchronous send reap did not restore idle state");

    api->receiveStatus = SOCKET_ERROR;
    api->socketError = WSA_IO_PENDING;
    auto eofIssue = socket->issueReceive();
    require(
        static_cast<bool>(eofIssue) &&
            eofIssue.value() == IssueDisposition::Pending,
        "EOF receive setup did not remain pending");
    auto eof = socket->reap(
        ConnectionKey, 0U, socket->borrowedOperation(), ERROR_SUCCESS);
    requireError(
        eof,
        Domain::ErrorCodes::TransportClosed,
        false,
        "zero-byte receive was not peer EOF");
    require(socket->state() == ConnectionState::Idle,
            "EOF reap did not consume the exact operation");
}

void malformedIssuesCancellationAndShutdownAreTyped()
{
    RealHarness harness{"127.0.0.1", AF_INET, portBase(50500U)};
    auto pair = harness.acceptOne();
    const auto api = std::make_shared<FakeNativeApi>();
    auto socket = owner(pair, harness, api);

    api->receiveStatus = 1;
    api->socketError = WSA_IO_PENDING;
    auto malformedReceiveStatus = socket->issueReceive();
    requireError(
        malformedReceiveStatus,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "undefined native receive status was accepted as pending");
    require(socket->state() == ConnectionState::Idle,
            "undefined native receive status retained issued storage");

    api->receiveStatus = SOCKET_ERROR;
    api->socketError = WSAENOBUFS;
    auto failedReceive = socket->issueReceive();
    requireError(
        failedReceive,
        Domain::ErrorCodes::LimitExceeded,
        true,
        "immediate receive issue failure was accepted");
    require(
        socket->state() == ConnectionState::Idle &&
            bytesAreZero(socket->borrowedOperation(), sizeof(OVERLAPPED)),
        "receive issue failure did not roll back stable storage");
    api->socketError = WSAEWOULDBLOCK;
    auto blockedReceive = socket->issueReceive();
    requireError(
        blockedReceive,
        Domain::ErrorCodes::Conflict,
        true,
        "would-block receive issue did not return a retryable conflict");
    require(socket->state() == ConnectionState::Idle,
            "would-block receive issue retained operation storage");
    requireError(
        socket->issueSend({}),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "empty send span was accepted");

    const std::array<std::byte, 3U> bytes{
        std::byte{1}, std::byte{2}, std::byte{3}};
    api->sendStatus = 1;
    api->socketError = WSA_IO_PENDING;
    auto malformedSendStatus = socket->issueSend(bytes);
    requireError(
        malformedSendStatus,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "undefined native send status was accepted as pending");
    require(socket->state() == ConnectionState::Idle,
            "undefined native send status retained issued storage");

    api->sendStatus = SOCKET_ERROR;
    api->socketError = WSAECONNRESET;
    auto failedSend = socket->issueSend(bytes);
    requireError(
        failedSend,
        Domain::ErrorCodes::TransportClosed,
        true,
        "immediate send issue failure was accepted");
    require(socket->state() == ConnectionState::Idle,
            "send issue failure did not roll back state");
    api->socketError = WSAEWOULDBLOCK;
    auto blockedSend = socket->issueSend(bytes);
    requireError(
        blockedSend,
        Domain::ErrorCodes::Conflict,
        true,
        "would-block send issue did not return a retryable conflict");
    require(socket->state() == ConnectionState::Idle,
            "would-block send issue retained operation storage");

    api->receiveStatus = SOCKET_ERROR;
    api->socketError = WSA_IO_PENDING;
    static_cast<void>(take(socket->issueReceive()));
    OVERLAPPED foreign{};
    auto wrongPointer = socket->reap(
        ConnectionKey, 1U, &foreign, ERROR_SUCCESS);
    requireError(
        wrongPointer,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "foreign completion pointer was accepted");
    require(socket->state() == ConnectionState::ReceiveIssued,
            "foreign completion pointer consumed the live operation");
    auto wrongKey = socket->reap(
        CompletionKey{ConnectionKey.value() + 1U},
        1U,
        socket->borrowedOperation(),
        ERROR_SUCCESS);
    requireError(
        wrongKey,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "wrong connection key was accepted");
    require(socket->state() == ConnectionState::Idle,
            "exact malformed-key packet stranded its operation");
    requireError(
        socket->reap(
            ConnectionKey,
            1U,
            socket->borrowedOperation(),
            ERROR_SUCCESS),
        Domain::ErrorCodes::Conflict,
        false,
        "consumed malformed packet was reaped twice");

    static_cast<void>(take(socket->issueReceive()));
    auto receiveOverflow = socket->reap(
        ConnectionKey,
        static_cast<DWORD>(ConnectionSocket::ReceiveBufferLength + 1U),
        socket->borrowedOperation(),
        ERROR_SUCCESS);
    requireError(
        receiveOverflow,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "receive completion exceeded fixed storage");
    require(socket->state() == ConnectionState::Idle,
            "receive overflow did not consume exact operation");

    api->sendStatus = SOCKET_ERROR;
    api->socketError = WSA_IO_PENDING;
    static_cast<void>(take(socket->issueSend(bytes)));
    auto sendOverflow = socket->reap(
        ConnectionKey,
        4U,
        socket->borrowedOperation(),
        ERROR_SUCCESS);
    requireError(
        sendOverflow,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "send completion exceeded borrowed storage");
    require(socket->state() == ConnectionState::Idle,
            "send overflow did not consume exact operation");

    static_cast<void>(take(socket->issueSend(bytes)));
    auto zeroSend = socket->reap(
        ConnectionKey, 0U, socket->borrowedOperation(), ERROR_SUCCESS);
    requireError(
        zeroSend,
        Domain::ErrorCodes::TransportClosed,
        false,
        "zero-byte nonempty send was accepted");

    static_cast<void>(take(socket->issueReceive()));
    api->cancelResult = TRUE;
    auto cancellation = socket->requestCancellation();
    require(
        static_cast<bool>(cancellation) &&
            cancellation.value() == CancellationDisposition::Requested,
        "receive cancellation was not requested");
    require(
        socket->state() ==
            ConnectionState::ReceiveCancellationRequested &&
            api->lastOperation == socket->borrowedOperation(),
        "receive cancellation did not retain exact operation storage");
    auto repeatedCancellation = socket->requestCancellation();
    require(
        static_cast<bool>(repeatedCancellation) &&
            repeatedCancellation.value() ==
                CancellationDisposition::AlreadyRequested &&
            api->cancelCalls == 1U,
        "repeated receive cancellation was not coalesced");
    auto aborted = socket->reap(
        ConnectionKey,
        0U,
        socket->borrowedOperation(),
        ERROR_OPERATION_ABORTED);
    requireError(
        aborted,
        Domain::ErrorCodes::Cancelled,
        false,
        "cancelled receive returned wrong completion");

    static_cast<void>(take(socket->issueSend(bytes)));
    api->cancelResult = FALSE;
    api->systemError = ERROR_NOT_FOUND;
    auto completionWon = socket->requestCancellation();
    require(
        static_cast<bool>(completionWon) &&
            completionWon.value() ==
                CancellationDisposition::CompletionMayHaveWon,
        "ERROR_NOT_FOUND did not preserve completion-race storage");
    require(
        socket->state() == ConnectionState::SendCancellationRequested,
        "completion race did not retain send cancellation state");
    require(static_cast<bool>(socket->reap(
                ConnectionKey,
                1U,
                socket->borrowedOperation(),
                ERROR_SUCCESS)),
            "successful send could not win cancellation race");

    static_cast<void>(take(socket->issueReceive()));
    api->systemError = ERROR_ACCESS_DENIED;
    auto cancellationFailure = socket->requestCancellation();
    requireError(
        cancellationFailure,
        Domain::ErrorCodes::Unauthorized,
        false,
        "cancellation failure returned a disposition");
    require(socket->state() == ConnectionState::ReceiveIssued,
            "failed cancellation released issued storage");
    requireError(
        socket->reap(
            ConnectionKey,
            0U,
            socket->borrowedOperation(),
            ERROR_OPERATION_ABORTED),
        Domain::ErrorCodes::Cancelled,
        false,
        "operation could not reap after cancellation failure");

    static_cast<void>(take(socket->issueReceive()));
    api->cancelResult = TRUE;
    api->beginBlockingCancellation();
    std::optional<Domain::Result<CancellationDisposition>> serializedCancel;
    std::optional<Domain::Result<
        Detail::DashboardConnectionSocketReapResult>> serializedReap;
    std::atomic_bool reapStarted{};
    std::atomic_bool reapFinished{};
    std::thread cancelThread{[&] {
        serializedCancel.emplace(socket->requestCancellation());
    }};
    require(api->waitForCancellationEntry(),
            "blocking cancellation did not enter the native seam");
    std::thread reapThread{[&] {
        reapStarted.store(true, std::memory_order_release);
        serializedReap.emplace(socket->reap(
            ConnectionKey,
            1U,
            socket->borrowedOperation(),
            ERROR_SUCCESS));
        reapFinished.store(true, std::memory_order_release);
    }};
    while (!reapStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(10ms);
    require(!reapFinished.load(std::memory_order_acquire),
            "exact reap raced through an in-flight CancelIoEx call");
    require(socket->state() == ConnectionState::ReceiveIssued,
            "operation state changed before CancelIoEx returned");
    api->releaseCancellation();
    cancelThread.join();
    reapThread.join();
    require(
        serializedCancel.has_value() &&
            static_cast<bool>(*serializedCancel) &&
            serializedCancel->value() == CancellationDisposition::Requested,
        "serialized cancellation returned wrong disposition");
    require(
        serializedReap.has_value() && static_cast<bool>(*serializedReap),
        "completion could not win after serialized cancellation returned");
    require(socket->state() == ConnectionState::Idle,
            "serialized cancellation/reap did not restore idle state");

    api->shutdownStatus = 0;
    auto shutdown = socket->shutdownBoth();
    require(
        static_cast<bool>(shutdown) &&
            shutdown.value() == ShutdownDisposition::Requested,
        "SD_BOTH shutdown was not requested");
    require(
        api->lastShutdownHow == SD_BOTH && api->shutdownCalls == 1U &&
            socket->snapshot().shutdownRequested(),
        "shutdown did not retain its exact transition");
    auto repeatedShutdown = socket->shutdownBoth();
    require(
        static_cast<bool>(repeatedShutdown) &&
            repeatedShutdown.value() ==
                ShutdownDisposition::AlreadyRequested &&
            api->shutdownCalls == 1U,
        "repeated shutdown called the native API again");
    requireError(
        socket->issueReceive(),
        Domain::ErrorCodes::TransportClosed,
        false,
        "receive was issued after shutdown");
    requireError(
        socket->issueSend(bytes),
        Domain::ErrorCodes::TransportClosed,
        false,
        "send was issued after shutdown");
    require(socket->borrowedNativeSocket() != INVALID_SOCKET,
            "shutdown freed the accepted socket");

    for (const int closedCode : {WSAENOTCONN, WSAESHUTDOWN}) {
        auto closedPair = harness.acceptOne();
        const auto closedApi = std::make_shared<FakeNativeApi>();
        auto closedSocket = owner(closedPair, harness, closedApi);
        closedApi->shutdownStatus = SOCKET_ERROR;
        closedApi->socketError = closedCode;
        auto alreadyClosed = closedSocket->shutdownBoth();
        require(
            static_cast<bool>(alreadyClosed) &&
                alreadyClosed.value() == ShutdownDisposition::AlreadyClosed,
            "native already-closed shutdown was not idempotent success");
        require(closedSocket->snapshot().shutdownRequested(),
                "already-closed shutdown did not retain terminal state");
    }

    auto failedPair = harness.acceptOne();
    const auto failedApi = std::make_shared<FakeNativeApi>();
    auto failedSocketOwner = owner(failedPair, harness, failedApi);
    failedApi->shutdownStatus = SOCKET_ERROR;
    failedApi->socketError = WSAENETDOWN;
    auto failedShutdown = failedSocketOwner->shutdownBoth();
    requireError(
        failedShutdown,
        Domain::ErrorCodes::HostCapabilityUnavailable,
        true,
        "unexpected shutdown failure was accepted");
    require(!failedSocketOwner->snapshot().shutdownRequested(),
            "failed shutdown changed terminal state");
}

void repeatedFakeIssueReapKeepsOneStableOperation()
{
    RealHarness harness{"127.0.0.1", AF_INET, portBase(51000U)};
    auto pair = harness.acceptOne();
    const auto api = std::make_shared<FakeNativeApi>();
    api->receiveStatus = SOCKET_ERROR;
    api->sendStatus = SOCKET_ERROR;
    api->socketError = WSA_IO_PENDING;
    auto socket = owner(pair, harness, api);
    OVERLAPPED* const stableOperation = socket->borrowedOperation();
    const std::array<std::byte, 1U> oneByte{std::byte{0x5a}};

    for (std::size_t iteration{}; iteration < 500U; ++iteration) {
        if ((iteration % 2U) == 0U) {
            require(static_cast<bool>(socket->issueReceive()),
                    "stress receive issue failed");
            require(api->lastOperation == stableOperation,
                    "stress receive changed OVERLAPPED address");
            api->lastBuffers->buf[0] = static_cast<char>(0x5a);
            auto reaped = socket->reap(
                ConnectionKey, 1U, stableOperation, ERROR_SUCCESS);
            require(static_cast<bool>(reaped),
                    "stress receive reap failed");
            require(socket->receivedBytes().size() == 1U,
                    "stress receive view used wrong length");
        } else {
            require(static_cast<bool>(socket->issueSend(oneByte)),
                    "stress send issue failed");
            require(api->lastOperation == stableOperation,
                    "stress send changed OVERLAPPED address");
            auto reaped = socket->reap(
                ConnectionKey, 1U, stableOperation, ERROR_SUCCESS);
            require(static_cast<bool>(reaped), "stress send reap failed");
            require(socket->receivedBytes().empty(),
                    "stress send retained a receive view");
        }
        require(socket->state() == ConnectionState::Idle,
                "stress cycle did not restore idle state");
    }
}

[[nodiscard]] bool waitForPeerClose(const SOCKET client)
{
    u_long nonblocking = 1U;
    if (::ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    char byte{};
    while (std::chrono::steady_clock::now() < deadline) {
        const int received = ::recv(client, &byte, 1, 0);
        if (received == 0) {
            return true;
        }
        if (received == SOCKET_ERROR) {
            const int code = ::WSAGetLastError();
            if (code == WSAECONNRESET || code == WSAENOTCONN ||
                code == WSAESHUTDOWN) {
                return true;
            }
            if (code != WSAEWOULDBLOCK) {
                return false;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void associationFailureConsumesAndClosesAcceptedSocket()
{
    RealHarness harness{"127.0.0.1", AF_INET, portBase(51500U)};

    auto zeroPair = harness.acceptOne();
    const auto zeroApi = std::make_shared<FakeNativeApi>();
    auto zeroKeyOwner = ConnectionSocket::create(
        std::move(zeroPair.accepted),
        *harness.kernel,
        CompletionKey{0U},
        zeroApi);
    require(static_cast<bool>(zeroKeyOwner),
            "completion key zero was incorrectly reserved");
    require(zeroKeyOwner.value()->completionKey() == CompletionKey{0U},
            "completion key zero changed during association");
    zeroKeyOwner.value().reset();

    auto reservedPair = harness.acceptOne();
    const auto reservedApi = std::make_shared<FakeNativeApi>();
    auto reservedKey = ConnectionSocket::create(
        std::move(reservedPair.accepted),
        *harness.kernel,
        CompletionKey{Kernel::ShutdownKeyValue},
        reservedApi);
    requireError(
        reservedKey,
        Domain::ErrorCodes::InvalidRequest,
        false,
        "kernel shutdown key was associated to a connection");
    require(waitForPeerClose(reservedPair.client.get()),
            "reserved-key rejection did not close consumed ownership");

    auto pair = harness.acceptOne();
    harness.retireListener(true, false);
    const auto api = std::make_shared<FakeNativeApi>();
    auto created = ConnectionSocket::create(
        std::move(pair.accepted),
        *harness.kernel,
        ConnectionKey,
        api);
    requireError(
        created,
        Domain::ErrorCodes::TransportClosed,
        false,
        "shutdown kernel associated an accepted socket");
    require(
        waitForPeerClose(pair.client.get()),
        "association failure did not close consumed accepted ownership");
}

void receiveExactWithDeadline(
    const SOCKET socket,
    const std::span<const std::byte> expected)
{
    std::array<std::byte, 64U> storage{};
    require(expected.size() <= storage.size(), "expected payload was too large");
    u_long nonblocking = 1U;
    require(
        ::ioctlsocket(socket, FIONBIO, &nonblocking) == 0,
        "bounded peer receive could not enter nonblocking mode");
    const auto deadline =
        std::chrono::steady_clock::now() + NetworkOperationTimeout;
    std::size_t offset{};
    while (offset < expected.size()) {
        static_cast<void>(remainingNetworkTime(
            deadline, "peer receive exceeded its fixed deadline"));
        const int received = ::recv(
            socket,
            reinterpret_cast<char*>(storage.data() + offset),
            static_cast<int>(expected.size() - offset),
            0);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        require(received == SOCKET_ERROR,
                "peer closed before the exact bounded receive completed");
        const int code = ::WSAGetLastError();
        if (code == WSAEINTR) {
            continue;
        }
        require(code == WSAEWOULDBLOCK,
                "bounded peer receive failed with a socket error");
        const SHORT revents = waitForSocketEvent(
            socket,
            POLLIN,
            deadline,
            "peer receive exceeded its fixed deadline",
            "peer receive poll failed");
        require((revents & POLLNVAL) == 0,
                "peer receive polled an invalid socket");
    }
    require(
        offset == expected.size() &&
        std::equal(
            expected.begin(),
            expected.end(),
            storage.begin(),
            storage.begin() + static_cast<std::ptrdiff_t>(offset)),
        "bounded peer receive did not return the exact bytes");
}

void reapExactIocpReceive(
    ConnectionSocket& socket,
    CollectingKernelSink& sink,
    const std::span<const std::byte> expected)
{
    std::array<std::byte, 64U> accumulated{};
    require(expected.size() <= accumulated.size(),
            "expected IOCP receive payload was too large");
    const auto deadline =
        std::chrono::steady_clock::now() + NetworkOperationTimeout;
    std::size_t offset{};
    while (offset < expected.size()) {
        CollectingKernelSink::Completion completion{};
        require(
            sink.waitTake(
                completion,
                remainingNetworkTime(
                    deadline,
                    "real receive exceeded its fixed IOCP deadline")),
            "real receive completion did not arrive");
        require(
            completion.packet.completionKey == ConnectionKey &&
                completion.packet.operation == socket.borrowedOperation(),
            "real receive completion routing was not exact");
        auto received = socket.reap(
            completion.packet.completionKey,
            completion.packet.transferredBytes,
            completion.packet.operation,
            completion.nativeError);
        require(static_cast<bool>(received),
                "real receive completion failed");
        const auto view = socket.receivedBytes();
        require(
            received.value().operationKind() == OperationKind::Receive &&
                received.value().transferredBytes() == view.size(),
            "real receive view did not match its reaped byte count");
        require(
            view.size() <= expected.size() - offset,
            "real receive produced bytes beyond the exact expected payload");
        std::copy(
            view.begin(),
            view.end(),
            accumulated.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += view.size();
        if (offset < expected.size()) {
            require(static_cast<bool>(socket.issueReceive()),
                    "real partial receive could not issue its next chunk");
        }
    }
    require(
        offset == expected.size() &&
            std::equal(
                expected.begin(),
                expected.end(),
                accumulated.begin(),
                accumulated.begin() +
                    static_cast<std::ptrdiff_t>(offset)),
        "real IOCP receive did not accumulate the exact expected bytes");
}

void realSocketIocpSmoke(
    const std::string_view host,
    const int addressFamily,
    const std::uint16_t basePort)
{
    RealHarness harness{host, addressFamily, portBase(basePort)};
    auto pair = harness.acceptOne();
    harness.retireListener(false, true);
    require(harness.runtime == nullptr,
            "real smoke retained the outer Winsock runtime facade");
    auto socket = take(ConnectionSocket::create(
        std::move(pair.accepted),
        *harness.kernel,
        ConnectionKey));

    auto receiveIssue = socket->issueReceive();
    require(static_cast<bool>(receiveIssue), "real receive issue failed");
    const std::array<std::byte, 5U> inbound{
        std::byte{'i'},
        std::byte{'n'},
        std::byte{'b'},
        std::byte{'o'},
        std::byte{'x'}};
    sendExactWithDeadline(pair.client.get(), inbound);
    reapExactIocpReceive(*socket, *harness.sink, inbound);
    CollectingKernelSink::Completion completion{};

    const std::array<std::byte, 6U> outbound{
        std::byte{'o'},
        std::byte{'u'},
        std::byte{'t'},
        std::byte{'b'},
        std::byte{'o'},
        std::byte{'x'}};
    auto sendIssue = socket->issueSend(outbound);
    require(static_cast<bool>(sendIssue), "real send issue failed");
    require(harness.sink->waitTake(completion, 5s),
            "real send completion did not arrive");
    require(
        completion.packet.completionKey == ConnectionKey &&
            completion.packet.operation == socket->borrowedOperation(),
        "real send completion routing was not exact");
    auto sentCompletion = socket->reap(
        completion.packet.completionKey,
        completion.packet.transferredBytes,
        completion.packet.operation,
        completion.nativeError);
    require(static_cast<bool>(sentCompletion), "real send completion failed");
    require(
        sentCompletion.value().transferredBytes() == outbound.size(),
        "real send completion was unexpectedly partial");
    receiveExactWithDeadline(pair.client.get(), outbound);

    auto cancelIssue = socket->issueReceive();
    require(static_cast<bool>(cancelIssue), "real cancellation receive failed");
    auto cancellation = socket->requestCancellation();
    require(
        static_cast<bool>(cancellation) &&
            (cancellation.value() == CancellationDisposition::Requested ||
             cancellation.value() ==
                 CancellationDisposition::CompletionMayHaveWon),
        "real CancelIoEx did not retain completion ownership");
    require(harness.sink->waitTake(completion, 5s),
            "real cancelled receive completion did not arrive");
    auto cancelled = socket->reap(
        completion.packet.completionKey,
        completion.packet.transferredBytes,
        completion.packet.operation,
        completion.nativeError);
    requireError(
        cancelled,
        Domain::ErrorCodes::Cancelled,
        false,
        "real cancelled receive did not return cancellation");
    require(socket->state() == ConnectionState::Idle,
            "real cancellation did not restore idle state");

    auto shutdown = socket->shutdownBoth();
    require(static_cast<bool>(shutdown), "real SD_BOTH shutdown failed");
    socket.reset();
    require(harness.sink->healthy(),
            "real connection smoke overflowed or hit fatal IOCP state");
}

void realIpv4AcceptedSocketIocpSmoke()
{
    realSocketIocpSmoke("127.0.0.1", AF_INET, 52000U);
}

void realIpv6AcceptedSocketIocpSmoke()
{
    realSocketIocpSmoke("::1", AF_INET6, 54000U);
}

} // namespace

int main()
{
    try {
        immediatePendingMutualExclusionAndPartialSend();
        malformedIssuesCancellationAndShutdownAreTyped();
        repeatedFakeIssueReapKeepsOneStableOperation();
        associationFailureConsumesAndClosesAcceptedSocket();
        realIpv4AcceptedSocketIocpSmoke();
        realIpv6AcceptedSocketIocpSmoke();
        std::cout << "Dashboard connection socket tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard connection socket tests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard connection socket tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
