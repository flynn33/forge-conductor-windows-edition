#include "DashboardWinsockExtensions.h"

#include <MSWSock.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error makeNativeError(
    const std::string_view action,
    const int nativeCode,
    const std::string_view stableCode,
    const bool retryable = false) noexcept
{
    try {
        std::string message{action};
        message += " failed with Winsock error ";
        message += std::to_string(nativeCode);
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard Winsock extension operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error discoveryError(
    const std::string_view extensionName,
    const int nativeCode) noexcept
{
    std::string action;
    try {
        action = "Discover the dashboard ";
        action += extensionName;
        action += " extension";
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard Winsock extension discovery failed.");
    }

    switch (nativeCode) {
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        return makeNativeError(
            action, nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable, true);
    case WSAENOBUFS:
        return makeNativeError(
            action, nativeCode, Domain::ErrorCodes::LimitExceeded, true);
    case WSA_OPERATION_ABORTED:
        return makeNativeError(
            action, nativeCode, Domain::ErrorCodes::Cancelled);
    case WSAEAFNOSUPPORT:
    case WSAENOPROTOOPT:
    case WSAEPROTONOSUPPORT:
    case WSAEOPNOTSUPP:
    case WSAEINVAL:
        return makeNativeError(
            action, nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable);
    default:
        return makeNativeError(
            action, nativeCode, Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error acceptIssueError(const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSA_OPERATION_ABORTED:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::Cancelled);
    case WSAECONNRESET:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::TransportClosed, true);
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::TransportClosed);
    case WSAEMFILE:
    case WSAENOBUFS:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::LimitExceeded, true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable, true);
    case WSAEINPROGRESS:
    case WSAEWOULDBLOCK:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::Conflict, true);
    default:
        return makeNativeError(
            "Issue dashboard AcceptEx", nativeCode,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Result<DWORD> requiredAddressRegionLength(
    const int addressFamily) noexcept
{
    if (addressFamily == AF_INET) {
        return Domain::Result<DWORD>::success(
            DashboardWinsockExtensions::Ipv4AddressRegionLength);
    }
    if (addressFamily == AF_INET6) {
        return Domain::Result<DWORD>::success(
            DashboardWinsockExtensions::Ipv6AddressRegionLength);
    }
    return Domain::Result<DWORD>::failure(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard AcceptEx requires an IPv4 or IPv6 address family."));
}

[[nodiscard]] Domain::Result<void> validateAcceptBuffer(
    const int addressFamily,
    const void* const outputBuffer,
    const DWORD outputBufferLength,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength) noexcept
{
    if (outputBuffer == nullptr) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard AcceptEx requires an output buffer."));
    }
    if (receiveDataLength !=
        DashboardWinsockExtensions::RequiredReceiveDataLength) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard AcceptEx receive length must be zero so acceptance does not wait for request data."));
    }

    const auto requiredLength = requiredAddressRegionLength(addressFamily);
    if (!requiredLength) {
        return Domain::Result<void>::failure(requiredLength.error());
    }
    if (localAddressLength != requiredLength.value() ||
        remoteAddressLength != requiredLength.value()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard AcceptEx address regions must each equal the selected sockaddr size plus 16 bytes."));
    }

    const std::uint64_t requiredBufferLength =
        static_cast<std::uint64_t>(receiveDataLength) +
        static_cast<std::uint64_t>(localAddressLength) +
        static_cast<std::uint64_t>(remoteAddressLength);
    if (requiredBufferLength >
            static_cast<std::uint64_t>(
                (std::numeric_limits<DWORD>::max)()) ||
        requiredBufferLength > outputBufferLength) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard AcceptEx output storage is smaller than its declared framing regions."));
    }
    return Domain::Result<void>::success();
}

template <typename FunctionPointer>
[[nodiscard]] Domain::Result<FunctionPointer> discoverFunction(
    IDashboardWinsockExtensionApi& api,
    const SOCKET socket,
    const GUID& identifier,
    const std::string_view name) noexcept
{
    FunctionPointer function{};
    DWORD bytesReturned{};
    const int status = api.ioctl(
        socket,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &identifier,
        static_cast<DWORD>(sizeof(identifier)),
        &function,
        static_cast<DWORD>(sizeof(function)),
        bytesReturned);
    if (status == SOCKET_ERROR) {
        return Domain::Result<FunctionPointer>::failure(
            discoveryError(name, api.lastError()));
    }
    if (status != 0) {
        return Domain::Result<FunctionPointer>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard Winsock extension discovery returned a nonstandard success status."));
    }
    if (bytesReturned != sizeof(function)) {
        return Domain::Result<FunctionPointer>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Dashboard Winsock extension discovery returned a truncated or oversized function pointer."));
    }
    if (function == nullptr) {
        return Domain::Result<FunctionPointer>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "Dashboard Winsock extension discovery returned a null function pointer."));
    }
    return Domain::Result<FunctionPointer>::success(function);
}

