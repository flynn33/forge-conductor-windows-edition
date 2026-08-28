#include "Infrastructure/Windows/Detail/DashboardAcceptSlotSet.h"

#include <MSWSock.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

using AcceptApi = Detail::IDashboardAcceptSlotApi;
using CompletionKey = Detail::DashboardIoCompletionKey;
using Endpoint = Detail::DashboardLoopbackEndpoint;
using ExtensionApi = Detail::IDashboardWinsockExtensionApi;
using IoApi = Detail::IDashboardIoCompletionPortApi;
using Kernel = Detail::DashboardIocpWorkerKernel;
using KernelSink = Detail::IDashboardIocpCompletionSink;
using Listener = Detail::DashboardListeningSocket;
using ListenerApi = Detail::IDashboardListeningSocketApi;
using Port = Detail::DashboardIoCompletionPort;
using ReapDisposition = Detail::DashboardAcceptReapDisposition;
using ReapResult = Detail::DashboardAcceptReapResult;
using Runtime = Detail::DashboardWinsockRuntime;
using Set = Detail::DashboardAcceptSlotSet;
using Snapshot = Detail::DashboardAcceptSlotSetSnapshot;
using WinsockApi = Detail::IDashboardWinsockApi;

constexpr CompletionKey ListenerKey{0xACCE5510U};

static_assert(std::is_final_v<Set>);
static_assert(std::is_final_v<Snapshot>);
static_assert(std::is_final_v<ReapResult>);
static_assert(Set::SlotCount == 4U);
static_assert(Set::SlotCount == Kernel::WorkerCount);
static_assert(!std::is_copy_constructible_v<Set>);
static_assert(!std::is_move_constructible_v<Set>);
static_assert(!std::is_copy_constructible_v<ReapResult>);
static_assert(std::is_nothrow_move_constructible_v<ReapResult>);
static_assert(std::is_nothrow_destructible_v<Set>);
static_assert(noexcept(std::declval<const Set&>().snapshot()));
static_assert(!noexcept(std::declval<const Set&>().fullLifecycleFailure()));
static_assert(noexcept(std::declval<Set&>().start(
    std::declval<Kernel&>())));
static_assert(noexcept(
    std::declval<Set&>().closeAdmissionAndRequestCancellation()));
static_assert(noexcept(std::declval<Set&>().reap(
    ListenerKey, 0U, nullptr, ERROR_SUCCESS)));

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

enum class Event : std::uint8_t {
    DiscoverAccept,
    DiscoverAddresses,
    Associate,
    Issue,
    Cancel,
    UpdateContext,
    ExtractAddresses,
};

class EventRecorder final {
public:
    void record(const Event event) noexcept
    {
        const std::lock_guard lock{mutex_};
        if (count_ < events_.size()) {
            events_[count_++] = event;
        }
    }

    [[nodiscard]] std::size_t count(const Event searched) const noexcept
    {
        const std::lock_guard lock{mutex_};
        std::size_t result{};
        for (std::size_t index{}; index < count_; ++index) {
            if (events_[index] == searched) {
                ++result;
            }
        }
        return result;
    }

    [[nodiscard]] std::size_t indexOf(const Event searched) const
    {
        const std::lock_guard lock{mutex_};
        for (std::size_t index{}; index < count_; ++index) {
            if (events_[index] == searched) {
                return index;
            }
        }
        fail("expected event was not recorded");
    }

private:
    mutable std::mutex mutex_;
    std::array<Event, 256U> events_{};
    std::size_t count_{};
};

class FakeWinsockApi final : public WinsockApi {
public:
    [[nodiscard]] int startup(
        const WORD requestedVersion,
        WSADATA& data) noexcept override
    {
        ++startupCalls;
        requestedVersionValue = requestedVersion;
        data.wVersion = MAKEWORD(2, 2);
        return 0;
    }

    [[nodiscard]] int cleanup() noexcept override
    {
        ++cleanupCalls;
        return 0;
    }

    [[nodiscard]] SOCKET createSocket(
        const int addressFamily,
        const int socketType,
        const int protocol,
        const DWORD flags) noexcept override
    {
        ++createCalls;
        lastAddressFamily = addressFamily;
        lastSocketType = socketType;
        lastProtocol = protocol;
        lastFlags = flags;
        return nextSocket++;
    }

    [[nodiscard]] int closeSocket(const SOCKET socket) noexcept override
    {
        if (closeCalls < closedSockets.size()) {
            closedSockets[closeCalls] = socket;
        }
        ++closeCalls;
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override { return nativeError; }

    SOCKET nextSocket{static_cast<SOCKET>(0x4100U)};
    int nativeError{WSAENOBUFS};
    std::array<SOCKET, 64U> closedSockets{};
    std::size_t startupCalls{};
    std::size_t cleanupCalls{};
    std::size_t createCalls{};
    std::size_t closeCalls{};
    WORD requestedVersionValue{};
    int lastAddressFamily{};
    int lastSocketType{};
    int lastProtocol{};
    DWORD lastFlags{};
};

class FakeListenerApi final : public ListenerApi {
public:
    explicit FakeListenerApi(const Endpoint& endpoint) noexcept
    {
        observedLength_ = endpoint.nativeAddressLength();
        std::memcpy(
            &observedAddress_,
            endpoint.nativeAddress(),
            static_cast<std::size_t>(observedLength_));
    }

