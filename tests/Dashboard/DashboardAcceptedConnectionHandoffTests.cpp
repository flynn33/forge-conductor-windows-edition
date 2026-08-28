#include "Infrastructure/Windows/Detail/DashboardAcceptedConnectionHandoff.h"

#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"
#include "Infrastructure/Windows/Detail/DashboardListeningSocket.h"
#include "Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.h"
#include "Infrastructure/Windows/Detail/DashboardWinsockRuntime.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <MSWSock.h>

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
#include <type_traits>
#include <utility>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using AcceptApi = Detail::IDashboardAcceptSlotApi;
using AcceptedConnection = Detail::DashboardAcceptedConnection;
using AdmissionController = Detail::DashboardAdmissionController;
using CompletionKey = Detail::DashboardIoCompletionKey;
using Endpoint = Detail::DashboardLoopbackEndpoint;
using ExtensionApi = Detail::IDashboardWinsockExtensionApi;
using Handoff = Detail::DashboardAcceptedConnectionHandoff;
using HandoffDisposition =
    Detail::DashboardAcceptedConnectionHandoffDisposition;
using IoApi = Detail::IDashboardIoCompletionPortApi;
using Kernel = Detail::DashboardIocpWorkerKernel;
using KernelSink = Detail::IDashboardIocpCompletionSink;
using Listener = Detail::DashboardListeningSocket;
using ListenerApi = Detail::IDashboardListeningSocketApi;
using OverloadWork = Detail::DashboardAdmissionOverloadWork;
using Port = Detail::DashboardIoCompletionPort;
using ReapResult = Detail::DashboardAcceptReapResult;
using Runtime = Detail::DashboardWinsockRuntime;
using RuntimeServices = Detail::DashboardConnectionRuntimeServices;
using Set = Detail::DashboardAcceptSlotSet;
using WinsockApi = Detail::IDashboardWinsockApi;

constexpr CompletionKey ListenerKey{0xACCE5510U};
constexpr std::uint64_t GenerationId = 7U;

static_assert(std::is_final_v<Handoff>);
static_assert(std::is_final_v<OverloadWork>);
static_assert(std::is_abstract_v<
              Detail::IDashboardAcceptedConnectionOwnerFactory>);
static_assert(std::is_abstract_v<Detail::IDashboardConnectionRegistrar>);
static_assert(std::is_abstract_v<
              Detail::IDashboardAdmissionOverloadResponder>);
static_assert(!std::is_copy_constructible_v<OverloadWork>);
static_assert(std::is_nothrow_move_constructible_v<OverloadWork>);
static_assert(!std::is_move_assignable_v<OverloadWork>);
static_assert(noexcept(std::declval<Handoff&>().consume(
    std::declval<ReapResult>())));
static_assert(noexcept(std::declval<OverloadWork&>().complete()));
static_assert(noexcept(
    std::declval<OverloadWork&>().closeOriginAdmission()));

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
    AcceptIssue,
    AcceptedSocketClose,
    OwnerFactory,
    Registrar,
    TargetStart,
    TargetShutdown,
    OverloadResponse,
};

class Trace final {
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
        std::size_t count{};
        for (std::size_t index{}; index < count_; ++index) {
            if (events_[index] == searched) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t lastIndexOf(const Event searched) const
    {
        const std::lock_guard lock{mutex_};
        for (std::size_t index = count_; index > 0U; --index) {
            if (events_[index - 1U] == searched) {
                return index - 1U;
            }
        }
        fail("expected trace event was not recorded");
    }

private:
    mutable std::mutex mutex_;
    std::array<Event, 128U> events_{};
    std::size_t count_{};
};

class FakeWinsockApi final : public WinsockApi {
public:
    explicit FakeWinsockApi(std::shared_ptr<Trace> trace) noexcept
        : trace_{std::move(trace)}
    {
    }

    [[nodiscard]] int startup(WORD, WSADATA& data) noexcept override
    {
        data.wVersion = MAKEWORD(2, 2);
        return 0;
    }

    [[nodiscard]] int cleanup() noexcept override { return 0; }

    [[nodiscard]] SOCKET createSocket(
        int,
        int,
        int,
        DWORD) noexcept override
    {
        return nextSocket++;
    }

