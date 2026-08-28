#include "DashboardLoopbackEndpoint.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::string_view Ipv4LoopbackLiteral = "127.0.0.1";
constexpr std::string_view Ipv6LoopbackLiteral = "::1";
constexpr std::array<unsigned char, 4U> Ipv4LoopbackBytes{127U, 0U, 0U, 1U};
constexpr std::array<unsigned char, 16U> Ipv6LoopbackBytes{
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};

static_assert(
    std::endian::native == std::endian::little ||
        std::endian::native == std::endian::big,
    "Dashboard endpoint port conversion requires a conventional byte order.");

[[nodiscard]] constexpr std::uint16_t toNetworkPort(
    const std::uint16_t value) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(value << 8U) |
            static_cast<std::uint16_t>(value >> 8U));
    } else {
        return value;
    }
}

[[nodiscard]] Domain::Error invalidConfigurationError(
    const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        std::string{message});
}

[[nodiscard]] Domain::Error boundIntegrityError(
    const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        std::string{message});
}

[[nodiscard]] Domain::Error peerAuthorizationError(
    const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        std::string{message});
}

[[nodiscard]] Domain::Error internalError(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::string{message});
}

[[nodiscard]] bool hasExactBytes(
    const void* const actual,
    const std::span<const unsigned char> expected) noexcept
{
    return std::memcmp(actual, expected.data(), expected.size()) == 0;
}

template <typename SocketAddress>
[[nodiscard]] bool hasExactAddressLength(const int addressLength) noexcept
{
    return addressLength == static_cast<int>(sizeof(SocketAddress));
}