    [[nodiscard]] int setSocketOption(
        SOCKET,
        int,
        int,
        const char*,
        int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int bindSocket(
        SOCKET,
        const sockaddr*,
        int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int getSocketName(
        SOCKET,
        sockaddr* const address,
        int& addressLength) noexcept override
    {
        if (address == nullptr || addressLength < observedLength_) {
            return SOCKET_ERROR;
        }
        std::memcpy(
            address,
            &observedAddress_,
            static_cast<std::size_t>(observedLength_));
        addressLength = observedLength_;
        return 0;
    }

    [[nodiscard]] int listenSocket(SOCKET, int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override { return WSAEINVAL; }

private:
    sockaddr_storage observedAddress_{};
    int observedLength_{};
};

struct FakeExtensionState final {
    std::shared_ptr<EventRecorder> recorder;
    std::uint16_t listenerPort{};
    int addressFamily{AF_INET};
    std::size_t issueCalls{};
    std::size_t failIssueCall{};
    std::size_t synchronousIssueCall{};
    int issueError{WSAENOBUFS};
    int lastNativeError{WSA_IO_PENDING};
    SOCKET listenerSocket{INVALID_SOCKET};
    std::array<OVERLAPPED*, 4U> uniqueOperations{};
    std::size_t uniqueOperationCount{};
    OVERLAPPED* lastOperation{};

    void remember(OVERLAPPED* const operation) noexcept
    {
        lastOperation = operation;
        for (std::size_t index{}; index < uniqueOperationCount; ++index) {
            if (uniqueOperations[index] == operation) {
                return;
            }
        }
        if (uniqueOperationCount < uniqueOperations.size()) {
            uniqueOperations[uniqueOperationCount++] = operation;
        }
    }
};

FakeExtensionState* activeExtensionState{};

BOOL PASCAL fakeAcceptEx(
    const SOCKET listenerSocket,
    SOCKET,
    PVOID,
    DWORD,
    DWORD,
    DWORD,
    LPDWORD const bytesReceived,
    LPOVERLAPPED const operation)
{
    if (activeExtensionState == nullptr) {
        return FALSE;
    }
    auto& state = *activeExtensionState;
    state.recorder->record(Event::Issue);
    ++state.issueCalls;
    state.listenerSocket = listenerSocket;
    state.remember(operation);
    if (bytesReceived != nullptr) {
        *bytesReceived = 0U;
    }
    if (state.issueCalls == state.failIssueCall) {
        state.lastNativeError = state.issueError;
        return FALSE;
    }
    state.lastNativeError = WSA_IO_PENDING;
    return state.issueCalls == state.synchronousIssueCall ? TRUE : FALSE;
}

void populateAcceptedAddress(
    void* const destination,
    const int addressFamily,
    const std::uint16_t port) noexcept
{
    if (addressFamily == AF_INET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::memcpy(destination, &address, sizeof(address));
        return;
    }

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_loopback;
    std::memcpy(destination, &address, sizeof(address));
}

VOID PASCAL fakeGetAcceptExSockaddrs(
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    DWORD,
    LPSOCKADDR* const localAddress,
    LPINT const localAddressLengthActual,
    LPSOCKADDR* const remoteAddress,
    LPINT const remoteAddressLengthActual)
{
    if (activeExtensionState == nullptr) {
        return;
    }
    auto& state = *activeExtensionState;
    state.recorder->record(Event::ExtractAddresses);
    auto* const bytes = static_cast<std::byte*>(outputBuffer);
    auto* const local = reinterpret_cast<sockaddr*>(bytes + 16U);
    auto* const remote = reinterpret_cast<sockaddr*>(
        bytes + receiveDataLength + localAddressLength + 16U);
    populateAcceptedAddress(
        local, state.addressFamily, state.listenerPort);
    populateAcceptedAddress(remote, state.addressFamily, 49123U);
    *localAddress = local;
    *remoteAddress = remote;
    const int length = state.addressFamily == AF_INET
        ? static_cast<int>(sizeof(sockaddr_in))
        : static_cast<int>(sizeof(sockaddr_in6));
    *localAddressLengthActual = length;
    *remoteAddressLengthActual = length;
}

[[nodiscard]] bool sameGuid(const GUID& left, const GUID& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

class FakeExtensionApi final : public ExtensionApi {
public:
    explicit FakeExtensionApi(FakeExtensionState& state) noexcept
        : state_{state}
    {
    }

    [[nodiscard]] int ioctl(
        const SOCKET socket,
        const DWORD controlCode,
        const void* const inputBuffer,
        const DWORD inputBufferLength,
        void* const outputBuffer,
        const DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept override
    {
        ++ioctlCalls;
        discoverySocket = socket;
        if (controlCode != SIO_GET_EXTENSION_FUNCTION_POINTER ||
            inputBuffer == nullptr || outputBuffer == nullptr ||
            inputBufferLength != sizeof(GUID)) {
            return SOCKET_ERROR;
        }
        const auto& identifier = *static_cast<const GUID*>(inputBuffer);
        if (sameGuid(identifier, WSAID_ACCEPTEX) &&
            outputBufferLength == sizeof(LPFN_ACCEPTEX)) {
            state_.recorder->record(Event::DiscoverAccept);
            const LPFN_ACCEPTEX function = &fakeAcceptEx;
            std::memcpy(outputBuffer, &function, sizeof(function));
            bytesReturned = static_cast<DWORD>(sizeof(function));
            return 0;
        }
        if (sameGuid(identifier, WSAID_GETACCEPTEXSOCKADDRS) &&
            outputBufferLength == sizeof(LPFN_GETACCEPTEXSOCKADDRS)) {
            state_.recorder->record(Event::DiscoverAddresses);
            const LPFN_GETACCEPTEXSOCKADDRS function =
                &fakeGetAcceptExSockaddrs;
            std::memcpy(outputBuffer, &function, sizeof(function));
            bytesReturned = static_cast<DWORD>(sizeof(function));
            return 0;
        }
        return SOCKET_ERROR;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        return state_.lastNativeError;
    }

    FakeExtensionState& state_;
    std::size_t ioctlCalls{};
    SOCKET discoverySocket{INVALID_SOCKET};
};

class FakeAcceptApi final : public AcceptApi {
public:
    explicit FakeAcceptApi(std::shared_ptr<EventRecorder> recorder) noexcept
        : recorder_{std::move(recorder)}
    {
    }

    [[nodiscard]] int updateAcceptContext(
        const SOCKET acceptedSocket,
        const SOCKET listenerSocket) noexcept override
    {
        recorder_->record(Event::UpdateContext);
        ++updateCalls;
        lastAcceptedSocket = acceptedSocket;
        lastListenerSocket = listenerSocket;
        return updateStatus;
    }

    [[nodiscard]] BOOL cancelAccept(
        const SOCKET listenerSocket,
        OVERLAPPED* const operation) noexcept override
    {
        recorder_->record(Event::Cancel);
        ++cancelCalls;
        lastListenerSocket = listenerSocket;
        if (cancelCalls <= cancelledOperations.size()) {
            cancelledOperations[cancelCalls - 1U] = operation;
        }
        return cancelCalls == failCancelCall ? FALSE : TRUE;
    }

    [[nodiscard]] int lastSocketError() noexcept override
    {
        return socketError;
    }

    [[nodiscard]] DWORD lastSystemError() noexcept override
    {
        return systemError;
    }

    std::shared_ptr<EventRecorder> recorder_;
    std::array<OVERLAPPED*, 16U> cancelledOperations{};
    std::size_t updateCalls{};
    std::size_t cancelCalls{};
    std::size_t failCancelCall{};
    int updateStatus{};
    int socketError{WSAECONNRESET};
    DWORD systemError{ERROR_ACCESS_DENIED};
    SOCKET lastAcceptedSocket{INVALID_SOCKET};
    SOCKET lastListenerSocket{INVALID_SOCKET};
};

thread_local DWORD fakeIoError{ERROR_SUCCESS};

class FakeIoApi final : public IoApi {
public:
    struct QueuedCompletion final {
        DWORD transferredBytes{};
        ULONG_PTR completionKey{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE fileHandle,
        const HANDLE existingCompletionPort,
        const ULONG_PTR completionKey,
        const DWORD concurrentThreadCount) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (fileHandle == INVALID_HANDLE_VALUE) {
            ++createCalls;
            createConcurrency = concurrentThreadCount;
            return portHandle();
        }
        recorder_->record(Event::Associate);
        ++associationCalls;
        associatedHandle = fileHandle;
        associatedKey = completionKey;
        associationConcurrency = concurrentThreadCount;
        if (failAssociation) {
            fakeIoError = ERROR_INVALID_HANDLE;
            return nullptr;
        }
        return existingCompletionPort;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        HANDLE,
        const DWORD transferredBytes,
        const ULONG_PTR completionKey,
        OVERLAPPED* const operation) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (queueCount_ == queue_.size()) {
            fakeIoError = ERROR_NOT_ENOUGH_MEMORY;
            return FALSE;
        }
        const std::size_t tail = (queueHead_ + queueCount_) % queue_.size();
        queue_[tail] = QueuedCompletion{
            transferredBytes, completionKey, operation};
        ++queueCount_;
        changed_.notify_one();
        return TRUE;
    }

    [[nodiscard]] BOOL getQueuedCompletionStatus(
        HANDLE,
        DWORD& transferredBytes,
        ULONG_PTR& completionKey,
        OVERLAPPED*& operation,
        const DWORD timeoutMilliseconds) noexcept override
    {
        std::unique_lock lock{mutex_};
        const auto ready = [this] { return queueCount_ != 0U || closed_; };
        if (!ready()) {
            static_cast<void>(changed_.wait_for(
                lock,
                std::chrono::milliseconds{timeoutMilliseconds},
                ready));
        }
        if (queueCount_ == 0U) {
            transferredBytes = 0U;
            completionKey = 0U;
            operation = nullptr;
            fakeIoError = closed_ ? ERROR_INVALID_HANDLE : WAIT_TIMEOUT;
            return FALSE;
        }
        const auto completion = queue_[queueHead_];
        queueHead_ = (queueHead_ + 1U) % queue_.size();
        --queueCount_;
        transferredBytes = completion.transferredBytes;
        completionKey = completion.completionKey;
        operation = completion.operation;
        fakeIoError = ERROR_SUCCESS;
        return TRUE;
    }

    [[nodiscard]] BOOL closeHandle(HANDLE) noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++closeCalls;
        closed_ = true;
        changed_.notify_all();
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override { return fakeIoError; }

    explicit FakeIoApi(std::shared_ptr<EventRecorder> recorder) noexcept
        : recorder_{std::move(recorder)}
    {
    }

    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x9900U));
    }

    std::shared_ptr<EventRecorder> recorder_;
    bool failAssociation{};
    std::size_t createCalls{};
    std::size_t associationCalls{};
    std::size_t closeCalls{};
    DWORD createConcurrency{};
    HANDLE associatedHandle{};
    ULONG_PTR associatedKey{};
    DWORD associationConcurrency{};

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<QueuedCompletion, 16U> queue_{};
    std::size_t queueHead_{};
    std::size_t queueCount_{};
    bool closed_{};
};

class QuietKernelSink final : public KernelSink {
public:
    void consume(Detail::DashboardIoCompletionPacket, DWORD) noexcept override
    {
        ++consumeCalls;
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        ++fatalCalls;
        lastFatalError = nativeError;
    }

