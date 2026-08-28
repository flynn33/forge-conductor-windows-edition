#include "DashboardAcceptSlot.h"

#include <MSWSock.h>

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] int nativeAddressFamily(
    const DashboardListeningSocket& listener) noexcept
{
    return listener.endpoint().addressFamily() ==
            DashboardLoopbackAddressFamily::Ipv4
        ? AF_INET
        : AF_INET6;
}

[[nodiscard]] DWORD addressRegionLength(const int family) noexcept
{
    return family == AF_INET
        ? DashboardWinsockExtensions::Ipv4AddressRegionLength
        : DashboardWinsockExtensions::Ipv6AddressRegionLength;
}

[[nodiscard]] Domain::Error nativeError(
    const std::string_view action,
    const std::uint64_t nativeCode,
    const std::string_view stableCode,
    const bool retryable = false) noexcept
{
    try {
        std::string message{action};
        message += " failed with native error ";
        message += std::to_string(nativeCode);
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard accept-slot operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error updateContextError(const int code) noexcept
{
    switch (code) {
    case WSA_OPERATION_ABORTED:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::Cancelled);
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::TransportClosed);
    case WSAECONNRESET:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::TransportClosed,
            true);
    case WSAENOBUFS:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    case WSAENOPROTOOPT:
    case WSAEOPNOTSUPP:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::HostCapabilityUnavailable);
    default:
        return nativeError(
            "Apply SO_UPDATE_ACCEPT_CONTEXT",
            static_cast<std::uint64_t>(code),
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error cancellationError(const DWORD code) noexcept
{
    switch (code) {
    case ERROR_OPERATION_ABORTED:
        return nativeError(
            "Request dashboard AcceptEx cancellation",
            code,
            Domain::ErrorCodes::Cancelled);
    case ERROR_INVALID_HANDLE:
        return nativeError(
            "Request dashboard AcceptEx cancellation",
            code,
            Domain::ErrorCodes::TransportClosed);
    case ERROR_ACCESS_DENIED:
        return nativeError(
            "Request dashboard AcceptEx cancellation",
            code,
            Domain::ErrorCodes::Unauthorized);
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return nativeError(
            "Request dashboard AcceptEx cancellation",
            code,
            Domain::ErrorCodes::LimitExceeded,
            true);
    default:
        return nativeError(
            "Request dashboard AcceptEx cancellation",
            code,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error completionError(const DWORD code) noexcept
{
    switch (code) {
    case ERROR_OPERATION_ABORTED:
        return nativeError(
            "Reap dashboard AcceptEx completion",
            code,
            Domain::ErrorCodes::Cancelled);
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_ABORTED:
    case WSAECONNRESET:
        return nativeError(
            "Reap dashboard AcceptEx completion",
            code,
            Domain::ErrorCodes::TransportClosed,
            true);
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
        return nativeError(
            "Reap dashboard AcceptEx completion",
            code,
            Domain::ErrorCodes::TransportClosed);
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case WSAENOBUFS:
        return nativeError(
            "Reap dashboard AcceptEx completion",
            code,
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSAENETDOWN:
    case WSANOTINITIALISED:
        return nativeError(
            "Reap dashboard AcceptEx completion",
            code,
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    default:
        return nativeError(
            "Reap dashboard AcceptEx completion",
            code,
            Domain::ErrorCodes::InternalFailure);
    }
}

} // namespace

int DashboardAcceptSlotSystemApi::updateAcceptContext(
    const SOCKET acceptedSocket,
    const SOCKET listenerSocket) noexcept
{
    return ::setsockopt(
        acceptedSocket,
        SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<const char*>(&listenerSocket),
        static_cast<int>(sizeof(listenerSocket)));
}

BOOL DashboardAcceptSlotSystemApi::cancelAccept(
    const SOCKET listenerSocket,
    OVERLAPPED* const operation) noexcept
{
    return ::CancelIoEx(reinterpret_cast<HANDLE>(listenerSocket), operation);
}

int DashboardAcceptSlotSystemApi::lastSocketError() noexcept
{
    return ::WSAGetLastError();
}

DWORD DashboardAcceptSlotSystemApi::lastSystemError() noexcept
{
    return ::GetLastError();
}

DashboardAcceptedConnection::DashboardAcceptedConnection(
    UniqueDashboardSocket socket,
    DashboardAcceptedAddresses addresses) noexcept
    : socket_{std::move(socket)}, addresses_{std::move(addresses)}
{
}

DashboardAcceptSlot::DashboardAcceptSlot(
    std::shared_ptr<IDashboardAcceptSlotApi> api) noexcept
    : api_{std::move(api)}
{
}

DashboardAcceptSlot::~DashboardAcceptSlot() noexcept
{
    if (state_.load(std::memory_order_acquire) !=
        DashboardAcceptSlotState::Idle) {
        std::terminate();
    }
}

DashboardAcceptSlotState DashboardAcceptSlot::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

DWORD DashboardAcceptSlot::activeAddressBufferLength() const noexcept
{
    const std::scoped_lock lock{mutex_};
    return addressBufferLength_;
}

Domain::Result<std::unique_ptr<DashboardAcceptSlot>>
DashboardAcceptSlot::create() noexcept
{
    try {
        return create(std::make_shared<DashboardAcceptSlotSystemApi>());
    } catch (...) {
        return Domain::Result<std::unique_ptr<DashboardAcceptSlot>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its accept-slot API owner."));
    }
}

Domain::Result<std::unique_ptr<DashboardAcceptSlot>>
DashboardAcceptSlot::create(
    std::shared_ptr<IDashboardAcceptSlotApi> api) noexcept
{
    if (api == nullptr) {
        return Domain::Result<std::unique_ptr<DashboardAcceptSlot>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A dashboard accept slot requires a native API dependency."));
    }
    try {
        return Domain::Result<std::unique_ptr<DashboardAcceptSlot>>::success(
            std::unique_ptr<DashboardAcceptSlot>{
                new DashboardAcceptSlot{std::move(api)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<DashboardAcceptSlot>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate a stable accept slot."));
    }
}

Domain::Result<DashboardAcceptIssueDisposition> DashboardAcceptSlot::issue(
    DashboardWinsockRuntime& runtime,
    DashboardWinsockExtensions& extensions,
    const DashboardListeningSocket& listener) noexcept
{
    const std::scoped_lock lock{mutex_};
    if (state_.load(std::memory_order_relaxed) !=
        DashboardAcceptSlotState::Idle) {
        return Domain::Result<DashboardAcceptIssueDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "A dashboard accept slot cannot be reused before its matching completion is reaped."));
    }
    if (listener.borrowedNativeSocket() == INVALID_SOCKET) {
        return Domain::Result<DashboardAcceptIssueDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A dashboard accept slot requires a live listener socket."));
    }

    try {
        const int family = nativeAddressFamily(listener);
        auto socketResult = runtime.createOverlappedTcpSocket(family);
        if (!socketResult) {
            return Domain::Result<DashboardAcceptIssueDisposition>::failure(
                std::move(socketResult).error());
        }

        acceptedSocket_.emplace(std::move(socketResult).value());
        listenerSocket_ = listener.borrowedNativeSocket();
        listenerForceClosed_ = false;
        addressFamily_ = family;
        addressRegionLength_ = addressRegionLength(family);
        addressBufferLength_ = addressRegionLength_ * 2U;
        listenerEndpoint_.emplace(listener.endpoint());
        std::fill(addressBuffer_.begin(), addressBuffer_.end(), std::byte{});
        operation_ = {};

        // Publish every field a completion needs before calling AcceptEx. An
        // already-associated listener may dispatch its completion on another
        // IOCP worker before issueAccept returns.
        state_.store(
            DashboardAcceptSlotState::Issued, std::memory_order_release);

        auto issued = extensions.issueAccept(
            listenerSocket_,
            acceptedSocket_->get(),
            addressFamily_,
            addressBuffer_.data(),
            addressBufferLength_,
            DashboardWinsockExtensions::RequiredReceiveDataLength,
            addressRegionLength_,
            addressRegionLength_,
            operation_);
        if (!issued) {
            auto error = std::move(issued).error();
            resetAfterReap();
            return Domain::Result<DashboardAcceptIssueDisposition>::failure(
                std::move(error));
        }

        return issued;
    } catch (...) {
        resetAfterReap();
        return Domain::Result<DashboardAcceptIssueDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard accept slot could not issue AcceptEx."));
    }
}

Domain::Result<DashboardAcceptCancellationDisposition>
DashboardAcceptSlot::requestCancellation() noexcept
{
    const std::scoped_lock lock{mutex_};
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardAcceptSlotState::Idle) {
        return Domain::Result<
            DashboardAcceptCancellationDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "An idle dashboard accept slot has no operation to cancel."));
    }
    if (current == DashboardAcceptSlotState::CancellationRequested) {
        return Domain::Result<DashboardAcceptCancellationDisposition>::success(
            DashboardAcceptCancellationDisposition::AlreadyRequested);
    }

    if (api_->cancelAccept(listenerSocket_, &operation_) != FALSE) {
        state_.store(
            DashboardAcceptSlotState::CancellationRequested,
            std::memory_order_release);
        return Domain::Result<DashboardAcceptCancellationDisposition>::success(
            DashboardAcceptCancellationDisposition::Requested);
    }

    const DWORD code = api_->lastSystemError();
    if (code == ERROR_NOT_FOUND) {
        state_.store(
            DashboardAcceptSlotState::CancellationRequested,
            std::memory_order_release);
        return Domain::Result<DashboardAcceptCancellationDisposition>::success(
            DashboardAcceptCancellationDisposition::CompletionMayHaveWon);
    }
    return Domain::Result<DashboardAcceptCancellationDisposition>::failure(
        cancellationError(code));
}

void DashboardAcceptSlot::recordListenerCloseCancellation() noexcept
{
    const std::scoped_lock lock{mutex_};
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardAcceptSlotState::Issued) {
        state_.store(
            DashboardAcceptSlotState::CancellationRequested,
            std::memory_order_release);
    }
    if (current == DashboardAcceptSlotState::Issued ||
        current == DashboardAcceptSlotState::CancellationRequested) {
        // The RAII listener owner has closed this handle. Never retain its
        // numeric value because Winsock may reuse it before a late success
        // completion reaches this slot.
        listenerSocket_ = INVALID_SOCKET;
        listenerForceClosed_ = true;
    }
}

Domain::Result<void> DashboardAcceptSlot::validateCompletion(
    const OVERLAPPED* const completedOperation) const noexcept
{
    if (state_.load(std::memory_order_relaxed) ==
        DashboardAcceptSlotState::Idle) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "An idle dashboard accept slot has no completion to reap."));
    }
    if (completedOperation != &operation_) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A dashboard accept completion did not match its slot OVERLAPPED."));
    }
    const bool listenerStateValid = listenerForceClosed_
        ? listenerSocket_ == INVALID_SOCKET
        : listenerSocket_ != INVALID_SOCKET;
    if (!acceptedSocket_.has_value() ||
        !static_cast<bool>(*acceptedSocket_) ||
        !listenerStateValid ||
        (addressFamily_ != AF_INET && addressFamily_ != AF_INET6) ||
        addressRegionLength_ == 0U ||
        addressBufferLength_ == 0U ||
        !listenerEndpoint_.has_value()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A dashboard accept slot lost issued-operation ownership state."));
    }
    return Domain::Result<void>::success();
}

