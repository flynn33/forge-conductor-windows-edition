#include "Infrastructure/Windows/Detail/DashboardAcceptSlot.h"

#include <MSWSock.h>

#include <algorithm>
#include <array>
#include <atomic>
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

namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using AcceptedConnection = Detail::DashboardAcceptedConnection;
using CancellationDisposition =
    Detail::DashboardAcceptCancellationDisposition;
using ExtensionApi = Detail::IDashboardWinsockExtensionApi;
using Extensions = Detail::DashboardWinsockExtensions;
using IssueDisposition = Detail::DashboardAcceptIssueDisposition;
using Listener = Detail::DashboardListeningSocket;
using ListenerApi = Detail::IDashboardListeningSocketApi;
using Runtime = Detail::DashboardWinsockRuntime;
using Slot = Detail::DashboardAcceptSlot;
using SlotApi = Detail::IDashboardAcceptSlotApi;
using SlotState = Detail::DashboardAcceptSlotState;
using SystemSlotApi = Detail::DashboardAcceptSlotSystemApi;
using WinsockApi = Detail::IDashboardWinsockApi;

static_assert(std::is_abstract_v<SlotApi>);
static_assert(std::is_final_v<SystemSlotApi>);
static_assert(std::is_final_v<Slot>);
static_assert(!std::is_copy_constructible_v<Slot>);
static_assert(!std::is_move_constructible_v<Slot>);
static_assert(std::is_nothrow_destructible_v<Slot>);
static_assert(std::is_final_v<AcceptedConnection>);
static_assert(!std::is_copy_constructible_v<AcceptedConnection>);
static_assert(std::is_nothrow_move_constructible_v<AcceptedConnection>);
static_assert(std::is_nothrow_move_assignable_v<AcceptedConnection>);
static_assert(
    Slot::Ipv4AddressBufferLength ==
    2U * (static_cast<DWORD>(sizeof(sockaddr_in)) + 16U));
static_assert(
    Slot::Ipv6AddressBufferLength ==
    2U * (static_cast<DWORD>(sizeof(sockaddr_in6)) + 16U));
static_assert(
    Slot::MaximumAddressBufferLength == Slot::Ipv6AddressBufferLength);
static_assert(noexcept(Slot::create()));
static_assert(noexcept(Slot::create({})));

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
    if (result.error().retryable != retryable) {
        fail(std::string{context} + " used the wrong retryability");
    }
    ++assertionCount;
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

enum class Event : std::uint8_t {
    AcceptEx,
    UpdateAcceptContext,
    ExtractAddresses,
    CancelAccept,
};

class Recorder final {
public:
    void record(const Event event) noexcept
    {
        const std::scoped_lock lock{mutex_};
        if (count_ < events_.size()) {
            events_[count_++] = event;
        }
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return count_;
    }

    [[nodiscard]] std::size_t indexOf(const Event searched) const
    {
        const std::scoped_lock lock{mutex_};
        for (std::size_t index{}; index < count_; ++index) {
            if (events_[index] == searched) {
                return index;
            }
        }
        fail("expected event was not recorded");
    }

private:
    mutable std::mutex mutex_;
    std::array<Event, 64U> events_{};
    std::size_t count_{};
};

class FakeWinsockApi final : public WinsockApi {
public:
    [[nodiscard]] int startup(
        const WORD requestedVersion,
        WSADATA& data) noexcept override
    {
        requestedVersion_ = requestedVersion;
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
        const std::size_t index = createCalls++;
        if (index < createdSockets.size()) {
            createdFamilies[index] = addressFamily;
            createdTypes[index] = socketType;
            createdProtocols[index] = protocol;
            createdFlags[index] = flags;
            createdSockets[index] =
                static_cast<SOCKET>(500U + index);
            return createdSockets[index];
        }
        return INVALID_SOCKET;
    }

    [[nodiscard]] int closeSocket(const SOCKET socket) noexcept override
    {
        if (closeCalls < closedSockets.size()) {
            closedSockets[closeCalls] = socket;
        }
        ++closeCalls;
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        ++lastErrorCalls;
        return WSAENOBUFS;
    }

    [[nodiscard]] bool wasClosed(const SOCKET socket) const noexcept
    {
        return std::find(
                   closedSockets.begin(),
                   closedSockets.begin() +
                       static_cast<std::ptrdiff_t>(
                           (std::min)(closeCalls, closedSockets.size())),
                   socket) !=
            closedSockets.begin() +
                static_cast<std::ptrdiff_t>(
                    (std::min)(closeCalls, closedSockets.size()));
    }

    [[nodiscard]] WORD requestedVersion() const noexcept
    {
        return requestedVersion_;
    }

    std::array<SOCKET, 64U> createdSockets{};
    std::array<int, 64U> createdFamilies{};
    std::array<int, 64U> createdTypes{};
    std::array<int, 64U> createdProtocols{};
    std::array<DWORD, 64U> createdFlags{};
    std::array<SOCKET, 64U> closedSockets{};
    std::size_t createCalls{};
    std::size_t closeCalls{};
    std::size_t cleanupCalls{};
    std::size_t lastErrorCalls{};

private:
    WORD requestedVersion_{};
};