    std::atomic_size_t consumeCalls{};
    std::atomic_size_t fatalCalls{};
    std::atomic<DWORD> lastFatalError{};
};

class FakeContext final {
public:
    FakeContext()
        : endpoint{take(Endpoint::create("127.0.0.1", 18441U))},
          recorder{std::make_shared<EventRecorder>()},
          winsock{std::make_shared<FakeWinsockApi>()},
          runtime{take(Runtime::create(winsock))},
          listenerApi{std::make_shared<FakeListenerApi>(endpoint)},
          extensionState{
              recorder,
              endpoint.port(),
              AF_INET},
          extensionApi{std::make_shared<FakeExtensionApi>(extensionState)},
          acceptApi{std::make_shared<FakeAcceptApi>(recorder)},
          ioApi{std::make_shared<FakeIoApi>(recorder)},
          sink{std::make_shared<QuietKernelSink>()}
    {
        activeExtensionState = &extensionState;
        auto listener = take(Listener::create(
            *runtime, endpoint, listenerApi));
        listenerSocket = listener.borrowedNativeSocket();
        kernel = take(Kernel::create(take(Port::create(ioApi)), sink));
        set = take(Set::create(
            *runtime,
            std::move(listener),
            ListenerKey,
            extensionApi,
            acceptApi));
    }

    ~FakeContext() noexcept
    {
        drainWithoutAssertions();
        set.reset();
        if (kernel != nullptr) {
            kernel->shutdown();
            kernel.reset();
        }
        if (activeExtensionState == &extensionState) {
            activeExtensionState = nullptr;
        }
        runtime.reset();
    }

