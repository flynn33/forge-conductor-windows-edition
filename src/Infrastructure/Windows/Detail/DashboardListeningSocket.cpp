#include "DashboardListeningSocket.h"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] int nativeAddressFamily(
    const DashboardLoopbackEndpoint& endpoint) noexcept
{
    return endpoint.addressFamily() == DashboardLoopbackAddressFamily::Ipv4
        ? AF_INET
        : AF_INET6;
}

[[nodiscard]] Domain::Error nativeError(
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
            "A dashboard listener operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error optionError(
    const std::string_view option,
    const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSAENOBUFS:
        return nativeError(
            option, nativeCode, Domain::ErrorCodes::LimitExceeded, true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSASYSNOTREADY:
        return nativeError(
            option,
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    case WSAEAFNOSUPPORT:
    case WSAENOPROTOOPT:
    case WSAEPROTONOSUPPORT:
        return nativeError(
            option,
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable);
    default:
        return nativeError(
            option, nativeCode, Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error bindError(const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSAEADDRINUSE:
        return nativeError(
            "Bind the dashboard loopback listener",
            nativeCode,
            Domain::ErrorCodes::Conflict,
            true);
    case WSAEACCES:
        return nativeError(
            "Bind the dashboard loopback listener",
            nativeCode,
            Domain::ErrorCodes::Unauthorized);
    case WSAENOBUFS:
        return nativeError(
            "Bind the dashboard loopback listener",
            nativeCode,
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSASYSNOTREADY:
        return nativeError(
            "Bind the dashboard loopback listener",
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    case WSAEADDRNOTAVAIL:
    case WSAEAFNOSUPPORT:
        return nativeError(
            "Bind the dashboard loopback listener",
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable);
    default:
        return nativeError(
            "Bind the dashboard loopback listener",
            nativeCode,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error socketNameError(const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSAENOBUFS:
        return nativeError(
            "Read the bound dashboard listener address",
            nativeCode,
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSASYSNOTREADY:
        return nativeError(
            "Read the bound dashboard listener address",
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    default:
        return nativeError(
            "Read the bound dashboard listener address",
            nativeCode,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error listenError(const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSAEADDRINUSE:
        return nativeError(
            "Listen on the dashboard loopback socket",
            nativeCode,
            Domain::ErrorCodes::Conflict,
            true);
    case WSAEMFILE:
    case WSAENOBUFS:
        return nativeError(
            "Listen on the dashboard loopback socket",
            nativeCode,
            Domain::ErrorCodes::LimitExceeded,
            true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSASYSNOTREADY:
        return nativeError(
            "Listen on the dashboard loopback socket",
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable,
            true);
    case WSAEOPNOTSUPP:
        return nativeError(
            "Listen on the dashboard loopback socket",
            nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable);
    default:
        return nativeError(
            "Listen on the dashboard loopback socket",
            nativeCode,
            Domain::ErrorCodes::InternalFailure);
    }
}

template <typename ErrorFactory>
[[nodiscard]] Domain::Result<DashboardListeningSocket> failNativeOperation(
    UniqueDashboardSocket& socket,
    IDashboardListeningSocketApi& api,
    ErrorFactory&& errorFactory)
{
    const int code = api.lastError();
    socket.reset();
    return Domain::Result<DashboardListeningSocket>::failure(
        std::forward<ErrorFactory>(errorFactory)(code));
}

} // namespace

int DashboardListeningSocketSystemApi::setSocketOption(
    const SOCKET socket,
    const int level,
    const int optionName,
    const char* const optionValue,
    const int optionLength) noexcept
{
    return ::setsockopt(
        socket, level, optionName, optionValue, optionLength);
}

int DashboardListeningSocketSystemApi::bindSocket(
    const SOCKET socket,
    const sockaddr* const address,
    const int addressLength) noexcept
{
    return ::bind(socket, address, addressLength);
}

int DashboardListeningSocketSystemApi::getSocketName(
    const SOCKET socket,
    sockaddr* const address,
    int& addressLength) noexcept
{
    return ::getsockname(socket, address, &addressLength);
}

int DashboardListeningSocketSystemApi::listenSocket(
    const SOCKET socket,
    const int backlog) noexcept
{
    return ::listen(socket, backlog);
}

int DashboardListeningSocketSystemApi::lastError() noexcept
{
    return ::WSAGetLastError();
}

DashboardListeningSocket::DashboardListeningSocket(
    UniqueDashboardSocket socket,
    const DashboardLoopbackEndpoint& endpoint) noexcept
    : socket_{std::move(socket)}, endpoint_{endpoint}
{
}

DashboardListeningSocket::DashboardListeningSocket(
    DashboardListeningSocket&& other) noexcept
    : socket_{std::move(other.socket_)}, endpoint_{other.endpoint_}
{
}

Domain::Result<DashboardListeningSocket> DashboardListeningSocket::create(
    DashboardWinsockRuntime& runtime,
    const DashboardLoopbackEndpoint& endpoint) noexcept
{
    try {
        return create(
            runtime,
            endpoint,
            std::make_shared<DashboardListeningSocketSystemApi>());
    } catch (...) {
        return Domain::Result<DashboardListeningSocket>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its listener API owner."));
    }
}

Domain::Result<DashboardListeningSocket> DashboardListeningSocket::create(
    DashboardWinsockRuntime& runtime,
    const DashboardLoopbackEndpoint& endpoint,
    std::shared_ptr<IDashboardListeningSocketApi> api) noexcept
{
    if (api == nullptr) {
        return Domain::Result<DashboardListeningSocket>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard listening socket requires a native API dependency."));
    }

    try {
        auto socketResult = runtime.createOverlappedTcpSocket(
            nativeAddressFamily(endpoint));
        if (!socketResult) {
            return Domain::Result<DashboardListeningSocket>::failure(
                std::move(socketResult).error());
        }
        auto socket = std::move(socketResult).value();

        const BOOL enabled = TRUE;
        if (api->setSocketOption(
                socket.get(),
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&enabled),
                static_cast<int>(sizeof(enabled))) == SOCKET_ERROR) {
            return failNativeOperation(
                socket,
                *api,
                [](const int code) noexcept {
                    return optionError(
                        "Set SO_EXCLUSIVEADDRUSE on the dashboard listener",
                        code);
                });
        }

        if (endpoint.addressFamily() == DashboardLoopbackAddressFamily::Ipv6 &&
            api->setSocketOption(
                socket.get(),
                IPPROTO_IPV6,
                IPV6_V6ONLY,
                reinterpret_cast<const char*>(&enabled),
                static_cast<int>(sizeof(enabled))) == SOCKET_ERROR) {
            return failNativeOperation(
                socket,
                *api,
                [](const int code) noexcept {
                    return optionError(
                        "Set IPV6_V6ONLY on the dashboard listener", code);
                });
        }

        if (api->bindSocket(
                socket.get(),
                endpoint.nativeAddress(),
                endpoint.nativeAddressLength()) == SOCKET_ERROR) {
            return failNativeOperation(
                socket,
                *api,
                [](const int code) noexcept { return bindError(code); });
        }

        sockaddr_storage observedAddress{};
        int observedAddressLength = static_cast<int>(sizeof(observedAddress));
        if (api->getSocketName(
                socket.get(),
                reinterpret_cast<sockaddr*>(&observedAddress),
                observedAddressLength) == SOCKET_ERROR) {
            return failNativeOperation(
                socket,
                *api,
                [](const int code) noexcept { return socketNameError(code); });
        }

        auto validation = endpoint.validateBoundAddress(
            reinterpret_cast<const sockaddr*>(&observedAddress),
            observedAddressLength);
        if (!validation) {
            auto error = std::move(validation).error();
            socket.reset();
            return Domain::Result<DashboardListeningSocket>::failure(
                std::move(error));
        }

        if (api->listenSocket(socket.get(), ListenBacklog) == SOCKET_ERROR) {
            return failNativeOperation(
                socket,
                *api,
                [](const int code) noexcept { return listenError(code); });
        }

        return Domain::Result<DashboardListeningSocket>::success(
            DashboardListeningSocket{std::move(socket), endpoint});
    } catch (...) {
        return Domain::Result<DashboardListeningSocket>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard listening socket could not be configured."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
