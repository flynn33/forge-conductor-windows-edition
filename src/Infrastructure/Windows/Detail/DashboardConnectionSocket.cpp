#include "DashboardConnectionSocket.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error connectionError(
    const std::string_view action,
    const std::uint64_t nativeCode,
    const std::string_view stableCode,
    const bool retryable = false) noexcept
{
    try {
        std::string message{action};
        if (nativeCode != ERROR_SUCCESS) {
            message += " failed with native error ";
            message += std::to_string(nativeCode);
        }
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard connection socket operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error invalidOwnerError() noexcept
{
    return connectionError(
        "Validate dashboard connection socket ownership",
        ERROR_SUCCESS,
        Domain::ErrorCodes::InvalidRequest);
}

[[nodiscard]] Domain::Error conflictError(
    const std::string_view action) noexcept
{
    return connectionError(
        action, ERROR_SUCCESS, Domain::ErrorCodes::Conflict);
}

[[nodiscard]] Domain::Error integrityError(
    const std::string_view action) noexcept
{
    return connectionError(
        action, ERROR_SUCCESS, Domain::ErrorCodes::IntegrityFailure);
}

[[nodiscard]] Domain::Error transportClosedError(
    const std::string_view action,
    const bool retryable = false) noexcept
{
    return connectionError(
        action,
        ERROR_SUCCESS,
        Domain::ErrorCodes::TransportClosed,
        retryable);
}

[[nodiscard]] Domain::Error socketError(
    const std::string_view action,
    const int code) noexcept
{
    switch (code) {
    case WSA_OPERATION_ABORTED:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::Cancelled);
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::TransportClosed,
            true);
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
    case WSAENOTCONN:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::TransportClosed);
    case WSAENOBUFS:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSAEWOULDBLOCK:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::Conflict,
            true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    default:
        return connectionError(
            action,
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error completionError(const DWORD code) noexcept
{
    switch (code) {
    case ERROR_OPERATION_ABORTED:
        return connectionError(
            "Reap dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::Cancelled);
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_ABORTED:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET:
        return connectionError(
            "Reap dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::TransportClosed,
            true);
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
    case WSAENOTCONN:
        return connectionError(
            "Reap dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::TransportClosed);
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case WSAENOBUFS:
        return connectionError(
            "Reap dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        return connectionError(
            "Reap dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    default:
        return connectionError(
            "Reap dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error cancellationError(const DWORD code) noexcept
{
    switch (code) {
    case ERROR_OPERATION_ABORTED:
        return connectionError(
            "Cancel dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::Cancelled);
    case ERROR_INVALID_HANDLE:
        return connectionError(
            "Cancel dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::TransportClosed);
    case ERROR_ACCESS_DENIED:
        return connectionError(
            "Cancel dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::Unauthorized);
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return connectionError(
            "Cancel dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::LimitExceeded,
            true);
    default:
        return connectionError(
            "Cancel dashboard connected-socket operation",
            code,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] bool isReceiveState(
    const DashboardConnectionSocketState state) noexcept
{
    return state == DashboardConnectionSocketState::ReceiveIssued ||
           state ==
               DashboardConnectionSocketState::ReceiveCancellationRequested;
}

[[nodiscard]] bool isSendState(
    const DashboardConnectionSocketState state) noexcept
{
    return state == DashboardConnectionSocketState::SendIssued ||
           state ==
               DashboardConnectionSocketState::SendCancellationRequested;
}

} // namespace

int DashboardConnectionSocketSystemApi::receive(
    const SOCKET socket,
    WSABUF* const buffers,
    const DWORD bufferCount,
    DWORD& transferredBytes,
    DWORD& flags,
    OVERLAPPED* const operation) noexcept
{
    return ::WSARecv(
        socket,
        buffers,
        bufferCount,
        &transferredBytes,
        &flags,
        operation,
        nullptr);
}

int DashboardConnectionSocketSystemApi::send(
    const SOCKET socket,
    WSABUF* const buffers,
    const DWORD bufferCount,
    DWORD& transferredBytes,
    const DWORD flags,
    OVERLAPPED* const operation) noexcept
{
    return ::WSASend(
        socket,
        buffers,
        bufferCount,
        &transferredBytes,
        flags,
        operation,
        nullptr);
}

BOOL DashboardConnectionSocketSystemApi::cancel(
    const SOCKET socket,
    OVERLAPPED* const operation) noexcept
{
    return ::CancelIoEx(reinterpret_cast<HANDLE>(socket), operation);
}

int DashboardConnectionSocketSystemApi::shutdownSocket(
    const SOCKET socket,
    const int how) noexcept
{
    return ::shutdown(socket, how);
}

int DashboardConnectionSocketSystemApi::lastSocketError() noexcept
{
    return ::WSAGetLastError();
}

DWORD DashboardConnectionSocketSystemApi::lastSystemError() noexcept
{
    return ::GetLastError();
}

DashboardConnectionSocketReapResult::DashboardConnectionSocketReapResult(
    const DashboardConnectionSocketOperationKind operationKind,
    const DWORD transferredBytes) noexcept
    : operationKind_{operationKind}, transferredBytes_{transferredBytes}
{
}

DashboardConnectionSocketSnapshot::DashboardConnectionSocketSnapshot(
    const DashboardConnectionSocketState state,
    const bool shutdownRequested,
    const std::size_t receivedByteCount,
    const std::size_t activeBufferLength) noexcept
    : state_{state},
      shutdownRequested_{shutdownRequested},
      receivedByteCount_{receivedByteCount},
      activeBufferLength_{activeBufferLength}
{
}

DashboardConnectionSocket::DashboardConnectionSocket(
    DashboardAcceptedConnection acceptedConnection,
    const DashboardIoCompletionKey completionKey,
    std::shared_ptr<IDashboardConnectionSocketApi> api) noexcept
    : acceptedConnection_{std::move(acceptedConnection)},
      completionKey_{completionKey},
      api_{std::move(api)}
{
}

DashboardConnectionSocket::~DashboardConnectionSocket() noexcept
{
    if (state_.load(std::memory_order_acquire) !=
        DashboardConnectionSocketState::Idle) {
        std::terminate();
    }
}

Domain::Result<std::unique_ptr<DashboardConnectionSocket>>
DashboardConnectionSocket::create(
    DashboardAcceptedConnection acceptedConnection,
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey) noexcept
{
    try {
        return create(
            std::move(acceptedConnection),
            kernel,
            completionKey,
            std::make_shared<DashboardConnectionSocketSystemApi>());
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardConnectionSocket>>::failure(
            connectionError(
                "Allocate dashboard connected-socket native API",
                ERROR_SUCCESS,
                Domain::ErrorCodes::InternalFailure));
    }
}

Domain::Result<std::unique_ptr<DashboardConnectionSocket>>
DashboardConnectionSocket::create(
    DashboardAcceptedConnection acceptedConnection,
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey,
    std::shared_ptr<IDashboardConnectionSocketApi> api) noexcept
{
    if (acceptedConnection.borrowedNativeSocket() == INVALID_SOCKET ||
        completionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue ||
        api == nullptr) {
        return Domain::Result<
            std::unique_ptr<DashboardConnectionSocket>>::failure(
            invalidOwnerError());
    }

    try {
        auto owner = std::unique_ptr<DashboardConnectionSocket>{
            new DashboardConnectionSocket{
                std::move(acceptedConnection),
                completionKey,
                std::move(api)}};
        auto associated = kernel.associateSocket(
            owner->borrowedNativeSocket(), completionKey);
        if (!associated) {
            return Domain::Result<
                std::unique_ptr<DashboardConnectionSocket>>::failure(
                std::move(associated).error());
        }
        return Domain::Result<
            std::unique_ptr<DashboardConnectionSocket>>::success(
            std::move(owner));
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardConnectionSocket>>::failure(
            connectionError(
                "Create and associate dashboard connected socket",
                ERROR_SUCCESS,
                Domain::ErrorCodes::InternalFailure));
    }
}

DashboardConnectionSocketState DashboardConnectionSocket::state()
    const noexcept
{
    return state_.load(std::memory_order_acquire);
}

DashboardConnectionSocketSnapshot DashboardConnectionSocket::snapshot()
    const noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        const auto current = state_.load(std::memory_order_relaxed);
        const std::size_t activeLength = current ==
                DashboardConnectionSocketState::Idle
            ? 0U
            : static_cast<std::size_t>(activeBuffer_.len);
        return DashboardConnectionSocketSnapshot{
            current,
            shutdownRequested_.load(std::memory_order_relaxed),
            receivedByteCount_,
            activeLength};
    } catch (...) {
        return DashboardConnectionSocketSnapshot{
            state_.load(std::memory_order_acquire),
            shutdownRequested_.load(std::memory_order_acquire),
            0U,
            0U};
    }
}

Domain::Result<DashboardConnectionSocketIssueDisposition>
DashboardConnectionSocket::issueReceive() noexcept
{
    const std::scoped_lock lock{mutex_};
    if (state_.load(std::memory_order_relaxed) !=
        DashboardConnectionSocketState::Idle) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            conflictError(
                "Issue a receive while another dashboard socket operation is outstanding"));
    }
    if (shutdownRequested_.load(std::memory_order_relaxed)) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            transportClosedError(
                "Issue a receive after dashboard socket shutdown"));
    }

    operation_ = {};
    activeBuffer_.buf = reinterpret_cast<char*>(receiveBuffer_.data());
    activeBuffer_.len = static_cast<ULONG>(ReceiveBufferLength);
    receiveFlags_ = 0U;
    immediateTransferredBytes_ = 0U;
    borrowedSendBytes_ = nullptr;
    borrowedSendLength_ = 0U;
    receivedByteCount_ = 0U;
    state_.store(
        DashboardConnectionSocketState::ReceiveIssued,
        std::memory_order_release);

    const int status = api_->receive(
        borrowedNativeSocket(),
        &activeBuffer_,
        1U,
        immediateTransferredBytes_,
        receiveFlags_,
        &operation_);
    if (status == 0) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::success(
            DashboardConnectionSocketIssueDisposition::
                CompletedSynchronously);
    }
    if (status != SOCKET_ERROR) {
        resetIssuedOperationLocked();
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            integrityError(
                "Issue dashboard connected-socket receive with an undefined native status"));
    }

    const int code = api_->lastSocketError();
    if (code == WSA_IO_PENDING) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::success(
            DashboardConnectionSocketIssueDisposition::Pending);
    }
    resetIssuedOperationLocked();
    return Domain::Result<
        DashboardConnectionSocketIssueDisposition>::failure(
        socketError("Issue dashboard connected-socket receive", code));
}

Domain::Result<DashboardConnectionSocketIssueDisposition>
DashboardConnectionSocket::issueSend(
    const std::span<const std::byte> bytes) noexcept
{
    if (bytes.empty() ||
        bytes.size() >
            static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            connectionError(
                "Validate borrowed dashboard send bytes",
                ERROR_SUCCESS,
                Domain::ErrorCodes::InvalidRequest));
    }

    const std::scoped_lock lock{mutex_};
    if (state_.load(std::memory_order_relaxed) !=
        DashboardConnectionSocketState::Idle) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            conflictError(
                "Issue a send while another dashboard socket operation is outstanding"));
    }
    if (shutdownRequested_.load(std::memory_order_relaxed)) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            transportClosedError(
                "Issue a send after dashboard socket shutdown"));
    }

    operation_ = {};
    borrowedSendBytes_ = bytes.data();
    borrowedSendLength_ = bytes.size();
    activeBuffer_.buf = reinterpret_cast<char*>(
        const_cast<std::byte*>(borrowedSendBytes_));
    activeBuffer_.len = static_cast<ULONG>(borrowedSendLength_);
    receiveFlags_ = 0U;
    immediateTransferredBytes_ = 0U;
    receivedByteCount_ = 0U;
    state_.store(
        DashboardConnectionSocketState::SendIssued,
        std::memory_order_release);

    const int status = api_->send(
        borrowedNativeSocket(),
        &activeBuffer_,
        1U,
        immediateTransferredBytes_,
        0U,
        &operation_);
    if (status == 0) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::success(
            DashboardConnectionSocketIssueDisposition::
                CompletedSynchronously);
    }
    if (status != SOCKET_ERROR) {
        resetIssuedOperationLocked();
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            integrityError(
                "Issue dashboard connected-socket send with an undefined native status"));
    }

    const int code = api_->lastSocketError();
    if (code == WSA_IO_PENDING) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::success(
            DashboardConnectionSocketIssueDisposition::Pending);
    }
    resetIssuedOperationLocked();
    return Domain::Result<
        DashboardConnectionSocketIssueDisposition>::failure(
        socketError("Issue dashboard connected-socket send", code));
}