    FakeContext(const FakeContext&) = delete;
    FakeContext& operator=(const FakeContext&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept
    {
        return set->start(*kernel);
    }

    void drainWithoutAssertions() noexcept
    {
        if (set == nullptr) {
            return;
        }
        static_cast<void>(set->closeAdmissionAndRequestCancellation());
        for (std::size_t index{};
             index < extensionState.uniqueOperationCount;
             ++index) {
            static_cast<void>(set->reap(
                ListenerKey,
                0U,
                extensionState.uniqueOperations[index],
                ERROR_OPERATION_ABORTED));
        }
    }

    Endpoint endpoint;
    std::shared_ptr<EventRecorder> recorder;
    std::shared_ptr<FakeWinsockApi> winsock;
    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<FakeListenerApi> listenerApi;
    FakeExtensionState extensionState;
    std::shared_ptr<FakeExtensionApi> extensionApi;
    std::shared_ptr<FakeAcceptApi> acceptApi;
    std::shared_ptr<FakeIoApi> ioApi;
    std::shared_ptr<QuietKernelSink> sink;
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<Set> set;
    SOCKET listenerSocket{INVALID_SOCKET};
};

void requireSnapshotCounts(
    const Snapshot& snapshot,
    const std::size_t issued,
    const std::size_t idle,
    const std::size_t paused,
    const std::size_t awaitingReturn,
    const std::size_t cancellationRequested,
    const std::size_t drained,
    const std::string_view context)
{
    require(snapshot.issuedCount() == issued, context);
    require(snapshot.idleCount() == idle, context);
    require(snapshot.pausedCount() == paused, context);
    require(snapshot.awaitingReturnCount() == awaitingReturn, context);
    require(snapshot.cancellationRequestedCount() == cancellationRequested,
            context);
    require(snapshot.drainedCount() == drained, context);
    require(issued + idle + paused + awaitingReturn +
                cancellationRequested + drained == Set::SlotCount,
            "snapshot counts did not cover exactly four slots");
}

void factoryDiscoversExtensionsFromTheOwnedListener()
{
    FakeContext context;
    require(context.extensionApi->ioctlCalls == 2U,
            "factory did not discover exactly two extension functions");
    require(context.extensionApi->discoverySocket == context.listenerSocket,
            "extensions were not discovered from the owned listener socket");
    require(context.recorder->count(Event::DiscoverAccept) == 1U &&
                context.recorder->count(Event::DiscoverAddresses) == 1U,
            "factory did not retain one complete listener extension table");
    require(context.extensionState.issueCalls == 0U,
            "factory issued AcceptEx before listener association");
    requireSnapshotCounts(
        context.set->snapshot(), 0U, 4U, 0U, 0U, 0U, 0U,
        "fresh set did not own exactly four idle slots");
}

void startAssociatesBeforeExactlyFourIssues()
{
    FakeContext context;
    context.extensionState.synchronousIssueCall = 1U;
    const auto started = context.start();
    require(static_cast<bool>(started), "valid slot set did not start");
    require(context.ioApi->associationCalls == 1U,
            "start did not associate exactly one listener");
    require(context.ioApi->associatedHandle ==
                reinterpret_cast<HANDLE>(context.listenerSocket),
            "start associated the wrong listener socket");
    require(context.ioApi->associatedKey == ListenerKey.value(),
            "start associated the wrong typed generation key");
    require(context.recorder->indexOf(Event::Associate) <
                context.recorder->indexOf(Event::Issue),
            "AcceptEx was issued before listener IOCP association");
    require(context.extensionState.issueCalls == Set::SlotCount,
            "start did not prime exactly four slots");
    require(context.extensionState.uniqueOperationCount == Set::SlotCount,
            "four heap-stable slots did not expose four unique operations");
    const auto snapshot = context.set->snapshot();
    requireSnapshotCounts(
        snapshot, 4U, 0U, 0U, 0U, 0U, 0U,
        "started set did not retain four issued operations");
    require(snapshot.startAttempted() && snapshot.listenerAssociated() &&
                snapshot.admissionOpen(),
            "started snapshot omitted lifecycle state");
}

void associationFailureIssuesNothingAndDrainsIdleSlots()
{
    FakeContext context;
    context.ioApi->failAssociation = true;
    const auto started = context.start();
    requireError(
        started,
        Domain::ErrorCodes::InternalFailure,
        "association failure unexpectedly started admission");
    require(context.extensionState.issueCalls == 0U,
            "association failure still issued AcceptEx");
    const auto snapshot = context.set->snapshot();
    requireSnapshotCounts(
        snapshot, 0U, 0U, 0U, 0U, 0U, 4U,
        "association failure did not structurally drain idle slots");
    require(!snapshot.listenerAssociated() && !snapshot.admissionOpen(),
            "association failure published listener admission");
}

void terminalCloseBeforeStartPreventsNativeAssociation()
{
    FakeContext context;
    const auto closed = context.set->closeAdmissionAndRequestCancellation();
    require(!closed.has_value(), "pre-start close failed");
    const auto started = context.start();
    requireError(
        started,
        Domain::ErrorCodes::Conflict,
        "terminally closed set restarted");
    require(context.ioApi->associationCalls == 0U,
            "terminally closed set associated a native listener");
    require(context.extensionState.issueCalls == 0U,
            "terminally closed set issued AcceptEx");
    require(context.set->snapshot().fullyDrained(),
            "pre-start close did not drain all four slots");
}

void partialIssueFailureCancelsEveryIssuedSlotAndRetainsDrain()
{
    FakeContext context;
    context.extensionState.failIssueCall = 3U;
    context.extensionState.issueError = WSAENOBUFS;
    const auto started = context.start();
    requireError(
        started,
        Domain::ErrorCodes::LimitExceeded,
        "partial AcceptEx failure unexpectedly started admission");
    require(context.extensionState.issueCalls == 3U,
            "partial start did not stop at the failing issue");
    require(context.acceptApi->cancelCalls == 2U,
            "partial start did not cancel every previously issued slot");
    requireSnapshotCounts(
        context.set->snapshot(), 0U, 0U, 0U, 0U, 2U, 2U,
        "partial start did not retain issued operations for drain");

    for (std::size_t index{}; index < 2U; ++index) {
        auto reaped = context.set->reap(
            ListenerKey,
            0U,
            context.extensionState.uniqueOperations[index],
            ERROR_OPERATION_ABORTED);
        require(static_cast<bool>(reaped),
                "cancelled partial-start operation was not reaped");
        require(reaped.value().disposition() ==
                    ReapDisposition::FailureDrained,
                "cancelled partial-start operation was reissued");
    }
    require(context.set->snapshot().fullyDrained(),
            "partial-start owner did not become safely destructible after drain");
}

void closeIsIdempotentAndCancelsExactlyFour()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    require(!context.set->closeAdmissionAndRequestCancellation().has_value(),
            "close admission failed");
    require(context.acceptApi->cancelCalls == Set::SlotCount,
            "close did not cancel exactly four issued accepts");
    require(!context.set->closeAdmissionAndRequestCancellation().has_value(),
            "repeated close was not idempotent");
    require(context.acceptApi->cancelCalls == Set::SlotCount,
            "repeated close duplicated cancellation requests");
    requireSnapshotCounts(
        context.set->snapshot(), 0U, 0U, 0U, 0U, 4U, 0U,
        "close did not retain all cancellation-requested slots");
}

void successfulCompletionPausesUntilExactTokenReturns()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    OVERLAPPED* const operation =
        context.extensionState.uniqueOperations[2U];
    auto reaped = take(context.set->reap(
        ListenerKey, 0U, operation, ERROR_SUCCESS));
    require(reaped.disposition() ==
                ReapDisposition::AcceptedAndPaused,
            "successful accept did not pause for caller admission");
    require(reaped.hasAcceptedConnection(),
            "successful accept did not return its move-only connection");
    require(reaped.hasResumeToken(),
            "successful accept did not return one resume token");
    require(reaped.acceptFailure() == nullptr &&
                reaped.reissueFailure() == nullptr,
            "successful re-prime reported a spurious error");
    require(context.extensionState.issueCalls == 4U,
            "successful accept reissued before caller admission");
    auto connection = reaped.takeAcceptedConnection();
    require(connection.has_value(),
            "accepted connection could not be moved from the result");
    require(!reaped.hasAcceptedConnection(),
            "accepted connection remained observable after move-out");
    requireSnapshotCounts(
        context.set->snapshot(), 3U, 0U, 1U, 0U, 0U, 0U,
        "successful accept did not leave one bounded paused slot");
    auto token = reaped.takeResumeToken();
    require(token.has_value() && token->valid(),
            "resume token could not be moved from the result");
    auto resumed = context.set->resume(std::move(*token));
    require(static_cast<bool>(resumed) &&
                resumed.value() ==
                    Detail::DashboardAcceptResumeDisposition::Reissued,
            "caller return did not reissue the paused slot");
    require(!token->valid(), "resume did not consume its one-shot token");
    require(context.extensionState.issueCalls == 5U &&
                context.extensionState.lastOperation == operation,
            "resume did not re-prime the exact same slot once");
    const auto duplicateResume = context.set->resume(std::move(*token));
    requireError(
        duplicateResume,
        Domain::ErrorCodes::Conflict,
        "one-shot resume token was accepted twice");
    requireSnapshotCounts(
        context.set->snapshot(), 4U, 0U, 0U, 0U, 0U, 0U,
        "resume changed the fixed four-slot bound");
}

void crossSetTokenRejectionIsNonmutatingAndOriginCanResume()
{
    FakeContext origin;
    require(static_cast<bool>(origin.start()), "origin set did not start");
    auto accepted = take(origin.set->reap(
        ListenerKey,
        0U,
        origin.extensionState.uniqueOperations[0U],
        ERROR_SUCCESS));
    auto token = accepted.takeResumeToken();
    require(token.has_value() && token->valid(),
            "origin accept omitted its resume token");
    auto connection = accepted.takeAcceptedConnection();
    connection.reset();
    const auto originBefore = origin.set->snapshot();

    FakeContext foreign;
    require(static_cast<bool>(foreign.start()), "foreign set did not start");
    const auto foreignBefore = foreign.set->snapshot();
    const auto rejected = foreign.set->resume(std::move(*token));
    requireError(
        rejected,
        Domain::ErrorCodes::IntegrityFailure,
        "foreign listener generation accepted another set's token");
    require(token->valid(),
            "foreign rejection consumed the origin set's live token");
    const auto originAfter = origin.set->snapshot();
    const auto foreignAfter = foreign.set->snapshot();
    require(originAfter.issuedCount() == originBefore.issuedCount() &&
                originAfter.pausedCount() == originBefore.pausedCount() &&
                originAfter.outstandingResumeTokenCount() ==
                    originBefore.outstandingResumeTokenCount(),
            "cross-set rejection mutated the origin set");
    require(foreignAfter.issuedCount() == foreignBefore.issuedCount() &&
                foreignAfter.pausedCount() == foreignBefore.pausedCount() &&
                foreignAfter.outstandingResumeTokenCount() ==
                    foreignBefore.outstandingResumeTokenCount(),
            "cross-set rejection mutated the foreign set");

    activeExtensionState = &origin.extensionState;
    auto resumed = origin.set->resume(std::move(*token));
    require(static_cast<bool>(resumed) &&
                resumed.value() ==
                    Detail::DashboardAcceptResumeDisposition::Reissued,
            "origin set could not resume after foreign rejection");
    require(!token->valid(),
            "successful origin resume did not consume the token");
    require(origin.extensionState.issueCalls == 5U &&
                foreign.extensionState.issueCalls == 4U,
            "cross-set rejection or origin return issued the wrong work");
}

void fourSuccessfulAcceptsRemainPausedUntilAllTokensReturn()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    std::array<std::optional<ReapResult>, Set::SlotCount> outcomes{};
    std::array<std::optional<Detail::DashboardAcceptResumeToken>,
               Set::SlotCount> tokens{};