class FakeListenerApi final : public ListenerApi {
public:
    explicit FakeListenerApi(
        const Detail::DashboardLoopbackEndpoint& endpoint) noexcept
        : addressLength_{endpoint.nativeAddressLength()}
    {
        std::memcpy(
            &address_,
            endpoint.nativeAddress(),
            static_cast<std::size_t>(addressLength_));
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
        sockaddr* address,
        int& addressLength) noexcept override
    {
        if (address == nullptr || addressLength < addressLength_) {
            return SOCKET_ERROR;
        }
        std::memcpy(address, &address_, static_cast<std::size_t>(addressLength_));
        addressLength = addressLength_;
        return 0;
    }
    [[nodiscard]] int listenSocket(SOCKET, int) noexcept override { return 0; }
    [[nodiscard]] int lastError() noexcept override { return WSAEFAULT; }

private:
    sockaddr_storage address_{};
    int addressLength_{};
};

enum class AddressMode : std::uint8_t {
    Valid,
    WrongLocalPort,
    WrongLocalAddress,
    WrongRemoteAddress,
    ZeroRemotePort,
    WrongRemoteFamily,
};

struct NativeState final {
    std::shared_ptr<Recorder> recorder;
    BOOL acceptResult{TRUE};
    std::size_t acceptCalls{};
    SOCKET listenerSocket{INVALID_SOCKET};
    SOCKET acceptedSocket{INVALID_SOCKET};
    void* buffer{};
    DWORD receiveLength{};
    DWORD localLength{};
    DWORD remoteLength{};
    OVERLAPPED* operation{};
    Slot* publishedSlot{};
    SlotState observedState{SlotState::Idle};
    bool observedStableOperation{};
    std::size_t extractCalls{};
    int addressFamily{AF_INET};
    std::uint16_t localPort{};
    AddressMode addressMode{AddressMode::Valid};
};

NativeState* activeNativeState{};

void setNetworkPort(
    unsigned short& networkPort,
    const std::uint16_t hostPort) noexcept
{
    auto* const bytes = reinterpret_cast<unsigned char*>(&networkPort);
    bytes[0U] = static_cast<unsigned char>(hostPort >> 8U);
    bytes[1U] = static_cast<unsigned char>(hostPort & 0xffU);
}

void populateIpv4(
    sockaddr* const destination,
    const bool local,
    const NativeState& state) noexcept
{
    sockaddr_in value{};
    value.sin_family = AF_INET;
    const std::uint16_t port = local ? state.localPort : 49152U;
    setNetworkPort(value.sin_port, port);
    std::array<unsigned char, 4U> bytes{127U, 0U, 0U, 1U};
    if (local && state.addressMode == AddressMode::WrongLocalAddress) {
        bytes[3U] = 2U;
    }
    if (!local && state.addressMode == AddressMode::WrongRemoteAddress) {
        bytes[3U] = 2U;
    }
    if (local && state.addressMode == AddressMode::WrongLocalPort) {
        setNetworkPort(value.sin_port, static_cast<std::uint16_t>(port + 1U));
    }
    if (!local && state.addressMode == AddressMode::ZeroRemotePort) {
        value.sin_port = 0U;
    }
    std::memcpy(&value.sin_addr, bytes.data(), bytes.size());
    if (!local && state.addressMode == AddressMode::WrongRemoteFamily) {
        value.sin_family = AF_INET6;
    }
    std::memcpy(destination, &value, sizeof(value));
}

void populateIpv6(
    sockaddr* const destination,
    const bool local,
    const NativeState& state) noexcept
{
    sockaddr_in6 value{};
    value.sin6_family = AF_INET6;
    const std::uint16_t port = local ? state.localPort : 49153U;
    setNetworkPort(value.sin6_port, port);
    auto* const bytes = reinterpret_cast<unsigned char*>(&value.sin6_addr);
    bytes[15U] = 1U;
    if (local && state.addressMode == AddressMode::WrongLocalAddress) {
        bytes[15U] = 2U;
    }
    if (!local && state.addressMode == AddressMode::WrongRemoteAddress) {
        bytes[15U] = 2U;
    }
    if (local && state.addressMode == AddressMode::WrongLocalPort) {
        setNetworkPort(value.sin6_port, static_cast<std::uint16_t>(port + 1U));
    }
    if (!local && state.addressMode == AddressMode::ZeroRemotePort) {
        value.sin6_port = 0U;
    }
    if (!local && state.addressMode == AddressMode::WrongRemoteFamily) {
        value.sin6_family = AF_INET;
    }
    std::memcpy(destination, &value, sizeof(value));
}

BOOL PASCAL fakeAcceptEx(
    const SOCKET listenerSocket,
    const SOCKET acceptedSocket,
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength,
    LPDWORD const bytesReceived,
    LPOVERLAPPED const operation)
{
    if (activeNativeState == nullptr) {
        return FALSE;
    }
    auto& state = *activeNativeState;
    if (state.recorder != nullptr) {
        state.recorder->record(Event::AcceptEx);
    }
    ++state.acceptCalls;
    state.listenerSocket = listenerSocket;
    state.acceptedSocket = acceptedSocket;
    state.buffer = outputBuffer;
    state.receiveLength = receiveDataLength;
    state.localLength = localAddressLength;
    state.remoteLength = remoteAddressLength;
    state.operation = operation;
    if (bytesReceived != nullptr) {
        *bytesReceived = 0U;
    }
    if (operation != nullptr) {
        operation->Internal = static_cast<ULONG_PTR>(0x5a5aU);
    }
    if (state.publishedSlot != nullptr) {
        state.observedState = state.publishedSlot->state();
        state.observedStableOperation =
            operation == state.publishedSlot->borrowedOperation();
    }
    return state.acceptResult;
}