Domain::Result<DashboardConnectionSocketCancellationDisposition>
DashboardConnectionSocket::requestCancellation() noexcept
{
    const std::scoped_lock lock{mutex_};
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionSocketState::Idle) {
        return Domain::Result<
            DashboardConnectionSocketCancellationDisposition>::failure(
            conflictError(
                "Cancel an idle dashboard connected socket"));
    }
    if (current ==
            DashboardConnectionSocketState::ReceiveCancellationRequested ||
        current ==
            DashboardConnectionSocketState::SendCancellationRequested) {
        return Domain::Result<
            DashboardConnectionSocketCancellationDisposition>::success(
            DashboardConnectionSocketCancellationDisposition::
                AlreadyRequested);
    }

    const auto cancellationState =
        current == DashboardConnectionSocketState::ReceiveIssued
        ? DashboardConnectionSocketState::ReceiveCancellationRequested
        : DashboardConnectionSocketState::SendCancellationRequested;
    if (api_->cancel(borrowedNativeSocket(), &operation_) != FALSE) {
        state_.store(cancellationState, std::memory_order_release);
        return Domain::Result<
            DashboardConnectionSocketCancellationDisposition>::success(
            DashboardConnectionSocketCancellationDisposition::Requested);
    }

    const DWORD code = api_->lastSystemError();
    if (code == ERROR_NOT_FOUND) {
        state_.store(cancellationState, std::memory_order_release);
        return Domain::Result<
            DashboardConnectionSocketCancellationDisposition>::success(
            DashboardConnectionSocketCancellationDisposition::
                CompletionMayHaveWon);
    }
    return Domain::Result<
        DashboardConnectionSocketCancellationDisposition>::failure(
        cancellationError(code));
}