    for (std::size_t index{}; index < Set::SlotCount; ++index) {
        outcomes[index].emplace(take(context.set->reap(
            ListenerKey,
            0U,
            context.extensionState.uniqueOperations[index],
            ERROR_SUCCESS)));
        require(outcomes[index]->disposition() ==
                    ReapDisposition::AcceptedAndPaused,
                "successful accept did not pause its slot");
        tokens[index] = outcomes[index]->takeResumeToken();
        require(tokens[index].has_value() && tokens[index]->valid(),
                "paused slot omitted its exact token");
        auto connection = outcomes[index]->takeAcceptedConnection();
        require(connection.has_value(),
                "paused slot omitted its accepted connection");
    }

    const auto withheld = context.set->snapshot();
    requireSnapshotCounts(
        withheld, 0U, 0U, 4U, 0U, 0U, 0U,
        "withholding four tokens did not pause all four slots");
    require(withheld.outstandingResumeTokenCount() == Set::SlotCount,
            "snapshot omitted paused token obligations");
    require(context.extensionState.issueCalls == Set::SlotCount,
            "withheld tokens allowed replacement AcceptEx issues");

    for (auto& token : tokens) {
        auto resumed = context.set->resume(std::move(*token));
        require(static_cast<bool>(resumed) &&
                    resumed.value() ==
                        Detail::DashboardAcceptResumeDisposition::Reissued,
                "returned paused token did not reissue its exact slot");
        require(!token->valid(),
                "returned paused token remained live");
    }
    requireSnapshotCounts(
        context.set->snapshot(), 4U, 0U, 0U, 0U, 0U, 0U,
        "returning all four tokens did not restore four issued slots");
    require(context.set->snapshot().outstandingResumeTokenCount() == 0U,
            "returned tokens remained in the snapshot obligation count");
    require(context.extensionState.issueCalls == Set::SlotCount * 2U,
            "returning four tokens did not issue exactly four replacements");
}

void closeWaitsForPausedTokenReturnWithoutNativeCancellation()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        ERROR_SUCCESS));
    auto token = reaped.takeResumeToken();
    require(token.has_value(), "paused accept omitted its return token");
    require(!context.set->closeAdmissionAndRequestCancellation().has_value(),
            "close admission failed with one paused slot");
    require(context.acceptApi->cancelCalls == 3U,
            "close sent native cancellation for the paused slot");
    const auto awaiting = context.set->snapshot();
    requireSnapshotCounts(
        awaiting, 0U, 0U, 0U, 1U, 3U, 0U,
        "closed paused slot was counted drained before token return");
    require(!awaiting.fullyDrained(),
            "set reported full drain while a token remained outstanding");
    auto returned = context.set->resume(std::move(*token));
    require(static_cast<bool>(returned) &&
                returned.value() ==
                    Detail::DashboardAcceptResumeDisposition::ReturnedAfterClose,
            "token return after close did not drain without issue");
    require(context.extensionState.issueCalls == 4U,
            "token return after close issued new AcceptEx work");
    requireSnapshotCounts(
        context.set->snapshot(), 0U, 0U, 0U, 0U, 3U, 1U,
        "returned closed token did not drain exactly its slot");
}

void closeRacingSuccessfulCompletionDrainsWithoutReplacementWhenCloseWins()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    require(!context.set->closeAdmissionAndRequestCancellation().has_value(),
            "close admission failed");
    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        ERROR_SUCCESS));
    require(reaped.disposition() == ReapDisposition::AcceptedAndDrained,
            "completion that won cancellation was reissued after close");
    require(reaped.hasAcceptedConnection(),
            "cancel-vs-success lost the accepted connection");
    require(context.extensionState.issueCalls == 4U,
            "closed admission issued a replacement accept");
    requireSnapshotCounts(
        context.set->snapshot(), 0U, 0U, 0U, 0U, 3U, 1U,
        "cancel-vs-success did not drain exactly one slot");
}

void retryableClientFailureReissuesTheExactSameSlot()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    OVERLAPPED* const operation =
        context.extensionState.uniqueOperations[1U];
    auto reaped = take(context.set->reap(
        ListenerKey, 0U, operation, ERROR_NETNAME_DELETED));
    require(reaped.disposition() == ReapDisposition::FailureReissued,
            "retryable client failure was not re-primed");
    require(!reaped.hasAcceptedConnection(),
            "failed accept produced an accepted connection");
    require(reaped.acceptFailure() != nullptr &&
                reaped.acceptFailure()->code ==
                    Domain::ErrorCodes::TransportClosed &&
                reaped.acceptFailure()->retryable,
            "client failure lost its typed retryable classification");
    require(context.extensionState.issueCalls == 5U &&
                context.extensionState.lastOperation == operation,
            "client failure did not re-prime the same slot exactly once");
    requireSnapshotCounts(
        context.set->snapshot(), 4U, 0U, 0U, 0U, 0U, 0U,
        "client retry changed the fixed slot count");
}

void retryableListenerFailureClosesAdmissionInsteadOfSpinning()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        WSAENETDOWN));
    require(reaped.disposition() == ReapDisposition::FailureDrained,
            "listener-wide failure was treated as a client retry");
    require(reaped.acceptFailure() != nullptr &&
                reaped.acceptFailure()->code ==
                    Domain::ErrorCodes::HostCapabilityUnavailable &&
                reaped.acceptFailure()->retryable,
            "listener-wide failure lost its typed classification");
    require(context.extensionState.issueCalls == 4U,
            "listener-wide failure entered an automatic retry loop");
    require(context.acceptApi->cancelCalls == 3U,
            "listener-wide failure did not cancel peer accepts");
    const auto snapshot = context.set->snapshot();
    require(!snapshot.admissionOpen(),
            "listener-wide failure left admission open");
    requireSnapshotCounts(
        snapshot, 0U, 0U, 0U, 0U, 3U, 1U,
        "listener-wide failure did not begin bounded drain");
}