VOID PASCAL fakeGetAcceptExSockaddrs(
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength,
    LPSOCKADDR* const localAddress,
    LPINT const localAddressLengthActual,
    LPSOCKADDR* const remoteAddress,
    LPINT const remoteAddressLengthActual)
{
    if (activeNativeState == nullptr) {
        return;
    }
    auto& state = *activeNativeState;
    if (state.recorder != nullptr) {
        state.recorder->record(Event::ExtractAddresses);
    }
    ++state.extractCalls;
    auto* const bytes = static_cast<std::byte*>(outputBuffer);
    auto* const local = reinterpret_cast<sockaddr*>(
        bytes + receiveDataLength + 16U);
    auto* const remote = reinterpret_cast<sockaddr*>(
        bytes + receiveDataLength + localAddressLength + 16U);
    if (state.addressFamily == AF_INET) {
        populateIpv4(local, true, state);
        populateIpv4(remote, false, state);
        *localAddressLengthActual = static_cast<int>(sizeof(sockaddr_in));
        *remoteAddressLengthActual = static_cast<int>(sizeof(sockaddr_in));
    } else {
        populateIpv6(local, true, state);
        populateIpv6(remote, false, state);
        *localAddressLengthActual = static_cast<int>(sizeof(sockaddr_in6));
        *remoteAddressLengthActual = static_cast<int>(sizeof(sockaddr_in6));
    }
    *localAddress = local;
    *remoteAddress = remote;
    static_cast<void>(remoteAddressLength);
}

class FakeExtensionApi final : public ExtensionApi {
public:
    [[nodiscard]] int ioctl(
        SOCKET,
        DWORD,
        const void*,
        DWORD,
        void* const outputBuffer,
        const DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept override
    {
        const std::size_t index = queryCalls++;
        if (index == 0U) {
            const LPFN_ACCEPTEX pointer = &fakeAcceptEx;
            if (outputBufferLength != sizeof(pointer)) {
                return SOCKET_ERROR;
            }
            std::memcpy(outputBuffer, &pointer, sizeof(pointer));
            bytesReturned = static_cast<DWORD>(sizeof(pointer));
            return 0;
        }
        const LPFN_GETACCEPTEXSOCKADDRS pointer =
            &fakeGetAcceptExSockaddrs;
        if (outputBufferLength != sizeof(pointer)) {
            return SOCKET_ERROR;
        }
        std::memcpy(outputBuffer, &pointer, sizeof(pointer));
        bytesReturned = static_cast<DWORD>(sizeof(pointer));
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        ++lastErrorCalls;
        return nativeError;
    }

    std::size_t queryCalls{};
    std::size_t lastErrorCalls{};
    int nativeError{ERROR_IO_PENDING};
};

class FakeSlotApi final : public SlotApi {
public:
    explicit FakeSlotApi(std::shared_ptr<Recorder> recorder) noexcept
        : recorder_{std::move(recorder)}
    {
    }

    [[nodiscard]] int updateAcceptContext(
        const SOCKET acceptedSocket,
        const SOCKET listenerSocket) noexcept override
    {
        recorder_->record(Event::UpdateAcceptContext);
        ++updateCalls;
        updatedAcceptedSocket = acceptedSocket;
        updatedListenerSocket = listenerSocket;
        return updateStatus;
    }

    [[nodiscard]] BOOL cancelAccept(
        const SOCKET listenerSocket,
        OVERLAPPED* const operation) noexcept override
    {
        recorder_->record(Event::CancelAccept);
        std::unique_lock lock{cancelMutex_};
        ++cancelCalls;
        cancelledListenerSocket = listenerSocket;
        cancelledOperation = operation;
        if (blockCancellation_) {
            cancellationEntered_ = true;
            cancelCondition_.notify_all();
            cancelCondition_.wait(lock, [this]() noexcept {
                return releaseCancellation_;
            });
        }
        return cancelResult;
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

    void beginBlockingCancellation()
    {
        const std::scoped_lock lock{cancelMutex_};
        blockCancellation_ = true;
        cancellationEntered_ = false;
        releaseCancellation_ = false;
    }

    void waitForCancellationEntry()
    {
        std::unique_lock lock{cancelMutex_};
        cancelCondition_.wait(lock, [this]() noexcept {
            return cancellationEntered_;
        });
    }

    void releaseCancellation()
    {
        const std::scoped_lock lock{cancelMutex_};
        releaseCancellation_ = true;
        blockCancellation_ = false;
        cancelCondition_.notify_all();
    }

    std::size_t updateCalls{};
    SOCKET updatedAcceptedSocket{INVALID_SOCKET};
    SOCKET updatedListenerSocket{INVALID_SOCKET};
    int updateStatus{};
    int socketError{WSAEINVAL};
    std::size_t lastSocketErrorCalls{};
    std::size_t cancelCalls{};
    SOCKET cancelledListenerSocket{INVALID_SOCKET};
    OVERLAPPED* cancelledOperation{};
    BOOL cancelResult{TRUE};
    DWORD systemError{ERROR_GEN_FAILURE};
    std::size_t lastSystemErrorCalls{};

private:
    std::shared_ptr<Recorder> recorder_;
    std::mutex cancelMutex_;
    std::condition_variable cancelCondition_;
    bool blockCancellation_{};
    bool cancellationEntered_{};
    bool releaseCancellation_{};
};

class ActiveNativeScope final {
public:
    explicit ActiveNativeScope(NativeState& state) noexcept
    {
        activeNativeState = &state;
    }
    ~ActiveNativeScope() noexcept { activeNativeState = nullptr; }