    [[nodiscard]] int closeSocket(const SOCKET socket) noexcept override
    {
        ++closeCalls;
        if (socket != listenerSocket) {
            trace_->record(Event::AcceptedSocketClose);
        }
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override { return WSAENOBUFS; }

    std::shared_ptr<Trace> trace_;
    SOCKET nextSocket{static_cast<SOCKET>(0x4100U)};
    SOCKET listenerSocket{INVALID_SOCKET};
    std::size_t closeCalls{};
};

class FakeListenerApi final : public ListenerApi {
public:
    explicit FakeListenerApi(const Endpoint& endpoint) noexcept
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

    [[nodiscard]] int bindSocket(SOCKET, const sockaddr*, int) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int getSocketName(
        SOCKET,
        sockaddr* const address,
        int& addressLength) noexcept override
    {
        if (address == nullptr || addressLength < addressLength_) {
            return SOCKET_ERROR;
        }
        std::memcpy(
            address, &address_, static_cast<std::size_t>(addressLength_));
        addressLength = addressLength_;
        return 0;
    }

    [[nodiscard]] int listenSocket(SOCKET, int) noexcept override { return 0; }
    [[nodiscard]] int lastError() noexcept override { return WSAEINVAL; }

private:
    sockaddr_storage address_{};
    int addressLength_{};
};

struct FakeExtensionState final {
    std::shared_ptr<Trace> trace;
    std::uint16_t listenerPort{};
    std::array<OVERLAPPED*, Set::SlotCount> operations{};
    std::size_t operationCount{};
    std::size_t issueCalls{};
    std::size_t failIssueCall{};
    int lastError{WSA_IO_PENDING};