void cancellationFailureDrainsWithoutReprime()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    require(!context.set->closeAdmissionAndRequestCancellation().has_value(),
            "close admission failed");
    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[3U],
        ERROR_OPERATION_ABORTED));
    require(reaped.disposition() == ReapDisposition::FailureDrained,
            "shutdown cancellation was re-primed");
    require(reaped.acceptFailure() != nullptr &&
                reaped.acceptFailure()->code ==
                    Domain::ErrorCodes::Cancelled &&
                reaped.cancellationRequestedForSlot(),
            "shutdown cancellation lost its typed error");
    require(context.extensionState.issueCalls == 4U,
            "shutdown cancellation issued a replacement");
}

void unsolicitedAbortHasNoExactCancellationProvenance()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    context.acceptApi->failCancelCall = 1U;
    context.acceptApi->systemError = ERROR_ACCESS_DENIED;
    const auto closeFailure =
        context.set->closeAdmissionAndRequestCancellation();
    require(closeFailure.has_value() &&
                closeFailure->kind ==
                    Detail::DashboardAcceptLifecycleFailureKind::Unauthorized,
            "injected cancellation failure was not returned");
    require(!context.set->closeAdmissionAndRequestCancellation().has_value() &&
                context.acceptApi->cancelCalls == Set::SlotCount,
            "ordinary repeated close retried a failed CancelIoEx");
    requireSnapshotCounts(
        context.set->snapshot(), 1U, 0U, 0U, 0U, 3U, 0U,
        "failed cancellation did not retain one exact issued slot");

    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        ERROR_OPERATION_ABORTED));
    require(reaped.disposition() == ReapDisposition::FailureDrained &&
                reaped.acceptFailure() != nullptr &&
                reaped.acceptFailure()->code ==
                    Domain::ErrorCodes::Cancelled,
            "unsolicited abort lost its native cancellation diagnostic");
    require(!reaped.cancellationRequestedForSlot(),
            "generation-wide close invented per-slot cancellation provenance");
}

void listenerForceCloseBoundsOneAndRepeatedCancellationFailures()
{
    const auto runCase = [](const bool failRetry) {
        FakeContext context;
        require(static_cast<bool>(context.start()),
                "slot set did not start");
        context.acceptApi->failCancelCall = 1U;
        context.acceptApi->systemError = ERROR_ACCESS_DENIED;
        const auto firstFailure =
            context.set->closeAdmissionAndRequestCancellation();
        require(firstFailure.has_value() &&
                    firstFailure->kind ==
                        Detail::DashboardAcceptLifecycleFailureKind::Unauthorized,
                "first CancelIoEx failure was not retained");

        context.acceptApi->failCancelCall = failRetry ? 5U : 0U;
        const auto forceFailure =
            context.set->forceCloseListenerAndRequestCancellation();
        require(forceFailure.has_value() == failRetry,
                "force-close cancellation retry used the wrong result");
        require(context.acceptApi->cancelCalls == 5U,
                "force close did not perform one bounded cancellation retry");
        const auto forced = context.set->snapshot();
        require(forced.listenerForceClosed() &&
                    !forced.fullyDrained(),
                "listener force close released native slot ownership");
        requireSnapshotCounts(
            forced, 0U, 0U, 0U, 0U, 4U, 0U,
            "listener close did not record exact outstanding-slot provenance");
        require(context.winsock->closeCalls == 1U &&
                    context.winsock->closedSockets[0U] ==
                        context.listenerSocket,
                "listener force close did not invalidate the exact RAII handle");

        for (std::size_t index{}; index < Set::SlotCount; ++index) {
            auto reaped = take(context.set->reap(
                ListenerKey,
                0U,
                context.extensionState.uniqueOperations[index],
                index == 0U ? ERROR_SUCCESS
                            : ERROR_OPERATION_ABORTED));
            require(reaped.cancellationRequestedForSlot(),
                    "listener-close reap lost exact per-slot provenance");
            if (index == 0U) {
                require(reaped.disposition() ==
                            ReapDisposition::FailureDrained &&
                            context.acceptApi->updateCalls == 0U,
                        "late success reused an invalidated listener handle");
            }
        }
        require(context.set->snapshot().fullyDrained(),
                "listener close did not retain storage through exact reaps");
    };

    runCase(false);
    runCase(true);
}

void wrongKeyAndForeignPointerAreNonMutating()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    const auto before = context.set->snapshot();
    const auto wrongKey = context.set->reap(
        CompletionKey{ListenerKey.value() + 1U},
        0U,
        context.extensionState.uniqueOperations[0U],
        ERROR_SUCCESS);
    requireError(
        wrongKey,
        Domain::ErrorCodes::IntegrityFailure,
        "wrong completion key was accepted");
    OVERLAPPED foreign{};
    const auto wrongPointer = context.set->reap(
        ListenerKey, 0U, &foreign, ERROR_SUCCESS);
    requireError(
        wrongPointer,
        Domain::ErrorCodes::IntegrityFailure,
        "foreign OVERLAPPED pointer was accepted");
    const auto after = context.set->snapshot();
    require(after.issuedCount() == before.issuedCount() &&
                after.cancellationRequestedCount() ==
                    before.cancellationRequestedCount() &&
                after.drainedCount() == before.drainedCount() &&
                after.admissionOpen() == before.admissionOpen(),
            "misrouted completion mutated slot ownership");
    require(context.extensionState.issueCalls == 4U,
            "misrouted completion issued new native work");
}

void nonzeroBytePacketIsConsumedThenRetiresTheGeneration()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    OVERLAPPED* const operation =
        context.extensionState.uniqueOperations[0U];
    const auto corrupted = context.set->reap(
        ListenerKey, 1U, operation, ERROR_SUCCESS);
    requireError(
        corrupted,
        Domain::ErrorCodes::IntegrityFailure,
        "nonzero-byte AcceptEx packet was accepted");
    const auto snapshot = context.set->snapshot();
    requireSnapshotCounts(
        snapshot, 0U, 0U, 0U, 0U, 3U, 1U,
        "corrupt dequeued packet stranded its slot instead of draining it");
    require(!snapshot.admissionOpen(),
            "corrupt AcceptEx packet left admission open");
    const auto duplicate = context.set->reap(
        ListenerKey, 0U, operation, ERROR_OPERATION_ABORTED);
    requireError(
        duplicate,
        Domain::ErrorCodes::IntegrityFailure,
        "already-consumed corrupt packet was reaped twice");
}

void explicitResumeIssueFailurePreservesConnectionAndClosesAdmission()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    context.extensionState.failIssueCall = 5U;
    context.extensionState.issueError = WSAENOBUFS;
    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        ERROR_SUCCESS));
    require(reaped.disposition() == ReapDisposition::AcceptedAndPaused,
            "successful accept did not pause before replacement issue");
    require(reaped.hasAcceptedConnection(),
            "replacement failure discarded the accepted connection");
    auto token = reaped.takeResumeToken();
    require(token.has_value(),
            "successful accept omitted its explicit resume token");
    auto resumed = context.set->resume(std::move(*token));
    requireError(
        resumed,
        Domain::ErrorCodes::LimitExceeded,
        "replacement issue failure was not returned by explicit resume");
    require(!token->valid(),
            "failed explicit resume did not consume its one-shot token");
    require(context.acceptApi->cancelCalls == 3U,
            "replacement failure did not cancel peer accepts");
    requireSnapshotCounts(
        context.set->snapshot(), 0U, 0U, 0U, 0U, 3U, 1U,
        "replacement failure did not close into bounded drain");
}