    ActiveNativeScope(const ActiveNativeScope&) = delete;
    ActiveNativeScope& operator=(const ActiveNativeScope&) = delete;
};

class Context final {
public:
    Context(const std::string_view host, const std::uint16_t port)
        : endpoint{take(Detail::DashboardLoopbackEndpoint::create(host, port))},
          recorder{std::make_shared<Recorder>()},
          winsock{std::make_shared<FakeWinsockApi>()},
          runtime{take(Runtime::create(winsock))},
          listenerApi{std::make_shared<FakeListenerApi>(endpoint)},
          extensionApi{std::make_shared<FakeExtensionApi>()},
          slotApi{std::make_shared<FakeSlotApi>(recorder)}
    {
        listener.emplace(take(Listener::create(
            *runtime, endpoint, listenerApi)));
        extensions = take(Extensions::discover(
            listener->borrowedNativeSocket(), extensionApi));
    }

    [[nodiscard]] NativeState nativeState() const
    {
        NativeState state{};
        state.recorder = recorder;
        state.addressFamily =
            endpoint.addressFamily() ==
                    Detail::DashboardLoopbackAddressFamily::Ipv4
                ? AF_INET
                : AF_INET6;
        state.localPort = endpoint.port();
        return state;
    }

    Detail::DashboardLoopbackEndpoint endpoint;
    std::shared_ptr<Recorder> recorder;
    std::shared_ptr<FakeWinsockApi> winsock;
    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<FakeListenerApi> listenerApi;
    std::optional<Listener> listener;
    std::shared_ptr<FakeExtensionApi> extensionApi;
    std::unique_ptr<Extensions> extensions;
    std::shared_ptr<FakeSlotApi> slotApi;
};

[[nodiscard]] std::unique_ptr<Slot> slot(
    const std::shared_ptr<FakeSlotApi>& api)
{
    return take(Slot::create(api));
}

void requireIdleAndZeroed(const Slot& acceptSlot)
{
    require(acceptSlot.state() == SlotState::Idle,
            "slot was not idle after completion reaping");
    require(acceptSlot.activeAddressBufferLength() == 0U,
            "idle slot retained an active address-buffer length");
    require(bytesAreZero(
                const_cast<Slot&>(acceptSlot).borrowedOperation(),
                sizeof(OVERLAPPED)),
            "idle slot did not zero its stable OVERLAPPED");
}

void factoryCreatesStableZeroedIdleStorage()
{
    requireError(
        Slot::create(std::shared_ptr<SlotApi>{}),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "accept slot accepted a null native API");

    auto api = std::make_shared<FakeSlotApi>(std::make_shared<Recorder>());
    auto acceptSlot = slot(api);
    require(acceptSlot != nullptr, "slot factory returned null ownership");
    OVERLAPPED* const operation = acceptSlot->borrowedOperation();
    require(operation != nullptr, "slot did not expose stable OVERLAPPED");
    require(bytesAreZero(operation, sizeof(*operation)),
            "new slot OVERLAPPED was not zeroed");
    requireIdleAndZeroed(*acceptSlot);
    const auto cancellation = acceptSlot->requestCancellation();
    requireError(
        cancellation,
        Domain::ErrorCodes::Conflict,
        false,
        "idle slot accepted cancellation");
    require(api->cancelCalls == 0U,
            "idle cancellation reached CancelIoEx");
}

void synchronousIpv4IssuePublishesBeforeNativeAndTransfersOwnership()
{
    Context context{"127.0.0.1", 18101U};
    auto acceptSlot = slot(context.slotApi);
    OVERLAPPED* const stableOperation = acceptSlot->borrowedOperation();
    NativeState native = context.nativeState();
    native.publishedSlot = acceptSlot.get();
    ActiveNativeScope active{native};

    const auto issued = acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener);
    require(static_cast<bool>(issued), "synchronous IPv4 issue failed");
    require(issued.value() == IssueDisposition::CompletedSynchronously,
            "synchronous AcceptEx was reported pending");
    require(native.observedState == SlotState::Issued,
            "slot state was not published before AcceptEx invocation");
    require(native.observedStableOperation,
            "AcceptEx did not receive the stable slot OVERLAPPED");
    require(acceptSlot->state() == SlotState::Issued,
            "issued slot did not retain issued state");
    require(acceptSlot->activeAddressBufferLength() ==
                Slot::Ipv4AddressBufferLength,
            "IPv4 slot used the wrong fixed buffer length");
    require(native.receiveLength == 0U,
            "AcceptEx issue waited for request payload");
    require(native.localLength == Extensions::Ipv4AddressRegionLength &&
                native.remoteLength ==
                    Extensions::Ipv4AddressRegionLength,
            "IPv4 issue changed sockaddr-plus-16 region lengths");
    require(native.listenerSocket ==
                context.listener->borrowedNativeSocket(),
            "AcceptEx issue changed the listener socket");
    require(context.winsock->createCalls == 2U,
            "slot did not create exactly one accepted socket");
    require(context.winsock->createdFamilies[1U] == AF_INET &&
                context.winsock->createdTypes[1U] == SOCK_STREAM &&
                context.winsock->createdProtocols[1U] == IPPROTO_TCP,
            "slot did not create an IPv4 TCP stream socket");
    require(context.winsock->createdFlags[1U] ==
                (WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT),
            "accepted socket omitted overlapped or non-inheritable flags");
    require(stableOperation->Internal == static_cast<ULONG_PTR>(0x5a5aU),
            "issued OVERLAPPED was reset before completion");