Domain::Result<DashboardAcceptedConnection>
DashboardAcceptSlot::reapSuccessful(
    DashboardWinsockExtensions& extensions,
    OVERLAPPED* const completedOperation) noexcept
{
    const std::scoped_lock lock{mutex_};
    const auto validation = validateCompletion(completedOperation);
    if (!validation) {
        return Domain::Result<DashboardAcceptedConnection>::failure(
            validation.error());
    }

    if (listenerForceClosed_) {
        resetAfterReap();
        return Domain::Result<DashboardAcceptedConnection>::failure(
            completionError(ERROR_OPERATION_ABORTED));
    }

    if (api_->updateAcceptContext(
            acceptedSocket_->get(), listenerSocket_) == SOCKET_ERROR) {
        const int code = api_->lastSocketError();
        resetAfterReap();
        return Domain::Result<DashboardAcceptedConnection>::failure(
            updateContextError(code));
    }

    auto addresses = extensions.extractAddresses(
        addressFamily_,
        addressBuffer_.data(),
        addressBufferLength_,
        DashboardWinsockExtensions::RequiredReceiveDataLength,
        addressRegionLength_,
        addressRegionLength_);
    if (!addresses) {
        auto error = std::move(addresses).error();
        resetAfterReap();
        return Domain::Result<DashboardAcceptedConnection>::failure(
            std::move(error));
    }

    const auto localValidation = listenerEndpoint_->validateBoundAddress(
        addresses.value().local().nativeAddress(),
        addresses.value().local().nativeAddressLength());
    if (!localValidation) {
        auto error = localValidation.error();
        resetAfterReap();
        return Domain::Result<DashboardAcceptedConnection>::failure(
            std::move(error));
    }
    const auto peerValidation = listenerEndpoint_->validatePeerAddress(
        addresses.value().remote().nativeAddress(),
        addresses.value().remote().nativeAddressLength());
    if (!peerValidation) {
        auto error = peerValidation.error();
        resetAfterReap();
        return Domain::Result<DashboardAcceptedConnection>::failure(
            std::move(error));
    }

    DashboardAcceptedConnection connection{
        std::move(*acceptedSocket_), std::move(addresses).value()};
    resetAfterReap();
    return Domain::Result<DashboardAcceptedConnection>::success(
        std::move(connection));
}

Domain::Result<void> DashboardAcceptSlot::reapFailed(
    OVERLAPPED* const completedOperation,
    const DWORD nativeCode) noexcept
{
    const std::scoped_lock lock{mutex_};
    const auto validation = validateCompletion(completedOperation);
    if (!validation) {
        return validation;
    }

    resetAfterReap();
    return Domain::Result<void>::failure(completionError(nativeCode));
}

void DashboardAcceptSlot::resetAfterReap() noexcept
{
    acceptedSocket_.reset();
    listenerSocket_ = INVALID_SOCKET;
    listenerForceClosed_ = false;
    addressFamily_ = AF_UNSPEC;
    addressRegionLength_ = 0U;
    addressBufferLength_ = 0U;
    listenerEndpoint_.reset();
    std::fill(addressBuffer_.begin(), addressBuffer_.end(), std::byte{});
    operation_ = {};
    state_.store(DashboardAcceptSlotState::Idle, std::memory_order_release);
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