void cancellationFailureIsRetainedInResultSnapshotAndFullDiagnostics()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    context.acceptApi->failCancelCall = 1U;
    context.acceptApi->systemError = ERROR_ACCESS_DENIED;
    auto reaped = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        WSAENETDOWN));
    require(reaped.cancellationFailure() != nullptr &&
                reaped.cancellationFailure()->kind ==
                    Detail::DashboardAcceptLifecycleFailureKind::Unauthorized,
            "reap result discarded the first cancellation failure");
    require(context.acceptApi->cancelCalls == 3U,
            "one cancellation failure prevented remaining cancellation calls");
    const auto snapshot = context.set->snapshot();
    require(snapshot.lifecycleFailure() != nullptr &&
                snapshot.lifecycleFailure()->kind ==
                    Detail::DashboardAcceptLifecycleFailureKind::Unauthorized,
            "allocation-free snapshot discarded cancellation classification");
    const auto full = context.set->fullLifecycleFailure();
    require(full.has_value() &&
                full->code == Domain::ErrorCodes::Unauthorized,
            "full diagnostic did not retain the cancellation error");
    requireSnapshotCounts(
        snapshot, 1U, 0U, 0U, 0U, 2U, 1U,
        "cancellation failure lost outstanding slot ownership");
}

void concurrentCloseAndCompletionRemainBounded()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    OVERLAPPED* const operation =
        context.extensionState.uniqueOperations[0U];
    std::optional<std::optional<Detail::DashboardAcceptLifecycleFailure>>
        closeResult;
    std::optional<Domain::Result<ReapResult>> reapResult;
    std::thread closer{[&] {
        closeResult.emplace(
            context.set->closeAdmissionAndRequestCancellation());
    }};
    std::thread reaper{[&] {
        reapResult.emplace(context.set->reap(
            ListenerKey, 0U, operation, ERROR_SUCCESS));
    }};
    closer.join();
    reaper.join();
    require(closeResult.has_value() && !closeResult->has_value(),
            "concurrent close failed");
    require(reapResult.has_value() &&
                static_cast<bool>(*reapResult),
            "concurrent successful completion failed");
    const auto disposition = reapResult->value().disposition();
    require(disposition == ReapDisposition::AcceptedAndDrained ||
                disposition == ReapDisposition::AcceptedAndPaused,
            "concurrent close produced an invalid success disposition");
    if (disposition == ReapDisposition::AcceptedAndPaused) {
        auto token = reapResult->value().takeResumeToken();
        require(token.has_value(),
                "concurrent paused completion omitted its token");
        auto returned = context.set->resume(std::move(*token));
        require(static_cast<bool>(returned) &&
                    returned.value() ==
                        Detail::DashboardAcceptResumeDisposition::ReturnedAfterClose,
                "close did not drain the subsequently returned paused token");
    }
    require(!context.set->snapshot().admissionOpen(),
            "concurrent close/reap reopened admission");
    require(context.extensionState.issueCalls == 4U,
            "concurrent close/reap issued before caller-controlled resume");
}

void concurrentCloseAndDirectResumeLeaveNoTokenObligation()
{
    FakeContext context;
    require(static_cast<bool>(context.start()), "slot set did not start");
    auto accepted = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.uniqueOperations[0U],
        ERROR_SUCCESS));
    auto token = accepted.takeResumeToken();
    require(token.has_value() && token->valid(),
            "successful accept omitted its direct-resume token");
    auto connection = accepted.takeAcceptedConnection();
    connection.reset();

    std::optional<std::optional<Detail::DashboardAcceptLifecycleFailure>>
        closeResult;
    std::optional<Domain::Result<Detail::DashboardAcceptResumeDisposition>>
        resumeResult;
    std::thread closer{[&] {
        closeResult.emplace(
            context.set->closeAdmissionAndRequestCancellation());
    }};
    std::thread resumer{[&] {
        resumeResult.emplace(context.set->resume(std::move(*token)));
    }};
    closer.join();
    resumer.join();

    require(closeResult.has_value() && !closeResult->has_value(),
            "direct close-vs-resume close failed");
    require(resumeResult.has_value() &&
                static_cast<bool>(*resumeResult),
            "direct close-vs-resume token return failed");
    require(!token->valid(),
            "direct close-vs-resume left its token live");
    const auto disposition = resumeResult->value();
    require(disposition ==
                Detail::DashboardAcceptResumeDisposition::Reissued ||
                disposition ==
                    Detail::DashboardAcceptResumeDisposition::ReturnedAfterClose,
            "direct close-vs-resume produced an invalid disposition");

    const auto snapshot = context.set->snapshot();
    require(snapshot.pausedCount() == 0U &&
                snapshot.awaitingReturnCount() == 0U &&
                snapshot.outstandingResumeTokenCount() == 0U,
            "direct close-vs-resume left a token obligation");
    if (disposition == Detail::DashboardAcceptResumeDisposition::Reissued) {
        requireSnapshotCounts(
            snapshot, 0U, 0U, 0U, 0U, 4U, 0U,
            "resume-first race did not cancel four issued accepts");
        require(context.extensionState.issueCalls == 5U,
                "resume-first race did not issue exactly one replacement");
    } else {
        requireSnapshotCounts(
            snapshot, 0U, 0U, 0U, 0U, 3U, 1U,
            "close-first race did not drain the returned token slot");
        require(context.extensionState.issueCalls == 4U,
                "close-first race issued after admission closed");
    }
}

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
        const std::lock_guard lock{mutex_};
        if (count_ == completions_.size()) {
            overflow_ = true;
            return;
        }
        const std::size_t tail = (head_ + count_) % completions_.size();
        completions_[tail] = Completion{packet, nativeError};
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
        completion = completions_[head_];
        head_ = (head_ + 1U) % completions_.size();
        --count_;
        return true;
    }

    [[nodiscard]] bool overflowed() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return overflow_;
    }

    [[nodiscard]] bool fatal() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return fatalError_.has_value();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Completion, 16U> completions_{};
    std::size_t head_{};
    std::size_t count_{};
    bool overflow_{};
    std::optional<DWORD> fatalError_;
};

class RealSmokeComposition final {
public:
    explicit RealSmokeComposition(
        const std::string_view host,
        const int addressFamily,
        const std::uint16_t firstPort)
        : runtime{take(Runtime::create())},
          sink{std::make_shared<CollectingKernelSink>()},
          kernel{take(Kernel::create(take(Port::create()), sink))}
    {
        std::optional<Listener> listener;
        for (std::uint16_t offset{}; offset < 16U; ++offset) {
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
        if (!listener.has_value() || !endpoint.has_value()) {
            fail("bounded real-smoke port list had no available listener");
        }
        set = take(Set::create(
            *runtime,
            std::move(*listener),
            ListenerKey));
        const auto started = set->start(*kernel);
        if (!started) {
            fail(started.error().code + ": " + started.error().message);
        }
        client.emplace(take(runtime->createOverlappedTcpSocket(addressFamily)));
    }

    ~RealSmokeComposition() noexcept
    {
        if (set != nullptr) {
            static_cast<void>(set->closeAdmissionAndRequestCancellation());
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!set->snapshot().fullyDrained() &&
                   std::chrono::steady_clock::now() < deadline) {
                CollectingKernelSink::Completion completion{};
                if (!sink->waitTake(completion, std::chrono::milliseconds{250})) {
                    continue;
                }
                static_cast<void>(set->reap(
                    completion.packet.completionKey,
                    completion.packet.transferredBytes,
                    completion.packet.operation,
                    completion.nativeError));
            }
            if (!set->snapshot().fullyDrained()) {
                std::terminate();
            }
            set.reset();
        }
        client.reset();
        if (kernel != nullptr) {
            kernel->shutdown();
            kernel.reset();
        }
        runtime.reset();
    }

    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<CollectingKernelSink> sink;
    std::unique_ptr<Kernel> kernel;
    std::optional<Endpoint> endpoint;
    std::unique_ptr<Set> set;
    std::optional<Detail::UniqueDashboardSocket> client;
};