    const std::size_t createsBefore = context.winsock->createCalls;
    const auto prematureReuse = acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener);
    requireError(
        prematureReuse,
        Domain::ErrorCodes::Conflict,
        false,
        "issued slot allowed reuse before reaping");
    require(context.winsock->createCalls == createsBefore &&
                native.acceptCalls == 1U,
            "premature reuse created or issued another socket");

    const SOCKET acceptedSocket = native.acceptedSocket;
    std::optional<AcceptedConnection> connection;
    {
        auto completed = acceptSlot->reapSuccessful(
            *context.extensions, stableOperation);
        require(static_cast<bool>(completed),
                "valid IPv4 completion failed");
        connection.emplace(std::move(completed).value());
    }
    require(context.slotApi->updateCalls == 1U,
            "completion did not apply accept context exactly once");
    require(context.slotApi->updatedAcceptedSocket == acceptedSocket &&
                context.slotApi->updatedListenerSocket ==
                    context.listener->borrowedNativeSocket(),
            "SO_UPDATE_ACCEPT_CONTEXT used the wrong socket pair");
    require(native.extractCalls == 1U,
            "completion did not extract addresses exactly once");
    require(context.recorder->indexOf(Event::UpdateAcceptContext) <
                context.recorder->indexOf(Event::ExtractAddresses),
            "addresses were extracted before SO_UPDATE_ACCEPT_CONTEXT");
    require(connection->borrowedNativeSocket() == acceptedSocket,
            "completion did not transfer accepted-socket ownership");
    require(connection->addresses().local().addressFamily() == AF_INET &&
                connection->addresses().remote().addressFamily() == AF_INET,
            "completion changed copied IPv4 address families");
    const auto bufferBegin = reinterpret_cast<std::uintptr_t>(native.buffer);
    const auto bufferEnd = bufferBegin + Slot::Ipv4AddressBufferLength;
    for (const auto* const address : {
             connection->addresses().local().nativeAddress(),
             connection->addresses().remote().nativeAddress()}) {
        const auto copied = reinterpret_cast<std::uintptr_t>(address);
        require(copied < bufferBegin || copied >= bufferEnd,
                "accepted address aliases reusable slot storage");
    }
    requireIdleAndZeroed(*acceptSlot);
    require(bytesAreZero(native.buffer, Slot::Ipv4AddressBufferLength),
            "reaped slot retained bytes in its address buffer");
    require(acceptSlot->borrowedOperation() == stableOperation,
            "slot reuse changed its stable OVERLAPPED address");
    require(!context.winsock->wasClosed(acceptedSocket),
            "slot closed ownership transferred to the connection result");
    connection.reset();
    require(context.winsock->wasClosed(acceptedSocket),
            "accepted connection destruction did not close exactly once");

    native.acceptResult = FALSE;
    context.extensionApi->nativeError = ERROR_IO_PENDING;
    const auto reissued = acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener);
    require(static_cast<bool>(reissued), "re-primed slot issue failed");
    const auto resetOutcome = acceptSlot->reapFailed(
        stableOperation, ERROR_CONNECTION_ABORTED);
    requireError(
        resetOutcome,
        Domain::ErrorCodes::TransportClosed,
        true,
        "per-client connection abort was not retryable");
    requireIdleAndZeroed(*acceptSlot);
}

void pendingIpv6IssueUsesExactFamilyFraming()
{
    Context context{"::1", 18102U};
    auto acceptSlot = slot(context.slotApi);
    NativeState native = context.nativeState();
    native.acceptResult = FALSE;
    ActiveNativeScope active{native};
    context.extensionApi->nativeError = ERROR_IO_PENDING;

    const auto issued = acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener);
    require(static_cast<bool>(issued), "pending IPv6 issue failed");
    require(issued.value() == IssueDisposition::Pending,
            "ERROR_IO_PENDING was not retained as an issued operation");
    require(context.extensionApi->lastErrorCalls == 1U,
            "pending AcceptEx did not read exactly one native error");
    require(acceptSlot->activeAddressBufferLength() ==
                Slot::Ipv6AddressBufferLength,
            "IPv6 slot used the wrong fixed buffer length");
    require(native.receiveLength == 0U &&
                native.localLength == Extensions::Ipv6AddressRegionLength &&
                native.remoteLength == Extensions::Ipv6AddressRegionLength,
            "IPv6 issue changed exact zero-receive framing");
    require(context.winsock->createdFamilies[1U] == AF_INET6,
            "IPv6 slot created an accepted socket in the wrong family");

    auto completed = acceptSlot->reapSuccessful(
        *context.extensions, acceptSlot->borrowedOperation());
    require(static_cast<bool>(completed),
            "valid IPv6 completion failed");
    require(completed.value().addresses().local().addressFamily() == AF_INET6 &&
                completed.value().addresses().remote().addressFamily() == AF_INET6,
            "IPv6 completion changed copied address families");
    requireIdleAndZeroed(*acceptSlot);
}