Domain::Result<DashboardConnectionSocketShutdownDisposition>
DashboardConnectionSocket::shutdownBoth() noexcept
{
    const std::scoped_lock lock{mutex_};
    if (shutdownRequested_.load(std::memory_order_relaxed)) {
        return Domain::Result<
            DashboardConnectionSocketShutdownDisposition>::success(
            DashboardConnectionSocketShutdownDisposition::AlreadyRequested);
    }

    if (api_->shutdownSocket(borrowedNativeSocket(), SD_BOTH) == 0) {
        shutdownRequested_.store(true, std::memory_order_release);
        return Domain::Result<
            DashboardConnectionSocketShutdownDisposition>::success(
            DashboardConnectionSocketShutdownDisposition::Requested);
    }

    const int code = api_->lastSocketError();
    if (code == WSAENOTCONN || code == WSAESHUTDOWN) {
        shutdownRequested_.store(true, std::memory_order_release);
        return Domain::Result<
            DashboardConnectionSocketShutdownDisposition>::success(
            DashboardConnectionSocketShutdownDisposition::AlreadyClosed);
    }
    return Domain::Result<
        DashboardConnectionSocketShutdownDisposition>::failure(
        socketError("Shut down dashboard connected socket", code));
}

Domain::Result<DashboardConnectionSocketReapResult>
DashboardConnectionSocket::reap(
    const DashboardIoCompletionKey completionKey,
    const DWORD transferredBytes,
    OVERLAPPED* const completedOperation,
    const DWORD nativeError) noexcept
{
    const std::scoped_lock lock{mutex_};
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionSocketState::Idle) {
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            conflictError(
                "Reap an idle dashboard connected socket"));
    }
    if (completedOperation != &operation_) {
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            integrityError(
                "Reap a dashboard connected-socket completion with a foreign OVERLAPPED"));
    }

    const bool receive = isReceiveState(current);
    if (!receive && !isSendState(current)) {
        resetIssuedOperationLocked();
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            integrityError(
                "Reap a dashboard connected socket with an undefined operation state"));
    }
    const auto kind = receive
        ? DashboardConnectionSocketOperationKind::Receive
        : DashboardConnectionSocketOperationKind::Send;
    const std::size_t maximumTransferred = receive
        ? ReceiveBufferLength
        : borrowedSendLength_;

    // From this point onward the exact packet has already left the kernel
    // queue. Release all operation and borrowed-send ownership before any
    // typed return, including malformed-key and malformed-byte failures.
    resetIssuedOperationLocked();

    if (!(completionKey == completionKey_)) {
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            integrityError(
                "Reap a dashboard connected-socket completion with the wrong key"));
    }
    if (static_cast<std::size_t>(transferredBytes) > maximumTransferred) {
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            integrityError(
                "Reap a dashboard connected-socket completion beyond its active buffer"));
    }
    if (nativeError != ERROR_SUCCESS) {
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            completionError(nativeError));
    }
    if (transferredBytes == 0U) {
        return Domain::Result<DashboardConnectionSocketReapResult>::failure(
            transportClosedError(
                receive
                    ? "Observe dashboard connected-socket peer EOF"
                    : "Observe a zero-byte dashboard connected-socket send"));
    }

    if (receive) {
        receivedByteCount_ = static_cast<std::size_t>(transferredBytes);
    }
    return Domain::Result<DashboardConnectionSocketReapResult>::success(
        DashboardConnectionSocketReapResult{kind, transferredBytes});
}

std::span<const std::byte> DashboardConnectionSocket::receivedBytes()
    const noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        if (receivedByteCount_ == 0U ||
            receivedByteCount_ > receiveBuffer_.size()) {
            return {};
        }
        return {receiveBuffer_.data(), receivedByteCount_};
    } catch (...) {
        return {};
    }
}

void DashboardConnectionSocket::resetIssuedOperationLocked() noexcept
{
    operation_ = {};
    activeBuffer_ = {};
    receiveFlags_ = 0U;
    immediateTransferredBytes_ = 0U;
    borrowedSendBytes_ = nullptr;
    borrowedSendLength_ = 0U;
    receivedByteCount_ = 0U;
    state_.store(
        DashboardConnectionSocketState::Idle,
        std::memory_order_release);
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