    void remember(OVERLAPPED* const operation) noexcept
    {
        for (std::size_t index{}; index < operationCount; ++index) {
            if (operations[index] == operation) {
                return;
            }
        }
        if (operationCount < operations.size()) {
            operations[operationCount++] = operation;
        }
    }
};

FakeExtensionState* activeExtensionState{};

BOOL PASCAL fakeAcceptEx(
    SOCKET,
    SOCKET,
    PVOID,
    DWORD,
    DWORD,
    DWORD,
    LPDWORD const received,
    LPOVERLAPPED const operation)
{
    if (activeExtensionState == nullptr) {
        return FALSE;
    }
    auto& state = *activeExtensionState;
    ++state.issueCalls;
    state.remember(operation);
    state.trace->record(Event::AcceptIssue);
    if (received != nullptr) {
        *received = 0U;
    }
    state.lastError = state.issueCalls == state.failIssueCall
        ? WSAENOBUFS
        : WSA_IO_PENDING;
    WSASetLastError(state.lastError);
    return FALSE;
}

void populateAddress(
    void* const destination,
    const std::uint16_t port) noexcept
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::memcpy(destination, &address, sizeof(address));
}

VOID PASCAL fakeGetAcceptExSockaddrs(
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    DWORD,
    LPSOCKADDR* const localAddress,
    LPINT const localLength,
    LPSOCKADDR* const remoteAddress,
    LPINT const remoteLength)
{
    auto* const bytes = static_cast<std::byte*>(outputBuffer);
    auto* const local = reinterpret_cast<sockaddr*>(bytes + 16U);
    auto* const remote = reinterpret_cast<sockaddr*>(
        bytes + receiveDataLength + localAddressLength + 16U);
    populateAddress(local, activeExtensionState->listenerPort);
    populateAddress(remote, 49123U);
    *localAddress = local;
    *remoteAddress = remote;
    *localLength = static_cast<int>(sizeof(sockaddr_in));
    *remoteLength = static_cast<int>(sizeof(sockaddr_in));
}

[[nodiscard]] bool sameGuid(const GUID& left, const GUID& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

class FakeExtensionApi final : public ExtensionApi {
public:
    [[nodiscard]] int ioctl(
        SOCKET,
        DWORD,
        const void* const inputBuffer,
        DWORD,
        void* const outputBuffer,
        const DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept override
    {
        const auto& identifier = *static_cast<const GUID*>(inputBuffer);
        if (sameGuid(identifier, WSAID_ACCEPTEX) &&
            outputBufferLength == sizeof(LPFN_ACCEPTEX)) {
            const LPFN_ACCEPTEX function = &fakeAcceptEx;
            std::memcpy(outputBuffer, &function, sizeof(function));
            bytesReturned = static_cast<DWORD>(sizeof(function));
            return 0;
        }
        if (sameGuid(identifier, WSAID_GETACCEPTEXSOCKADDRS) &&
            outputBufferLength == sizeof(LPFN_GETACCEPTEXSOCKADDRS)) {
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
        return activeExtensionState == nullptr
            ? WSAEINVAL
            : activeExtensionState->lastError;
    }
};

class FakeAcceptApi final : public AcceptApi {
public:
    [[nodiscard]] int updateAcceptContext(
        SOCKET,
        SOCKET) noexcept override
    {
        return 0;
    }

    [[nodiscard]] BOOL cancelAccept(SOCKET, OVERLAPPED*) noexcept override
    {
        return TRUE;
    }

    [[nodiscard]] int lastSocketError() noexcept override
    {
        return WSAECONNRESET;
    }

    [[nodiscard]] DWORD lastSystemError() noexcept override
    {
        return ERROR_SUCCESS;
    }
};

thread_local DWORD fakeIoError{ERROR_SUCCESS};

class FakeIoApi final : public IoApi {
public:
    struct Completion final {
        DWORD bytes{};
        ULONG_PTR key{};
        OVERLAPPED* operation{};
    };

    [[nodiscard]] HANDLE createIoCompletionPort(
        HANDLE file,
        HANDLE existing,
        ULONG_PTR,
        DWORD) noexcept override
    {
        return file == INVALID_HANDLE_VALUE ? portHandle() : existing;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        HANDLE,
        const DWORD bytes,
        const ULONG_PTR key,
        OVERLAPPED* const operation) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (queueCount_ == queue_.size()) {
            fakeIoError = ERROR_NOT_ENOUGH_MEMORY;
            return FALSE;
        }
        const auto tail = (queueHead_ + queueCount_) % queue_.size();
        queue_[tail] = Completion{bytes, key, operation};
        ++queueCount_;
        changed_.notify_one();
        return TRUE;
    }

    [[nodiscard]] BOOL getQueuedCompletionStatus(
        HANDLE,
        DWORD& bytes,
        ULONG_PTR& key,
        OVERLAPPED*& operation,
        const DWORD timeoutMilliseconds) noexcept override
    {
        std::unique_lock lock{mutex_};
        const auto ready = [this]() noexcept {
            return queueCount_ != 0U || closed_;
        };
        if (!ready()) {
            static_cast<void>(changed_.wait_for(
                lock,
                std::chrono::milliseconds{timeoutMilliseconds},
                ready));
        }
        if (queueCount_ == 0U) {
            bytes = 0U;
            key = 0U;
            operation = nullptr;
            fakeIoError = closed_ ? ERROR_INVALID_HANDLE : WAIT_TIMEOUT;
            return FALSE;
        }
        const auto completion = queue_[queueHead_];
        queueHead_ = (queueHead_ + 1U) % queue_.size();
        --queueCount_;
        bytes = completion.bytes;
        key = completion.key;
        operation = completion.operation;
        fakeIoError = ERROR_SUCCESS;
        return TRUE;
    }

    [[nodiscard]] BOOL closeHandle(HANDLE) noexcept override
    {
        const std::lock_guard lock{mutex_};
        closed_ = true;
        changed_.notify_all();
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override { return fakeIoError; }

    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x9900U));
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Completion, 16U> queue_{};
    std::size_t queueHead_{};
    std::size_t queueCount_{};
    bool closed_{};
};

class QuietKernelSink final : public KernelSink {
public:
    void consume(Detail::DashboardIoCompletionPacket, DWORD) noexcept override
    {
    }

    void fatal(DWORD) noexcept override { ++fatalCalls; }

    std::atomic_size_t fatalCalls{};
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
        return Domain::MonotonicTimePoint{std::chrono::seconds{10}};
    }
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

class FakeTarget final : public Detail::IDashboardConnectionDispatchTarget {
public:
    FakeTarget(
        const std::uint64_t generationId,
        const Detail::DashboardConnectionRuntimeIdentity identity,
        AdmissionController::Lease admissionLease,
        AcceptedConnection acceptedConnection,
        std::shared_ptr<Trace> trace) noexcept
        : generationId_{generationId},
          identity_{identity},
          admissionLease_{std::move(admissionLease)},
          acceptedConnection_{
              std::in_place, std::move(acceptedConnection)},
          trace_{std::move(trace)}
    {
    }

    ~FakeTarget() noexcept override { beginShutdown(); }

    [[nodiscard]] CompletionKey completionKey() const noexcept override
    {
        return identity_.completionKey;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return identity_.registrationId;
    }

    [[nodiscard]] std::uint64_t generationId() const noexcept override
    {
        return generationId_;
    }

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionDrainObserver>)
        noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        trace_->record(Event::TargetStart);
        started_ = true;
        return Domain::Result<void>::success();
    }