[[nodiscard]] Domain::Result<void> validateIpv4Address(
    const sockaddr* const address,
    const int addressLength,
    const std::uint16_t expectedLocalPort,
    const bool boundAddress)
{
    const auto reject = [boundAddress](const std::string_view message) {
        return Domain::Result<void>::failure(
            boundAddress ? boundIntegrityError(message)
                         : peerAuthorizationError(message));
    };

    if (address == nullptr || !hasExactAddressLength<sockaddr_in>(addressLength)) {
        return reject(boundAddress
                ? "The bound dashboard IPv4 address had an invalid size."
                : "The dashboard IPv4 peer address had an invalid size.");
    }

    sockaddr_in observed{};
    std::memcpy(&observed, address, sizeof(observed));
    if (observed.sin_family != AF_INET) {
        return reject(boundAddress
                ? "The bound dashboard address was not IPv4."
                : "The dashboard peer address was not IPv4.");
    }
    if (!hasExactBytes(&observed.sin_addr, Ipv4LoopbackBytes)) {
        return reject(boundAddress
                ? "The bound dashboard IPv4 address was not 127.0.0.1."
                : "The dashboard IPv4 peer was not 127.0.0.1.");
    }

    const auto observedPort = toNetworkPort(observed.sin_port);
    if ((boundAddress && observedPort != expectedLocalPort) ||
        (!boundAddress && observedPort == 0U)) {
        return reject(boundAddress
                ? "The bound dashboard IPv4 port did not match its configuration."
                : "The dashboard IPv4 peer used an invalid source port.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateIpv6Address(
    const sockaddr* const address,
    const int addressLength,
    const std::uint16_t expectedLocalPort,
    const bool boundAddress)
{
    const auto reject = [boundAddress](const std::string_view message) {
        return Domain::Result<void>::failure(
            boundAddress ? boundIntegrityError(message)
                         : peerAuthorizationError(message));
    };

    if (address == nullptr || !hasExactAddressLength<sockaddr_in6>(addressLength)) {
        return reject(boundAddress
                ? "The bound dashboard IPv6 address had an invalid size."
                : "The dashboard IPv6 peer address had an invalid size.");
    }

    sockaddr_in6 observed{};
    std::memcpy(&observed, address, sizeof(observed));
    if (observed.sin6_family != AF_INET6) {
        return reject(boundAddress
                ? "The bound dashboard address was not IPv6."
                : "The dashboard peer address was not IPv6.");
    }
    if (!hasExactBytes(&observed.sin6_addr, Ipv6LoopbackBytes)) {
        return reject(boundAddress
                ? "The bound dashboard IPv6 address was not ::1."
                : "The dashboard IPv6 peer was not ::1.");
    }
    if (observed.sin6_flowinfo != 0U || observed.sin6_scope_id != 0U) {
        return reject(boundAddress
                ? "The bound dashboard IPv6 address had noncanonical metadata."
                : "The dashboard IPv6 peer address had noncanonical metadata.");
    }

    const auto observedPort = toNetworkPort(observed.sin6_port);
    if ((boundAddress && observedPort != expectedLocalPort) ||
        (!boundAddress && observedPort == 0U)) {
        return reject(boundAddress
                ? "The bound dashboard IPv6 port did not match its configuration."
                : "The dashboard IPv6 peer used an invalid source port.");
    }
    return Domain::Result<void>::success();
}

} // namespace

Domain::Result<DashboardLoopbackEndpoint>
DashboardLoopbackEndpoint::createIpv4(const std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = toNetworkPort(port);
    std::memcpy(
        &address.sin_addr,
        Ipv4LoopbackBytes.data(),
        Ipv4LoopbackBytes.size());

    sockaddr_storage storage{};
    std::memcpy(&storage, &address, sizeof(address));

    std::array<char,
        DashboardLoopbackEndpoint::MaximumCanonicalAuthorityCharacters + 1U>
        authority{};
    auto* output = authority.data();
    output = std::copy(
        Ipv4LoopbackLiteral.begin(), Ipv4LoopbackLiteral.end(), output);
    *output++ = ':';
    const auto converted = std::to_chars(
        output, authority.data() + authority.size() - 1U, port);
    if (converted.ec != std::errc{}) {
        return Domain::Result<DashboardLoopbackEndpoint>::failure(
            internalError("The dashboard IPv4 authority could not be encoded."));
    }
    output = converted.ptr;

    return Domain::Result<DashboardLoopbackEndpoint>::success(
        DashboardLoopbackEndpoint{
            DashboardLoopbackAddressFamily::Ipv4,
            port,
            storage,
            static_cast<int>(sizeof(address)),
            authority,
            static_cast<std::size_t>(output - authority.data())});
}

Domain::Result<DashboardLoopbackEndpoint>
DashboardLoopbackEndpoint::createIpv6(const std::uint16_t port)
{
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = toNetworkPort(port);
    std::memcpy(
        &address.sin6_addr,
        Ipv6LoopbackBytes.data(),
        Ipv6LoopbackBytes.size());

    sockaddr_storage storage{};
    std::memcpy(&storage, &address, sizeof(address));

    std::array<char,
        DashboardLoopbackEndpoint::MaximumCanonicalAuthorityCharacters + 1U>
        authority{};
    auto* output = authority.data();
    *output++ = '[';
    output = std::copy(
        Ipv6LoopbackLiteral.begin(), Ipv6LoopbackLiteral.end(), output);
    *output++ = ']';
    *output++ = ':';
    const auto converted = std::to_chars(
        output, authority.data() + authority.size() - 1U, port);
    if (converted.ec != std::errc{}) {
        return Domain::Result<DashboardLoopbackEndpoint>::failure(
            internalError("The dashboard IPv6 authority could not be encoded."));
    }
    output = converted.ptr;

    return Domain::Result<DashboardLoopbackEndpoint>::success(
        DashboardLoopbackEndpoint{
            DashboardLoopbackAddressFamily::Ipv6,
            port,
            storage,
            static_cast<int>(sizeof(address)),
            authority,
            static_cast<std::size_t>(output - authority.data())});
}

DashboardLoopbackEndpoint::DashboardLoopbackEndpoint(
    const DashboardLoopbackAddressFamily family,
    const std::uint16_t port,
    const sockaddr_storage nativeAddress,
    const int nativeAddressLength,
    std::array<char, MaximumCanonicalAuthorityCharacters + 1U> authority,
    const std::size_t authorityLength) noexcept
    : family_{family},
      port_{port},
      nativeAddress_{nativeAddress},
      nativeAddressLength_{nativeAddressLength},
      authority_{std::move(authority)},
      authorityLength_{authorityLength}
{
}

Domain::Result<DashboardLoopbackEndpoint> DashboardLoopbackEndpoint::create(
    const std::string_view literalHost,
    const std::uint16_t port) noexcept
{
    try {
        if (port == 0U) {
            return Domain::Result<DashboardLoopbackEndpoint>::failure(
                invalidConfigurationError(
                    "The dashboard loopback port must be nonzero."));
        }
        if (literalHost == Ipv4LoopbackLiteral) {
            return createIpv4(port);
        }
        if (literalHost == Ipv6LoopbackLiteral) {
            return createIpv6(port);
        }
        return Domain::Result<DashboardLoopbackEndpoint>::failure(
            invalidConfigurationError(
                "The dashboard host must be the literal 127.0.0.1 or ::1."));
    } catch (...) {
        return Domain::Result<DashboardLoopbackEndpoint>::failure(
            internalError(
                "The dashboard loopback endpoint could not be constructed."));
    }
}

std::string_view DashboardLoopbackEndpoint::host() const noexcept
{
    return family_ == DashboardLoopbackAddressFamily::Ipv4
        ? Ipv4LoopbackLiteral
        : Ipv6LoopbackLiteral;
}

Domain::Result<void> DashboardLoopbackEndpoint::validateBoundAddress(
    const sockaddr* const address,
    const int addressLength) const noexcept
{
    try {
        return family_ == DashboardLoopbackAddressFamily::Ipv4
            ? validateIpv4Address(address, addressLength, port_, true)
            : validateIpv6Address(address, addressLength, port_, true);
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "The bound dashboard endpoint could not be validated."));
    }
}

Domain::Result<void> DashboardLoopbackEndpoint::validatePeerAddress(
    const sockaddr* const address,
    const int addressLength) const noexcept
{
    try {
        return family_ == DashboardLoopbackAddressFamily::Ipv4
            ? validateIpv4Address(address, addressLength, port_, false)
            : validateIpv6Address(address, addressLength, port_, false);
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "The dashboard peer endpoint could not be validated."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