void immediateAcceptFailureClosesAndRestoresIdleState()
{
    Context context{"127.0.0.1", 18103U};
    auto acceptSlot = slot(context.slotApi);
    NativeState native = context.nativeState();
    native.acceptResult = FALSE;
    native.publishedSlot = acceptSlot.get();
    ActiveNativeScope active{native};
    context.extensionApi->nativeError = WSAECONNRESET;

    const auto issued = acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener);
    requireError(
        issued,
        Domain::ErrorCodes::TransportClosed,
        true,
        "immediate AcceptEx failure returned an issued slot");
    require(native.observedState == SlotState::Issued,
            "immediate native call did not see fully published state");
    require(context.winsock->wasClosed(native.acceptedSocket),
            "immediate AcceptEx failure did not close accepted socket");
    require(context.slotApi->updateCalls == 0U && native.extractCalls == 0U,
            "immediate issue failure reached completion processing");
    requireIdleAndZeroed(*acceptSlot);
}

void updateContextFailuresAreTypedAndReaped()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSA_OPERATION_ABORTED, Domain::ErrorCodes::Cancelled, false},
        Case{WSAENOTSOCK, Domain::ErrorCodes::TransportClosed, false},
        Case{WSAESHUTDOWN, Domain::ErrorCodes::TransportClosed, false},
        Case{WSAECONNRESET, Domain::ErrorCodes::TransportClosed, true},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENOPROTOOPT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEOPNOTSUPP,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEINVAL, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 18104U};
        auto acceptSlot = slot(context.slotApi);
        NativeState native = context.nativeState();
        ActiveNativeScope active{native};
        static_cast<void>(take(acceptSlot->issue(
            *context.runtime, *context.extensions, *context.listener)));
        context.slotApi->updateStatus = SOCKET_ERROR;
        context.slotApi->socketError = testCase.nativeCode;
        const auto completed = acceptSlot->reapSuccessful(
            *context.extensions, acceptSlot->borrowedOperation());
        requireError(
            completed,
            testCase.stableCode,
            testCase.retryable,
            "failed accept-context update returned a connection");
        require(context.slotApi->lastSocketErrorCalls == 1U,
                "failed accept-context update did not read one error");
        require(native.extractCalls == 0U,
                "failed accept-context update still extracted addresses");
        require(context.winsock->wasClosed(native.acceptedSocket),
                "failed accept-context update did not close accepted socket");
        requireIdleAndZeroed(*acceptSlot);
    }
}

void exactListenerAndPeerValidationRejectsMismatches()
{
    struct Case final {
        AddressMode mode;
        std::string_view stableCode;
    };
    constexpr std::array cases{
        Case{AddressMode::WrongLocalPort,
             Domain::ErrorCodes::IntegrityFailure},
        Case{AddressMode::WrongLocalAddress,
             Domain::ErrorCodes::IntegrityFailure},
        Case{AddressMode::WrongRemoteAddress,
             Domain::ErrorCodes::Unauthorized},
        Case{AddressMode::ZeroRemotePort,
             Domain::ErrorCodes::Unauthorized},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 18105U};
        auto acceptSlot = slot(context.slotApi);
        NativeState native = context.nativeState();
        native.addressMode = testCase.mode;
        ActiveNativeScope active{native};
        static_cast<void>(take(acceptSlot->issue(
            *context.runtime, *context.extensions, *context.listener)));
        const auto completed = acceptSlot->reapSuccessful(
            *context.extensions, acceptSlot->borrowedOperation());
        requireError(
            completed,
            testCase.stableCode,
            false,
            "completion accepted mismatched local or peer endpoint");
        require(context.slotApi->updateCalls == 1U &&
                    native.extractCalls == 1U,
                "endpoint mismatch skipped context update or extraction");
        require(context.winsock->wasClosed(native.acceptedSocket),
                "endpoint mismatch retained accepted socket");
        requireIdleAndZeroed(*acceptSlot);
    }

    Context ipv6{"::1", 18106U};
    auto acceptSlot = slot(ipv6.slotApi);
    NativeState native = ipv6.nativeState();
    native.addressMode = AddressMode::WrongRemoteAddress;
    ActiveNativeScope active{native};
    static_cast<void>(take(acceptSlot->issue(
        *ipv6.runtime, *ipv6.extensions, *ipv6.listener)));
    const auto completed = acceptSlot->reapSuccessful(
        *ipv6.extensions, acceptSlot->borrowedOperation());
    requireError(
        completed,
        Domain::ErrorCodes::Unauthorized,
        false,
        "IPv6 non-loopback peer was accepted");
    requireIdleAndZeroed(*acceptSlot);
}

void wrongOverlappedNeverReapsOrReleasesOwnership()
{
    Context context{"127.0.0.1", 18107U};
    auto acceptSlot = slot(context.slotApi);
    NativeState native = context.nativeState();
    ActiveNativeScope active{native};
    static_cast<void>(take(acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener)));
    OVERLAPPED foreign{};

    const auto wrongSuccess = acceptSlot->reapSuccessful(
        *context.extensions, &foreign);
    requireError(
        wrongSuccess,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "slot accepted a foreign successful OVERLAPPED");
    const auto wrongFailure = acceptSlot->reapFailed(
        &foreign, ERROR_OPERATION_ABORTED);
    requireError(
        wrongFailure,
        Domain::ErrorCodes::IntegrityFailure,
        false,
        "slot accepted a foreign failed OVERLAPPED");
    require(acceptSlot->state() == SlotState::Issued,
            "foreign completion changed slot state");
    require(context.slotApi->updateCalls == 0U && native.extractCalls == 0U,
            "foreign completion reached post-processing");
    require(!context.winsock->wasClosed(native.acceptedSocket),
            "foreign completion released accepted socket");

    auto completed = acceptSlot->reapSuccessful(
        *context.extensions, acceptSlot->borrowedOperation());
    require(static_cast<bool>(completed),
            "exact completion failed after foreign rejection");
    requireIdleAndZeroed(*acceptSlot);
}