void realSocketSmoke(
    const std::string_view host,
    const int addressFamily,
    const std::uint16_t firstPort)
{
    RealSmokeComposition context{host, addressFamily, firstPort};
    require(context.endpoint.has_value(),
            "real smoke did not retain its exact endpoint");
    const int connectStatus = ::connect(
        context.client->get(),
        context.endpoint->nativeAddress(),
        context.endpoint->nativeAddressLength());
    require(connectStatus == 0,
            "real loopback client could not connect to the listener");

    CollectingKernelSink::Completion completion{};
    require(context.sink->waitTake(
                completion, std::chrono::seconds{5}),
            "real AcceptEx completion did not arrive");
    require(completion.nativeError == ERROR_SUCCESS,
            "real AcceptEx completed with a native failure");
    auto reaped = take(context.set->reap(
        completion.packet.completionKey,
        completion.packet.transferredBytes,
        completion.packet.operation,
        completion.nativeError));
    require(reaped.disposition() == ReapDisposition::AcceptedAndPaused,
            "real accepted socket did not pause for admission");
    auto accepted = reaped.takeAcceptedConnection();
    require(accepted.has_value() &&
                accepted->borrowedNativeSocket() != INVALID_SOCKET,
            "real AcceptEx did not hand off an owned accepted socket");
    require(static_cast<bool>(context.endpoint->validateBoundAddress(
                accepted->addresses().local().nativeAddress(),
                accepted->addresses().local().nativeAddressLength())),
            "real accepted local address did not match the listener");
    require(static_cast<bool>(context.endpoint->validatePeerAddress(
                accepted->addresses().remote().nativeAddress(),
                accepted->addresses().remote().nativeAddressLength())),
            "real accepted peer was not exact-family loopback");
    auto token = reaped.takeResumeToken();
    require(token.has_value(),
            "real accepted socket omitted its resume token");
    auto resumed = context.set->resume(std::move(*token));
    require(static_cast<bool>(resumed) &&
                resumed.value() ==
                    Detail::DashboardAcceptResumeDisposition::Reissued,
            "real accepted slot did not reissue after explicit resume");
    accepted.reset();
    context.client.reset();

    require(!context.set->closeAdmissionAndRequestCancellation().has_value(),
            "real listener close/cancel failed");
    for (std::size_t index{}; index < Set::SlotCount; ++index) {
        require(context.sink->waitTake(
                    completion, std::chrono::seconds{5}),
                "real cancelled AcceptEx completion did not drain");
        auto drained = context.set->reap(
            completion.packet.completionKey,
            completion.packet.transferredBytes,
            completion.packet.operation,
            completion.nativeError);
        require(static_cast<bool>(drained),
                "real cancelled AcceptEx packet was not reaped");
        require(drained.value().disposition() ==
                    ReapDisposition::FailureDrained,
                "real shutdown cancellation was reissued");
    }
    require(context.set->snapshot().fullyDrained(),
            "real listener did not drain all four operations");
    require(!context.sink->overflowed() && !context.sink->fatal(),
            "real smoke overflowed its bounded sink or hit fatal IOCP error");
}

void realIpv4CompositionSmoke()
{
    const auto base = static_cast<std::uint16_t>(
        52000U + (::GetCurrentProcessId() % 1000U));
    realSocketSmoke("127.0.0.1", AF_INET, base);
}

void realIpv6CompositionSmoke()
{
    const auto base = static_cast<std::uint16_t>(
        54000U + (::GetCurrentProcessId() % 1000U));
    realSocketSmoke("::1", AF_INET6, base);
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array<TestCase, 23U> TestCases{{
    {"factory exact-listener discovery",
     &factoryDiscoversExtensionsFromTheOwnedListener},
    {"associate before exactly four issues",
     &startAssociatesBeforeExactlyFourIssues},
    {"association failure",
     &associationFailureIssuesNothingAndDrainsIdleSlots},
    {"pre-start terminal close", &terminalCloseBeforeStartPreventsNativeAssociation},
    {"partial issue failure", &partialIssueFailureCancelsEveryIssuedSlotAndRetainsDrain},
    {"idempotent close", &closeIsIdempotentAndCancelsExactlyFour},
    {"successful paused same-slot resume", &successfulCompletionPausesUntilExactTokenReturns},
    {"cross-set token rejection", &crossSetTokenRejectionIsNonmutatingAndOriginCanResume},
    {"four withheld successful tokens", &fourSuccessfulAcceptsRemainPausedUntilAllTokensReturn},
    {"closed paused-token return", &closeWaitsForPausedTokenReturnWithoutNativeCancellation},
    {"cancel versus success", &closeRacingSuccessfulCompletionDrainsWithoutReplacementWhenCloseWins},
    {"retryable client failure", &retryableClientFailureReissuesTheExactSameSlot},
    {"listener failure retirement", &retryableListenerFailureClosesAdmissionInsteadOfSpinning},
    {"shutdown failure drain", &cancellationFailureDrainsWithoutReprime},
    {"unsolicited abort provenance", &unsolicitedAbortHasNoExactCancellationProvenance},
    {"listener force-close drain", &listenerForceCloseBoundsOneAndRepeatedCancellationFailures},
    {"misrouting rejection", &wrongKeyAndForeignPointerAreNonMutating},
    {"nonzero packet consume", &nonzeroBytePacketIsConsumedThenRetiresTheGeneration},
    {"explicit resume issue failure", &explicitResumeIssueFailurePreservesConnectionAndClosesAdmission},
    {"retained cancellation failure", &cancellationFailureIsRetainedInResultSnapshotAndFullDiagnostics},
    {"concurrent close and completion", &concurrentCloseAndCompletionRemainBounded},
    {"concurrent close and direct resume", &concurrentCloseAndDirectResumeLeaveNoTokenObligation},
    {"real IPv4 composition", &realIpv4CompositionSmoke},
}};

} // namespace

int main()
{
    try {
        for (const auto& test : TestCases) {
            test.run();
        }
        // IPv6 is a separate explicit smoke so a failure names the family.
        realIpv6CompositionSmoke();
        std::cout << "DashboardAcceptSlotSetTests passed "
                  << (TestCases.size() + 1U) << " cases with "
                  << assertionCount << " assertions.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DashboardAcceptSlotSetTests failed after "
                  << assertionCount << " assertions: "
                  << exception.what() << '\n';
        return 1;
    }
}