    void dispatchIocp(DWORD, OVERLAPPED*, DWORD) noexcept override {}
    void dispatchDeadline(
        ForgeConductor::Infrastructure::Windows::WindowsDashboardDeadline)
        noexcept override
    {
    }

    void beginShutdown() noexcept override
    {
        if (drained_) {
            return;
        }
        trace_->record(Event::TargetShutdown);
        acceptedConnection_.reset();
        admissionLease_.release();
        drained_ = true;
    }

    [[nodiscard]] bool isDrained() const noexcept override { return drained_; }

    [[nodiscard]] Detail::DashboardConnectionStateSnapshot snapshot()
        const noexcept override
    {
        return {
            drained_
                ? Detail::DashboardConnectionLifecycleState::Drained
                : Detail::DashboardConnectionLifecycleState::Created,
            identity_.registrationId,
            generationId_,
            identity_.completionKey,
            false,
            false,
            false,
            drained_,
            false};
    }

    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    std::uint64_t generationId_{};
    Detail::DashboardConnectionRuntimeIdentity identity_;
    AdmissionController::Lease admissionLease_;
    std::optional<AcceptedConnection> acceptedConnection_;
    std::shared_ptr<Trace> trace_;
    bool started_{};
    bool drained_{};
};

class FakeOwnerFactory final
    : public Detail::IDashboardAcceptedConnectionOwnerFactory {
public:
    FakeOwnerFactory(
        Set& set,
        FakeExtensionState& extensionState,
        std::shared_ptr<Trace> trace) noexcept
        : set_{set},
          extensionState_{extensionState},
          trace_{std::move(trace)},
          policyIdentity_{this}
    {
    }

    [[nodiscard]] const void* applicationPolicyIdentity()
        const noexcept override
    {
        return policyIdentity_;
    }

    [[nodiscard]] Domain::Result<
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget>>
    createOwner(
        const std::uint64_t generationId,
        const Detail::DashboardConnectionRuntimeIdentity identity,
        const Domain::MonotonicTimePoint admittedAt,
        AdmissionController::Lease admissionLease,
        AcceptedConnection acceptedConnection) noexcept override
    {
        trace_->record(Event::OwnerFactory);
        pausedAtCall = set_.snapshot().pausedCount();
        issuesAtCall = extensionState_.issueCalls;
        observedIdentity = identity;
        observedAdmittedAt = admittedAt;
        if (failCreation) {
            return Domain::Result<std::shared_ptr<
                Detail::IDashboardConnectionDispatchTarget>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Injected owner-factory failure."));
        }
        auto target = std::make_shared<FakeTarget>(
            generationId,
            identity,
            std::move(admissionLease),
            std::move(acceptedConnection),
            trace_);
        return Domain::Result<std::shared_ptr<
            Detail::IDashboardConnectionDispatchTarget>>::success(
            std::move(target));
    }

    Set& set_;
    FakeExtensionState& extensionState_;
    std::shared_ptr<Trace> trace_;
    const void* policyIdentity_{};
    Detail::DashboardConnectionRuntimeIdentity observedIdentity{};
    Domain::MonotonicTimePoint observedAdmittedAt{};
    std::size_t pausedAtCall{};
    std::size_t issuesAtCall{};
    bool failCreation{};
};

class FakeRegistrar final : public Detail::IDashboardConnectionRegistrar {
public:
    FakeRegistrar(
        Set& set,
        FakeExtensionState& extensionState,
        std::shared_ptr<Trace> trace) noexcept
        : set_{set},
          extensionState_{extensionState},
          trace_{std::move(trace)}
    {
    }

    [[nodiscard]] Domain::Result<void> registerConnection(
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget> target)
        noexcept override
    {
        trace_->record(Event::Registrar);
        pausedAtCall = set_.snapshot().pausedCount();
        issuesAtCall = extensionState_.issueCalls;
        if (failRegistration) {
            target->beginShutdown();
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "Injected registrar failure."));
        }
        retained = std::move(target);
        return retained->start();
    }

    void release() noexcept
    {
        if (retained != nullptr) {
            retained->beginShutdown();
            retained.reset();
        }
    }

    Set& set_;
    FakeExtensionState& extensionState_;
    std::shared_ptr<Trace> trace_;
    std::shared_ptr<Detail::IDashboardConnectionDispatchTarget> retained;
    std::size_t pausedAtCall{};
    std::size_t issuesAtCall{};
    bool failRegistration{};
};