void cancellationRetainsStorageAcrossBothRaceOutcomes()
{
    Context context{"127.0.0.1", 18108U};
    auto acceptSlot = slot(context.slotApi);
    NativeState native = context.nativeState();
    native.acceptResult = FALSE;
    ActiveNativeScope active{native};
    context.extensionApi->nativeError = ERROR_IO_PENDING;

    static_cast<void>(take(acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener)));
    OVERLAPPED* const operation = acceptSlot->borrowedOperation();
    const SOCKET firstAccepted = native.acceptedSocket;
    const ULONG_PTR issuedMarker = operation->Internal;
    const auto requested = acceptSlot->requestCancellation();
    require(static_cast<bool>(requested) &&
                requested.value() == CancellationDisposition::Requested,
            "CancelIoEx success did not report requested");
    require(acceptSlot->state() == SlotState::CancellationRequested,
            "cancellation request did not change typed state");
    require(context.slotApi->cancelledListenerSocket ==
                context.listener->borrowedNativeSocket() &&
                context.slotApi->cancelledOperation == operation,
            "CancelIoEx used the wrong listener or OVERLAPPED");
    require(operation->Internal == issuedMarker &&
                acceptSlot->activeAddressBufferLength() ==
                    Slot::Ipv4AddressBufferLength,
            "cancellation request reset live operation storage");
    require(!context.winsock->wasClosed(firstAccepted),
            "cancellation request closed accepted socket before reaping");
    const auto repeated = acceptSlot->requestCancellation();
    require(static_cast<bool>(repeated) &&
                repeated.value() ==
                    CancellationDisposition::AlreadyRequested,
            "repeated cancellation was not coalesced");
    require(context.slotApi->cancelCalls == 1U,
            "repeated cancellation called CancelIoEx again");

    auto normalCompletion = acceptSlot->reapSuccessful(
        *context.extensions, operation);
    require(static_cast<bool>(normalCompletion),
            "normal completion lost a cancellation race");
    require(normalCompletion.value().borrowedNativeSocket() == firstAccepted,
            "normal race completion lost accepted ownership");
    requireIdleAndZeroed(*acceptSlot);

    static_cast<void>(take(acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener)));
    context.slotApi->cancelResult = FALSE;
    context.slotApi->systemError = ERROR_NOT_FOUND;
    const SOCKET secondAccepted = native.acceptedSocket;
    const auto completionWon = acceptSlot->requestCancellation();
    require(static_cast<bool>(completionWon) &&
                completionWon.value() ==
                    CancellationDisposition::CompletionMayHaveWon,
            "ERROR_NOT_FOUND was not treated as a completion race");
    require(acceptSlot->state() == SlotState::CancellationRequested &&
                !context.winsock->wasClosed(secondAccepted),
            "ERROR_NOT_FOUND released operation ownership");
    auto racedCompletion = acceptSlot->reapSuccessful(
        *context.extensions, acceptSlot->borrowedOperation());
    require(static_cast<bool>(racedCompletion),
            "completion-race success could not be reaped");
    requireIdleAndZeroed(*acceptSlot);

    static_cast<void>(take(acceptSlot->issue(
        *context.runtime, *context.extensions, *context.listener)));
    context.slotApi->cancelResult = TRUE;
    const auto finalCancel = acceptSlot->requestCancellation();
    require(static_cast<bool>(finalCancel), "final cancellation request failed");
    const auto aborted = acceptSlot->reapFailed(
        acceptSlot->borrowedOperation(), ERROR_OPERATION_ABORTED);
    requireError(
        aborted,
        Domain::ErrorCodes::Cancelled,
        false,
        "aborted cancellation completion used the wrong outcome");
    requireIdleAndZeroed(*acceptSlot);
}

void cancellationFailuresLeaveTheIssuedOperationReapable()
{
    struct Case final {
        DWORD nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{ERROR_INVALID_HANDLE,
             Domain::ErrorCodes::TransportClosed, false},
        Case{ERROR_ACCESS_DENIED, Domain::ErrorCodes::Unauthorized, false},
        Case{ERROR_NOT_ENOUGH_MEMORY,
             Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_GEN_FAILURE, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 18109U};
        auto acceptSlot = slot(context.slotApi);
        NativeState native = context.nativeState();
        native.acceptResult = FALSE;
        ActiveNativeScope active{native};
        context.extensionApi->nativeError = ERROR_IO_PENDING;
        static_cast<void>(take(acceptSlot->issue(
            *context.runtime, *context.extensions, *context.listener)));
        context.slotApi->cancelResult = FALSE;
        context.slotApi->systemError = testCase.nativeCode;
        const auto cancellation = acceptSlot->requestCancellation();
        requireError(
            cancellation,
            testCase.stableCode,
            testCase.retryable,
            "CancelIoEx failure returned a disposition");
        require(acceptSlot->state() == SlotState::Issued,
                "failed cancellation changed issued state");
        require(!context.winsock->wasClosed(native.acceptedSocket),
                "failed cancellation released accepted socket");
        const auto reaped = acceptSlot->reapFailed(
            acceptSlot->borrowedOperation(), ERROR_OPERATION_ABORTED);
        requireError(
            reaped,
            Domain::ErrorCodes::Cancelled,
            false,
            "operation could not be reaped after cancellation failure");
        requireIdleAndZeroed(*acceptSlot);
    }
}