[[nodiscard]] int expectedSocketAddressLength(const int addressFamily) noexcept
{
    return addressFamily == AF_INET
        ? static_cast<int>(sizeof(sockaddr_in))
        : static_cast<int>(sizeof(sockaddr_in6));
}

[[nodiscard]] bool addressRangeIsInsideRegion(
    const void* const buffer,
    const DWORD bufferLength,
    const DWORD regionOffset,
    const DWORD regionLength,
    const sockaddr* const address,
    const int addressLength) noexcept
{
    if (address == nullptr || addressLength <= 0) {
        return false;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(buffer);
    const std::uintptr_t candidate =
        reinterpret_cast<std::uintptr_t>(address);
    if (candidate < base) {
        return false;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(candidate - base);
    const std::uint64_t length = static_cast<std::uint64_t>(addressLength);
    const std::uint64_t regionBegin = regionOffset;
    const std::uint64_t regionEnd =
        regionBegin + static_cast<std::uint64_t>(regionLength);
    const std::uint64_t bufferEnd = bufferLength;
    return offset >= regionBegin &&
        offset <= regionEnd &&
        length <= regionEnd - offset &&
        offset <= bufferEnd &&
        length <= bufferEnd - offset;
}

} // namespace

class DashboardWinsockExtensions::Impl final {
public:
    Impl(
        std::shared_ptr<IDashboardWinsockExtensionApi> api,
        const LPFN_ACCEPTEX acceptEx,
        const LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrs) noexcept
        : api_{std::move(api)},
          acceptEx_{acceptEx},
          getAcceptExSockaddrs_{getAcceptExSockaddrs}
    {
    }

    [[nodiscard]] IDashboardWinsockExtensionApi& api() const noexcept
    {
        return *api_;
    }
    [[nodiscard]] LPFN_ACCEPTEX acceptEx() const noexcept { return acceptEx_; }
    [[nodiscard]] LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrs() const noexcept
    {
        return getAcceptExSockaddrs_;
    }

private:
    std::shared_ptr<IDashboardWinsockExtensionApi> api_;
    LPFN_ACCEPTEX acceptEx_{};
    LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrs_{};
};

int DashboardWinsockExtensionSystemApi::ioctl(
    const SOCKET socket,
    const DWORD controlCode,
    const void* const inputBuffer,
    const DWORD inputBufferLength,
    void* const outputBuffer,
    const DWORD outputBufferLength,
    DWORD& bytesReturned) noexcept
{
    return ::WSAIoctl(
        socket,
        controlCode,
        const_cast<void*>(inputBuffer),
        inputBufferLength,
        outputBuffer,
        outputBufferLength,
        &bytesReturned,
        nullptr,
        nullptr);
}

int DashboardWinsockExtensionSystemApi::lastError() noexcept
{
    return ::WSAGetLastError();
}

DashboardAcceptedSocketAddress::DashboardAcceptedSocketAddress(
    const sockaddr* const address,
    const int addressLength) noexcept
    : length_{addressLength}
{
    std::memcpy(&storage_, address, static_cast<std::size_t>(addressLength));
}

DashboardAcceptedAddresses::DashboardAcceptedAddresses(
    DashboardAcceptedSocketAddress local,
    DashboardAcceptedSocketAddress remote) noexcept
    : local_{std::move(local)}, remote_{std::move(remote)}
{
}

DashboardWinsockExtensions::DashboardWinsockExtensions(
    std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

DashboardWinsockExtensions::~DashboardWinsockExtensions() noexcept = default;

Domain::Result<std::unique_ptr<DashboardWinsockExtensions>>
DashboardWinsockExtensions::discover(const SOCKET discoverySocket) noexcept
{
    try {
        return discover(
            discoverySocket,
            std::make_shared<DashboardWinsockExtensionSystemApi>());
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardWinsockExtensions>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its Winsock extension API owner."));
    }
}

Domain::Result<std::unique_ptr<DashboardWinsockExtensions>>
DashboardWinsockExtensions::discover(
    const SOCKET discoverySocket,
    std::shared_ptr<IDashboardWinsockExtensionApi> api) noexcept
{
    if (discoverySocket == INVALID_SOCKET) {
        return Domain::Result<
            std::unique_ptr<DashboardWinsockExtensions>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard Winsock extension discovery requires a valid socket."));
    }
    if (api == nullptr) {
        return Domain::Result<
            std::unique_ptr<DashboardWinsockExtensions>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard Winsock extension discovery requires a native API dependency."));
    }

    try {
        const GUID acceptExIdentifier = WSAID_ACCEPTEX;
        auto acceptEx = discoverFunction<LPFN_ACCEPTEX>(
            *api, discoverySocket, acceptExIdentifier, "AcceptEx");
        if (!acceptEx) {
            return Domain::Result<
                std::unique_ptr<DashboardWinsockExtensions>>::failure(
                std::move(acceptEx).error());
        }

        const GUID addressIdentifier = WSAID_GETACCEPTEXSOCKADDRS;
        auto getAcceptExSockaddrs =
            discoverFunction<LPFN_GETACCEPTEXSOCKADDRS>(
                *api,
                discoverySocket,
                addressIdentifier,
                "GetAcceptExSockaddrs");
        if (!getAcceptExSockaddrs) {
            return Domain::Result<
                std::unique_ptr<DashboardWinsockExtensions>>::failure(
                std::move(getAcceptExSockaddrs).error());
        }

        auto impl = std::make_unique<Impl>(
            std::move(api), acceptEx.value(), getAcceptExSockaddrs.value());
        return Domain::Result<
            std::unique_ptr<DashboardWinsockExtensions>>::success(
            std::unique_ptr<DashboardWinsockExtensions>{
                new DashboardWinsockExtensions{std::move(impl)}});
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardWinsockExtensions>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not retain its discovered Winsock extensions."));
    }
}

Domain::Result<DashboardAcceptIssueDisposition>
DashboardWinsockExtensions::issueAccept(
    const SOCKET listenerSocket,
    const SOCKET acceptedSocket,
    const int addressFamily,
    void* const outputBuffer,
    const DWORD outputBufferLength,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength,
    OVERLAPPED& operation) noexcept
{
    if (listenerSocket == INVALID_SOCKET ||
        acceptedSocket == INVALID_SOCKET) {
        return Domain::Result<DashboardAcceptIssueDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard AcceptEx requires valid listener and accepted sockets."));
    }
    const auto bufferValidation = validateAcceptBuffer(
        addressFamily,
        outputBuffer,
        outputBufferLength,
        receiveDataLength,
        localAddressLength,
        remoteAddressLength);
    if (!bufferValidation) {
        return Domain::Result<DashboardAcceptIssueDisposition>::failure(
            bufferValidation.error());
    }

    DWORD bytesReceived{};
    const BOOL accepted = impl_->acceptEx()(
        listenerSocket,
        acceptedSocket,
        outputBuffer,
        RequiredReceiveDataLength,
        localAddressLength,
        remoteAddressLength,
        &bytesReceived,
        &operation);
    if (accepted != FALSE) {
        return Domain::Result<DashboardAcceptIssueDisposition>::success(
            DashboardAcceptIssueDisposition::CompletedSynchronously);
    }

    const int nativeCode = impl_->api().lastError();
    if (nativeCode == ERROR_IO_PENDING) {
        return Domain::Result<DashboardAcceptIssueDisposition>::success(
            DashboardAcceptIssueDisposition::Pending);
    }
    return Domain::Result<DashboardAcceptIssueDisposition>::failure(
        acceptIssueError(nativeCode));
}

Domain::Result<DashboardAcceptedAddresses>
DashboardWinsockExtensions::extractAddresses(
    const int addressFamily,
    const void* const outputBuffer,
    const DWORD outputBufferLength,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength) const noexcept
{
    const auto bufferValidation = validateAcceptBuffer(
        addressFamily,
        outputBuffer,
        outputBufferLength,
        receiveDataLength,
        localAddressLength,
        remoteAddressLength);
    if (!bufferValidation) {
        return Domain::Result<DashboardAcceptedAddresses>::failure(
            bufferValidation.error());
    }

    sockaddr* localAddress{};
    sockaddr* remoteAddress{};
    int localAddressLengthActual{};
    int remoteAddressLengthActual{};
    impl_->getAcceptExSockaddrs()(
        const_cast<void*>(outputBuffer),
        RequiredReceiveDataLength,
        localAddressLength,
        remoteAddressLength,
        &localAddress,
        &localAddressLengthActual,
        &remoteAddress,
        &remoteAddressLengthActual);

    const DWORD localRegionOffset = RequiredReceiveDataLength;
    const DWORD remoteRegionOffset =
        RequiredReceiveDataLength + localAddressLength;
    const int expectedLength = expectedSocketAddressLength(addressFamily);
    if (localAddressLengthActual != expectedLength ||
        remoteAddressLengthActual != expectedLength ||
        !addressRangeIsInsideRegion(
            outputBuffer,
            outputBufferLength,
            localRegionOffset,
            localAddressLength,
            localAddress,
            localAddressLengthActual) ||
        !addressRangeIsInsideRegion(
            outputBuffer,
            outputBufferLength,
            remoteRegionOffset,
            remoteAddressLength,
            remoteAddress,
            remoteAddressLengthActual)) {
        return Domain::Result<DashboardAcceptedAddresses>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "GetAcceptExSockaddrs returned an invalid address pointer or length."));
    }
    DashboardAcceptedSocketAddress local{
        localAddress, localAddressLengthActual};
    DashboardAcceptedSocketAddress remote{
        remoteAddress, remoteAddressLengthActual};
    if (local.addressFamily() != addressFamily ||
        remote.addressFamily() != addressFamily) {
        return Domain::Result<DashboardAcceptedAddresses>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "GetAcceptExSockaddrs returned an address from the wrong family."));
    }

    return Domain::Result<DashboardAcceptedAddresses>::success(
        DashboardAcceptedAddresses{
            std::move(local), std::move(remote)});
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