class CapturingOverloadResponder final
    : public Detail::IDashboardAdmissionOverloadResponder {
public:
    explicit CapturingOverloadResponder(
        std::shared_ptr<Trace> trace,
        const bool retain = true) noexcept
        : trace_{std::move(trace)}, retain_{retain}
    {
    }

    void respond(OverloadWork work) noexcept override
    {
        trace_->record(Event::OverloadResponse);
        ++callCount;
        failureCode = work.admissionFailure().code;
        observedSocket = work.borrowedNativeSocket();
        if (retain_) {
            retained = std::make_unique<OverloadWork>(std::move(work));
        }
    }

    [[nodiscard]] std::size_t cancelGeneration(
        const std::uint64_t generationId) noexcept override
    {
        if (retained == nullptr ||
            retained->originGenerationId() != generationId) {
            return 0U;
        }
        const auto failure = retained->closeOriginAdmission();
        return failure.has_value() ? 0U : 1U;
    }

    void drainTerminalGenerationNotifications() noexcept override {}

    void complete() noexcept
    {
        retained.reset();
    }

    std::shared_ptr<Trace> trace_;
    std::unique_ptr<OverloadWork> retained;
    std::string failureCode;
    SOCKET observedSocket{INVALID_SOCKET};
    std::size_t callCount{};
    bool retain_{};
};

class Context final {
public:
    explicit Context(
        const Dashboard::DashboardTransportLimits limits = {1U, 1U, 2U})
        : endpoint{take(Endpoint::create("127.0.0.1", 18441U))},
          trace{std::make_shared<Trace>()},
          winsock{std::make_shared<FakeWinsockApi>(trace)},
          runtime{take(Runtime::create(winsock))},
          listenerApi{std::make_shared<FakeListenerApi>(endpoint)},
          extensionState{trace, endpoint.port()},
          extensionApi{std::make_shared<FakeExtensionApi>()},
          acceptApi{std::make_shared<FakeAcceptApi>()},
          ioApi{std::make_shared<FakeIoApi>()},
          kernelSink{std::make_shared<QuietKernelSink>()},
          clock{std::make_shared<FixedClock>()},
          uuidGenerator{std::make_shared<FixedUuidGenerator>()},
          operationalState{std::make_shared<FixedOperationalState>()}
    {
        activeExtensionState = &extensionState;
        auto listener = take(Listener::create(
            *runtime, endpoint, listenerApi));
        winsock->listenerSocket = listener.borrowedNativeSocket();
        kernel = take(Kernel::create(take(Port::create(ioApi)), kernelSink));
        set = take(Set::create(
            *runtime,
            std::move(listener),
            ListenerKey,
            extensionApi,
            acceptApi));
        admission = take(AdmissionController::create(limits));
        const std::array fixedKeys{CompletionKey{1U}, ListenerKey};
        runtimeServices = take(RuntimeServices::create(
            clock,
            uuidGenerator,
            operationalState,
            fixedKeys));
    }

    ~Context() noexcept
    {
        drain();
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

    [[nodiscard]] ReapResult accept(const std::size_t slot = 0U)
    {
        if (!started) {
            require(static_cast<bool>(set->start(*kernel)),
                    "accept-slot set did not start");
            started = true;
        }
        return take(set->reap(
            ListenerKey,
            0U,
            extensionState.operations[slot],
            ERROR_SUCCESS));
    }

    void drain() noexcept
    {
        if (set == nullptr) {
            return;
        }
        static_cast<void>(set->closeAdmissionAndRequestCancellation());
        for (std::size_t index{};
             index < extensionState.operationCount;
             ++index) {
            static_cast<void>(set->reap(
                ListenerKey,
                0U,
                extensionState.operations[index],
                ERROR_OPERATION_ABORTED));
        }
    }

    Endpoint endpoint;
    std::shared_ptr<Trace> trace;
    std::shared_ptr<FakeWinsockApi> winsock;
    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<FakeListenerApi> listenerApi;
    FakeExtensionState extensionState;
    std::shared_ptr<FakeExtensionApi> extensionApi;
    std::shared_ptr<FakeAcceptApi> acceptApi;
    std::shared_ptr<FakeIoApi> ioApi;
    std::shared_ptr<QuietKernelSink> kernelSink;
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<Set> set;
    std::unique_ptr<AdmissionController> admission;
    std::shared_ptr<FixedClock> clock;
    std::shared_ptr<FixedUuidGenerator> uuidGenerator;
    std::shared_ptr<FixedOperationalState> operationalState;
    std::unique_ptr<RuntimeServices> runtimeServices;
    bool started{};
};

struct HandoffFixture final {
    explicit HandoffFixture(Context& context)
        : factory{std::make_shared<FakeOwnerFactory>(
              *context.set,
              context.extensionState,
              context.trace)},
          registrar{std::make_shared<FakeRegistrar>(
              *context.set,
              context.extensionState,
              context.trace)},
          overload{std::make_shared<CapturingOverloadResponder>(context.trace)},
          handoff{take(Handoff::create(
              GenerationId,
              *context.set,
              *context.admission,
              *context.runtimeServices,
              factory,
              registrar,
              overload))}
    {
    }