void failedCompletionMappingsSeparateClientAndListenerOutcomes()
{
    struct Case final {
        DWORD nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{ERROR_OPERATION_ABORTED,
             Domain::ErrorCodes::Cancelled, false},
        Case{ERROR_NETNAME_DELETED,
             Domain::ErrorCodes::TransportClosed, true},
        Case{ERROR_CONNECTION_ABORTED,
             Domain::ErrorCodes::TransportClosed, true},
        Case{WSAECONNRESET, Domain::ErrorCodes::TransportClosed, true},
        Case{WSAENOTSOCK, Domain::ErrorCodes::TransportClosed, false},
        Case{WSAESHUTDOWN, Domain::ErrorCodes::TransportClosed, false},
        Case{ERROR_NOT_ENOUGH_MEMORY,
             Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{ERROR_GEN_FAILURE,
             Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 18110U};
        auto acceptSlot = slot(context.slotApi);
        NativeState native = context.nativeState();
        native.acceptResult = FALSE;
        ActiveNativeScope active{native};
        context.extensionApi->nativeError = ERROR_IO_PENDING;
        static_cast<void>(take(acceptSlot->issue(
            *context.runtime, *context.extensions, *context.listener)));
        const auto reaped = acceptSlot->reapFailed(
            acceptSlot->borrowedOperation(), testCase.nativeCode);
        requireError(
            reaped,
            testCase.stableCode,
            testCase.retryable,
            "failed completion mapping was incorrect");
        require(context.winsock->wasClosed(native.acceptedSocket),
                "failed completion did not close accepted socket");
        requireIdleAndZeroed(*acceptSlot);
    }
}

void cancelAndCompletionCallsAreDeterministicallySerialized()
{
    for (const bool successfulCompletion : {true, false}) {
        Context context{"127.0.0.1", 18111U};
        auto acceptSlot = slot(context.slotApi);
        NativeState native = context.nativeState();
        native.acceptResult = FALSE;
        ActiveNativeScope active{native};
        context.extensionApi->nativeError = ERROR_IO_PENDING;
        static_cast<void>(take(acceptSlot->issue(
            *context.runtime, *context.extensions, *context.listener)));

        context.slotApi->cancelResult = TRUE;
        context.slotApi->beginBlockingCancellation();
        std::optional<Domain::Result<CancellationDisposition>> cancelResult;
        std::thread cancelThread{[&]() {
            cancelResult.emplace(acceptSlot->requestCancellation());
        }};
        context.slotApi->waitForCancellationEntry();

        std::atomic<bool> reapStarted{};
        std::atomic<bool> reapFinished{};
        std::optional<Domain::Result<AcceptedConnection>> successResult;
        std::optional<Domain::Result<void>> failureResult;
        std::thread completionThread{[&]() {
            reapStarted.store(true, std::memory_order_release);
            if (successfulCompletion) {
                successResult.emplace(acceptSlot->reapSuccessful(
                    *context.extensions, acceptSlot->borrowedOperation()));
            } else {
                failureResult.emplace(acceptSlot->reapFailed(
                    acceptSlot->borrowedOperation(),
                    ERROR_OPERATION_ABORTED));
            }
            reapFinished.store(true, std::memory_order_release);
        }};

        while (!reapStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::yield();
        require(!reapFinished.load(std::memory_order_acquire),
                "completion raced through an in-flight CancelIoEx call");
        require(acceptSlot->state() == SlotState::Issued,
                "slot changed state before CancelIoEx returned");

        context.slotApi->releaseCancellation();
        cancelThread.join();
        completionThread.join();
        require(cancelResult.has_value() &&
                    static_cast<bool>(*cancelResult) &&
                    cancelResult->value() ==
                        CancellationDisposition::Requested,
                "serialized cancellation did not complete as requested");
        if (successfulCompletion) {
            require(successResult.has_value() &&
                        static_cast<bool>(*successResult),
                    "serialized normal completion failed");
        } else {
            require(failureResult.has_value(),
                    "serialized failed completion did not return");
            requireError(
                *failureResult,
                Domain::ErrorCodes::Cancelled,
                false,
                "serialized failed completion used the wrong outcome");
        }
        requireIdleAndZeroed(*acceptSlot);
    }
}

} // namespace

int main()
{
    try {
        factoryCreatesStableZeroedIdleStorage();
        synchronousIpv4IssuePublishesBeforeNativeAndTransfersOwnership();
        pendingIpv6IssueUsesExactFamilyFraming();
        immediateAcceptFailureClosesAndRestoresIdleState();
        updateContextFailuresAreTypedAndReaped();
        exactListenerAndPeerValidationRejectsMismatches();
        wrongOverlappedNeverReapsOrReleasesOwnership();
        cancellationRetainsStorageAcrossBothRaceOutcomes();
        cancellationFailuresLeaveTheIssuedOperationReapable();
        failedCompletionMappingsSeparateClientAndListenerOutcomes();
        cancelAndCompletionCallsAreDeterministicallySerialized();
        std::cout << "Dashboard accept slot tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard accept slot tests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard accept slot tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