    std::shared_ptr<FakeOwnerFactory> factory;
    std::shared_ptr<FakeRegistrar> registrar;
    std::shared_ptr<CapturingOverloadResponder> overload;
    std::unique_ptr<Handoff> handoff;
};

void registrationOwnsEverythingBeforeTheExactAcceptResumes()
{
    Context context;
    HandoffFixture fixture{context};
    require(fixture.handoff->generationId() == GenerationId &&
                fixture.handoff->completionKey() == ListenerKey &&
                fixture.handoff->acceptSlotSetIdentity() ==
                    context.set.get() &&
                fixture.handoff->applicationPolicyIdentity() ==
                    fixture.factory->applicationPolicyIdentity(),
            "handoff did not retain exact immutable composition identity");
    auto accepted = context.accept();
    const auto result = fixture.handoff->consume(std::move(accepted));
    require(static_cast<bool>(result) &&
                result.value() == HandoffDisposition::Registered,
            "valid accepted connection was not registered");
    require(fixture.factory->pausedAtCall == 1U &&
                fixture.registrar->pausedAtCall == 1U,
            "accept slot resumed before owner creation and registration");
    require(fixture.factory->issuesAtCall == Set::SlotCount &&
                fixture.registrar->issuesAtCall == Set::SlotCount &&
                context.extensionState.issueCalls == Set::SlotCount + 1U,
            "exact accept slot did not remain paused through registration");
    require(context.trace->lastIndexOf(Event::OwnerFactory) <
                context.trace->lastIndexOf(Event::Registrar) &&
                context.trace->lastIndexOf(Event::Registrar) <
                    context.trace->lastIndexOf(Event::TargetStart) &&
                context.trace->lastIndexOf(Event::TargetStart) <
                    context.trace->lastIndexOf(Event::AcceptIssue),
            "handoff sequencing was not factory, register/start, then resume");
    require(fixture.factory->observedIdentity.registrationId == 1U &&
                fixture.factory->observedIdentity.completionKey ==
                    CompletionKey{2U},
            "handoff did not allocate the exact first nonreserved identity");
    require(
        fixture.factory->observedAdmittedAt ==
            context.clock->monotonicNow(),
        "handoff did not preserve the time at which admission was obtained");
    require(fixture.registrar->retained != nullptr,
            "registrar did not retain complete connection ownership");
    require(take(context.admission->snapshot()).shortConnectionCount() == 1U,
            "registered connection did not retain its short admission");

    fixture.registrar->release();
    require(take(context.admission->snapshot()).totalConnectionCount() == 0U,
            "drained connection did not release its admission");
}

void ownerCreationFailureClosesAndReleasesBeforeResume()
{
    Context context;
    HandoffFixture fixture{context};
    fixture.factory->failCreation = true;
    const auto result = fixture.handoff->consume(context.accept());
    requireError(
        result,
        Domain::ErrorCodes::InternalFailure,
        "owner factory failure was not reported");
    require(fixture.registrar->retained == nullptr,
            "failed owner creation still reached registration");
    require(take(context.admission->snapshot()).totalConnectionCount() == 0U,
            "owner creation failure leaked admission");
    require(context.set->snapshot().pausedCount() == 0U &&
                context.extensionState.issueCalls == Set::SlotCount + 1U,
            "owner creation failure stranded its resume token");
    require(context.trace->lastIndexOf(Event::AcceptedSocketClose) <
                context.trace->lastIndexOf(Event::AcceptIssue),
            "failed accepted socket was not closed before slot resume");
}

void registrarFailureDrainsOwnershipBeforeResume()
{
    Context context;
    HandoffFixture fixture{context};
    fixture.registrar->failRegistration = true;
    const auto result = fixture.handoff->consume(context.accept());
    requireError(
        result,
        Domain::ErrorCodes::TransportClosed,
        "registrar failure was not reported");
    require(context.trace->lastIndexOf(Event::Registrar) <
                context.trace->lastIndexOf(Event::TargetShutdown) &&
                context.trace->lastIndexOf(Event::TargetShutdown) <
                    context.trace->lastIndexOf(Event::AcceptIssue),
            "registration failure did not drain before exact resume");
    require(take(context.admission->snapshot()).totalConnectionCount() == 0U,
            "registration failure leaked admission");
    require(context.set->snapshot().pausedCount() == 0U,
            "registration failure stranded its resume token");
}

void admissionRejectionTransfersBoundedOverloadOwnership()
{
    Context context;
    HandoffFixture fixture{context};
    auto heldAdmission = take(context.admission->tryAccept());
    const auto result = fixture.handoff->consume(context.accept());
    require(static_cast<bool>(result) &&
                result.value() ==
                    HandoffDisposition::OverloadResponseTransferred,
            "capacity rejection was not transferred to overload response");
    require(fixture.overload->callCount == 1U &&
                fixture.overload->failureCode ==
                    Domain::ErrorCodes::LimitExceeded &&
                fixture.overload->observedSocket != INVALID_SOCKET,
            "overload responder did not receive the fixed rejection work");
    require(fixture.overload->retained != nullptr &&
                fixture.overload->retained->ownsCompletionObligation(),
            "overload responder did not retain socket/resume ownership");
    require(context.set->snapshot().pausedCount() == 1U &&
                context.extensionState.issueCalls == Set::SlotCount,
            "overload response did not withhold its exact accept slot");
    require(fixture.factory->pausedAtCall == 0U &&
                fixture.registrar->pausedAtCall == 0U,
            "admission rejection still allocated connection ownership");

    const auto completed = fixture.overload->retained->complete();
    require(static_cast<bool>(completed) &&
                completed.value() ==
                    Detail::DashboardAcceptResumeDisposition::Reissued,
            "explicit overload completion did not reissue its exact slot");
    require(!fixture.overload->retained->ownsCompletionObligation(),
            "explicit overload completion retained an ownership obligation");
    requireError(
        fixture.overload->retained->complete(),
        Domain::ErrorCodes::Conflict,
        "completed overload work accepted a duplicate completion");
    fixture.overload->complete();
    require(context.set->snapshot().pausedCount() == 0U &&
                context.extensionState.issueCalls == Set::SlotCount + 1U,
            "completed overload response did not return its exact token");
    require(context.trace->lastIndexOf(Event::AcceptedSocketClose) <
                context.trace->lastIndexOf(Event::AcceptIssue),
            "overload completion resumed before closing the socket");
    heldAdmission.release();
}

void registeredConnectionSurvivesExactResumeIssueFailure()
{
    Context context;
    HandoffFixture fixture{context};
    auto accepted = context.accept();
    context.extensionState.failIssueCall = Set::SlotCount + 1U;
    const auto result = fixture.handoff->consume(std::move(accepted));
    requireError(
        result,
        Domain::ErrorCodes::LimitExceeded,
        "resume issue failure was not reported");
    require(fixture.registrar->retained != nullptr,
            "accept resume failure rolled back the registered connection");
    require(context.trace->lastIndexOf(Event::TargetStart) <
                context.trace->lastIndexOf(Event::AcceptIssue),
            "failed resume was attempted before connection start");
    const auto setSnapshot = context.set->snapshot();
    require(!setSnapshot.admissionOpen() &&
                setSnapshot.pausedCount() == 0U &&
                setSnapshot.drainedCount() == 1U,
            "failed exact resume did not close admission and drain its slot");
    require(take(context.admission->snapshot()).shortConnectionCount() == 1U,
            "registered owner lost admission after listener issue failure");
    fixture.registrar->release();
    require(take(context.admission->snapshot()).totalConnectionCount() == 0U,
            "registered owner did not release after resume issue failure");
}

void droppedOverloadWorkStillClosesAndResumes()
{
    Context context;
    auto factory = std::make_shared<FakeOwnerFactory>(
        *context.set, context.extensionState, context.trace);
    auto registrar = std::make_shared<FakeRegistrar>(
        *context.set, context.extensionState, context.trace);
    auto overload = std::make_shared<CapturingOverloadResponder>(
        context.trace, false);
    auto handoff = take(Handoff::create(
        GenerationId,
        *context.set,
        *context.admission,
        *context.runtimeServices,
        factory,
        registrar,
        overload));
    auto heldAdmission = take(context.admission->tryAccept());
    const auto result = handoff->consume(context.accept());
    require(static_cast<bool>(result),
            "synchronously dropped overload work failed the handoff");
    require(context.set->snapshot().pausedCount() == 0U &&
                context.extensionState.issueCalls == Set::SlotCount + 1U,
            "overload work destructor did not discharge both obligations");
    heldAdmission.release();
}

void overloadWorkReturnsAfterItsListenerGenerationCloses()
{
    Context context;
    HandoffFixture fixture{context};
    auto heldAdmission = take(context.admission->tryAccept());
    const auto result = fixture.handoff->consume(context.accept());
    require(static_cast<bool>(result) &&
                fixture.overload->retained != nullptr,
            "overload work was not retained before listener close");
    const auto closeFailure =
        fixture.overload->retained->closeOriginAdmission();
    require(!closeFailure.has_value(),
            "overload work could not close its originating generation");
    require(context.set->snapshot().awaitingReturnCount() == 1U,
            "listener close did not retain the exact overload token");

    const auto completed = fixture.overload->retained->complete();
    require(static_cast<bool>(completed) &&
                completed.value() ==
                    Detail::DashboardAcceptResumeDisposition::
                        ReturnedAfterClose,
            "overload completion reissued after listener close");
    fixture.overload->complete();
    const auto snapshot = context.set->snapshot();
    require(snapshot.awaitingReturnCount() == 0U &&
                snapshot.drainedCount() == 1U &&
                snapshot.outstandingResumeTokenCount() == 0U,
            "closed listener did not drain the returned overload slot");
    heldAdmission.release();
}

void closedGenerationAcceptIsRejectedWithoutInventingAResume()
{
    Context context;
    HandoffFixture fixture{context};
    require(static_cast<bool>(context.set->start(*context.kernel)),
            "accept-slot set did not start");
    context.started = true;
    static_cast<void>(context.set->closeAdmissionAndRequestCancellation());
    auto accepted = take(context.set->reap(
        ListenerKey,
        0U,
        context.extensionState.operations[0U],
        ERROR_SUCCESS));
    require(accepted.disposition() ==
                Detail::DashboardAcceptReapDisposition::AcceptedAndDrained &&
                !accepted.hasResumeToken(),
            "closed generation did not produce the expected terminal accept");
    const auto result = fixture.handoff->consume(std::move(accepted));
    requireError(
        result,
        Domain::ErrorCodes::IntegrityFailure,
        "terminal accept was incorrectly treated as a paused handoff");
    require(fixture.overload->callCount == 0U &&
                fixture.registrar->retained == nullptr,
            "terminal accept escaped into overload or connection ownership");
}

void invalidCompositionIsRejected()
{
    Context context;
    auto factory = std::make_shared<FakeOwnerFactory>(
        *context.set, context.extensionState, context.trace);
    auto registrar = std::make_shared<FakeRegistrar>(
        *context.set, context.extensionState, context.trace);
    auto overload =
        std::make_shared<CapturingOverloadResponder>(context.trace);
    requireError(
        Handoff::create(
            0U,
            *context.set,
            *context.admission,
            *context.runtimeServices,
            factory,
            registrar,
            overload),
        Domain::ErrorCodes::InvalidRequest,
        "zero listener generation was accepted");
    requireError(
        Handoff::create(
            GenerationId,
            *context.set,
            *context.admission,
            *context.runtimeServices,
            {},
            registrar,
            overload),
        Domain::ErrorCodes::InvalidRequest,
        "null owner factory was accepted");
    factory->policyIdentity_ = nullptr;
    requireError(
        Handoff::create(
            GenerationId,
            *context.set,
            *context.admission,
            *context.runtimeServices,
            factory,
            registrar,
            overload),
        Domain::ErrorCodes::InvalidRequest,
        "owner factory without application policy identity was accepted");
    require(context.extensionState.issueCalls == 0U &&
                !context.set->snapshot().startAttempted(),
            "invalid handoff composition issued native acceptance");
}

} // namespace

int main()
{
    try {
        registrationOwnsEverythingBeforeTheExactAcceptResumes();
        ownerCreationFailureClosesAndReleasesBeforeResume();
        registrarFailureDrainsOwnershipBeforeResume();
        admissionRejectionTransfersBoundedOverloadOwnership();
        droppedOverloadWorkStillClosesAndResumes();
        overloadWorkReturnsAfterItsListenerGenerationCloses();
        registeredConnectionSurvivesExactResumeIssueFailure();
        closedGenerationAcceptIsRejectedWithoutInventingAResume();
        invalidCompositionIsRejected();
        std::cout
            << "Dashboard accepted-connection handoff tests passed with "
            << assertionCount << " assertions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "Dashboard accepted-connection handoff tests failed after "
            << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    }
}
