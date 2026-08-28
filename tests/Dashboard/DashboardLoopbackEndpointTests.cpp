#include "Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.h"

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using AddressFamily = Detail::DashboardLoopbackAddressFamily;
using Endpoint = Detail::DashboardLoopbackEndpoint;

static_assert(std::is_final_v<Endpoint>);
static_assert(std::is_copy_constructible_v<Endpoint>);
static_assert(std::is_move_constructible_v<Endpoint>);

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
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, "failure used the wrong code");
    require(!result.error().retryable, "failure was unexpectedly retryable");
}

template <typename SocketAddress>
[[nodiscard]] SocketAddress copyAddress(const Endpoint& endpoint)
{
    require(
        endpoint.nativeAddressLength() ==
            static_cast<int>(sizeof(SocketAddress)),
        "native address used the wrong length");
    SocketAddress address{};
    std::memcpy(&address, endpoint.nativeAddress(), sizeof(address));
    return address;
}

template <typename SocketAddress>
[[nodiscard]] bool unusedNativeStorageIsZero(
    const Endpoint& endpoint) noexcept
{
    const auto* const bytes = reinterpret_cast<const unsigned char*>(
        endpoint.nativeAddress());
    return std::all_of(
        bytes + sizeof(SocketAddress),
        bytes + sizeof(sockaddr_storage),
        [](const unsigned char value) noexcept { return value == 0U; });
}

void setNetworkPort(
    unsigned short& networkPort,
    const std::uint16_t hostPort) noexcept
{
    auto* const bytes = reinterpret_cast<unsigned char*>(&networkPort);
    bytes[0U] = static_cast<unsigned char>(hostPort >> 8U);
    bytes[1U] = static_cast<unsigned char>(hostPort & 0xffU);
}

[[nodiscard]] sockaddr_in makeIpv4(
    const std::array<unsigned char, 4U> addressBytes,
    const std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    setNetworkPort(address.sin_port, port);
    std::memcpy(
        &address.sin_addr, addressBytes.data(), addressBytes.size());
    return address;
}

[[nodiscard]] sockaddr_in6 makeIpv6(
    const std::array<unsigned char, 16U> addressBytes,
    const std::uint16_t port)
{
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    setNetworkPort(address.sin6_port, port);
    std::memcpy(
        &address.sin6_addr, addressBytes.data(), addressBytes.size());
    return address;
}

template <typename SocketAddress>
[[nodiscard]] const sockaddr* asSockaddr(
    const SocketAddress& address) noexcept
{
    return reinterpret_cast<const sockaddr*>(&address);
}

void constructionAcceptsOnlyTheTwoExactLiterals()
{
    const auto ipv4 = Endpoint::create("127.0.0.1", 1U);
    require(static_cast<bool>(ipv4), "exact IPv4 literal was rejected");
    const auto ipv6 = Endpoint::create("::1", 65535U);
    require(static_cast<bool>(ipv6), "exact IPv6 literal was rejected");

    const std::string embeddedNull{"127.0.0.1\0ignored", 17U};
    const std::vector<std::string_view> rejectedHosts{
        "",
        "localhost",
        "LOCALHOST",
        "0.0.0.0",
        "127.0.0.2",
        "127.0.0.1 ",
        "[::1]",
        "::",
        "0:0:0:0:0:0:0:1",
        "::ffff:127.0.0.1",
        embeddedNull,
    };
    for (const auto host : rejectedHosts) {
        requireError(
            Endpoint::create(host, 17777U),
            Domain::ErrorCodes::InvalidRequest,
            "a noncanonical dashboard host was accepted");
    }
    requireError(
        Endpoint::create("127.0.0.1", 0U),
        Domain::ErrorCodes::InvalidRequest,
        "zero IPv4 port was accepted");
    requireError(
        Endpoint::create("::1", 0U),
        Domain::ErrorCodes::InvalidRequest,
        "zero IPv6 port was accepted");
}

void ipv4ConstructionHasExactNativeBytesAndCanonicalText()
{
    constexpr std::uint16_t Port = 0x1234U;
    const auto endpoint = take(Endpoint::create("127.0.0.1", Port));
    require(endpoint.addressFamily() == AddressFamily::Ipv4,
            "IPv4 endpoint changed address family");
    require(endpoint.host() == "127.0.0.1", "IPv4 host was not canonical");
    require(endpoint.port() == Port, "IPv4 port changed");
    require(endpoint.authority() == "127.0.0.1:4660",
            "IPv4 authority was not canonical");
    require(endpoint.authority().size() <=
                Endpoint::MaximumCanonicalAuthorityCharacters,
            "IPv4 authority exceeded its static bound");

    sockaddr_in expected{};
    expected.sin_family = AF_INET;
    setNetworkPort(expected.sin_port, Port);
    constexpr std::array<unsigned char, 4U> loopback{127U, 0U, 0U, 1U};
    std::memcpy(&expected.sin_addr, loopback.data(), loopback.size());
    require(
        std::memcmp(endpoint.nativeAddress(), &expected, sizeof(expected)) == 0,
        "IPv4 sockaddr bytes were not exact");
    require(unusedNativeStorageIsZero<sockaddr_in>(endpoint),
            "IPv4 sockaddr storage tail was not zeroed");

    const auto observed = copyAddress<sockaddr_in>(endpoint);
    const auto* const portBytes =
        reinterpret_cast<const unsigned char*>(&observed.sin_port);
    require(portBytes[0U] == 0x12U && portBytes[1U] == 0x34U,
            "IPv4 port was not in network byte order");
    require(static_cast<bool>(endpoint.validateBoundAddress(
                asSockaddr(observed), static_cast<int>(sizeof(observed)))),
            "exact IPv4 bound address was rejected");

    auto copied = endpoint;
    require(copied == endpoint, "copied IPv4 endpoint changed value");
    require(copied.nativeAddress() != endpoint.nativeAddress(),
            "copied endpoint aliased native storage");
}

void ipv6ConstructionHasExactNativeBytesAndCanonicalText()
{
    constexpr std::uint16_t Port = 0x5678U;
    const auto endpoint = take(Endpoint::create("::1", Port));
    require(endpoint.addressFamily() == AddressFamily::Ipv6,
            "IPv6 endpoint changed address family");
    require(endpoint.host() == "::1", "IPv6 host was not canonical");
    require(endpoint.port() == Port, "IPv6 port changed");
    require(endpoint.authority() == "[::1]:22136",
            "IPv6 authority was not canonical");
    require(endpoint.authority().size() <=
                Endpoint::MaximumCanonicalAuthorityCharacters,
            "IPv6 authority exceeded its static bound");

    sockaddr_in6 expected{};
    expected.sin6_family = AF_INET6;
    setNetworkPort(expected.sin6_port, Port);
    constexpr std::array<unsigned char, 16U> loopback{
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
    std::memcpy(&expected.sin6_addr, loopback.data(), loopback.size());
    require(
        std::memcmp(endpoint.nativeAddress(), &expected, sizeof(expected)) == 0,
        "IPv6 sockaddr bytes were not exact");
    require(unusedNativeStorageIsZero<sockaddr_in6>(endpoint),
            "IPv6 sockaddr storage tail was not zeroed");

    const auto observed = copyAddress<sockaddr_in6>(endpoint);
    const auto* const portBytes =
        reinterpret_cast<const unsigned char*>(&observed.sin6_port);
    require(portBytes[0U] == 0x56U && portBytes[1U] == 0x78U,
            "IPv6 port was not in network byte order");
    require(static_cast<bool>(endpoint.validateBoundAddress(
                asSockaddr(observed), static_cast<int>(sizeof(observed)))),
            "exact IPv6 bound address was rejected");
}

void ipv4BoundAndPeerValidationRejectsEveryVariant()
{
    const auto endpoint = take(Endpoint::create("127.0.0.1", 17777U));
    constexpr std::array<unsigned char, 4U> loopback{127U, 0U, 0U, 1U};
    constexpr std::array<unsigned char, 4U> alternate{127U, 0U, 0U, 2U};
    constexpr std::array<unsigned char, 4U> wildcard{0U, 0U, 0U, 0U};
    const auto exactBound = makeIpv4(loopback, 17777U);
    const auto exactPeer = makeIpv4(loopback, 49152U);
    require(static_cast<bool>(endpoint.validateBoundAddress(
                asSockaddr(exactBound), static_cast<int>(sizeof(exactBound)))),
            "exact IPv4 bound endpoint was rejected");
    require(static_cast<bool>(endpoint.validatePeerAddress(
                asSockaddr(exactPeer), static_cast<int>(sizeof(exactPeer)))),
            "exact IPv4 peer endpoint was rejected");

    requireError(endpoint.validateBoundAddress(nullptr, sizeof(sockaddr_in)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "null IPv4 bound address was accepted");
    requireError(endpoint.validatePeerAddress(nullptr, sizeof(sockaddr_in)),
                 Domain::ErrorCodes::Unauthorized,
                 "null IPv4 peer address was accepted");
    requireError(endpoint.validateBoundAddress(asSockaddr(exactBound), -1),
                 Domain::ErrorCodes::IntegrityFailure,
                 "negative IPv4 bound size was accepted");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(exactPeer),
                     static_cast<int>(sizeof(exactPeer)) - 1),
                 Domain::ErrorCodes::Unauthorized,
                 "short IPv4 peer size was accepted");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(exactBound),
                     static_cast<int>(sizeof(exactBound)) + 1),
                 Domain::ErrorCodes::IntegrityFailure,
                 "oversized IPv4 bound size was accepted");

    auto wrongFamily = exactBound;
    wrongFamily.sin_family = AF_UNSPEC;
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(wrongFamily), sizeof(wrongFamily)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "wrong IPv4 bound family was accepted");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(wrongFamily), sizeof(wrongFamily)),
                 Domain::ErrorCodes::Unauthorized,
                 "wrong IPv4 peer family was accepted");

    const auto alternateBound = makeIpv4(alternate, 17777U);
    const auto wildcardBound = makeIpv4(wildcard, 17777U);
    const auto wrongPort = makeIpv4(loopback, 17778U);
    const auto zeroPortPeer = makeIpv4(loopback, 0U);
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(alternateBound), sizeof(alternateBound)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "alternate IPv4 loopback was accepted as bound");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(alternateBound), sizeof(alternateBound)),
                 Domain::ErrorCodes::Unauthorized,
                 "alternate IPv4 loopback was accepted as peer");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(wildcardBound), sizeof(wildcardBound)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "IPv4 wildcard was accepted as bound");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(wildcardBound), sizeof(wildcardBound)),
                 Domain::ErrorCodes::Unauthorized,
                 "IPv4 wildcard was accepted as peer");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(wrongPort), sizeof(wrongPort)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "wrong IPv4 bound port was accepted");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(zeroPortPeer), sizeof(zeroPortPeer)),
                 Domain::ErrorCodes::Unauthorized,
                 "zero IPv4 peer port was accepted");

    auto ipv6Loopback = makeIpv6(
        {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
         0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U},
        17777U);
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(ipv6Loopback), sizeof(ipv6Loopback)),
                 Domain::ErrorCodes::Unauthorized,
                 "IPv6 peer was accepted by IPv4 endpoint");
}

void ipv6BoundAndPeerValidationRejectsEveryVariant()
{
    const auto endpoint = take(Endpoint::create("::1", 28888U));
    constexpr std::array<unsigned char, 16U> loopback{
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
    constexpr std::array<unsigned char, 16U> wildcard{};
    constexpr std::array<unsigned char, 16U> alternate{
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U};
    constexpr std::array<unsigned char, 16U> mappedIpv4{
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0xffU, 0xffU, 127U, 0U, 0U, 1U};
    const auto exactBound = makeIpv6(loopback, 28888U);
    const auto exactPeer = makeIpv6(loopback, 49153U);
    require(static_cast<bool>(endpoint.validateBoundAddress(
                asSockaddr(exactBound), sizeof(exactBound))),
            "exact IPv6 bound endpoint was rejected");
    require(static_cast<bool>(endpoint.validatePeerAddress(
                asSockaddr(exactPeer), sizeof(exactPeer))),
            "exact IPv6 peer endpoint was rejected");

    requireError(endpoint.validateBoundAddress(nullptr, sizeof(sockaddr_in6)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "null IPv6 bound address was accepted");
    requireError(endpoint.validatePeerAddress(nullptr, sizeof(sockaddr_in6)),
                 Domain::ErrorCodes::Unauthorized,
                 "null IPv6 peer address was accepted");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(exactBound),
                     static_cast<int>(sizeof(exactBound)) - 1),
                 Domain::ErrorCodes::IntegrityFailure,
                 "short IPv6 bound size was accepted");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(exactPeer),
                     static_cast<int>(sizeof(exactPeer)) + 1),
                 Domain::ErrorCodes::Unauthorized,
                 "oversized IPv6 peer size was accepted");

    auto wrongFamily = exactBound;
    wrongFamily.sin6_family = AF_INET;
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(wrongFamily), sizeof(wrongFamily)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "wrong IPv6 bound family was accepted");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(wrongFamily), sizeof(wrongFamily)),
                 Domain::ErrorCodes::Unauthorized,
                 "wrong IPv6 peer family was accepted");

    const auto wildcardBound = makeIpv6(wildcard, 28888U);
    const auto alternateBound = makeIpv6(alternate, 28888U);
    const auto mappedBound = makeIpv6(mappedIpv4, 28888U);
    const auto wrongPort = makeIpv6(loopback, 28889U);
    const auto zeroPortPeer = makeIpv6(loopback, 0U);
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(wildcardBound), sizeof(wildcardBound)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "IPv6 wildcard was accepted as bound");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(wildcardBound), sizeof(wildcardBound)),
                 Domain::ErrorCodes::Unauthorized,
                 "IPv6 wildcard was accepted as peer");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(alternateBound), sizeof(alternateBound)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "alternate IPv6 loopback was accepted as bound");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(alternateBound), sizeof(alternateBound)),
                 Domain::ErrorCodes::Unauthorized,
                 "alternate IPv6 loopback was accepted as peer");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(mappedBound), sizeof(mappedBound)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "IPv4-mapped IPv6 was accepted as bound");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(mappedBound), sizeof(mappedBound)),
                 Domain::ErrorCodes::Unauthorized,
                 "IPv4-mapped IPv6 was accepted as peer");
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(wrongPort), sizeof(wrongPort)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "wrong IPv6 bound port was accepted");
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(zeroPortPeer), sizeof(zeroPortPeer)),
                 Domain::ErrorCodes::Unauthorized,
                 "zero IPv6 peer port was accepted");

    auto flowInfo = exactPeer;
    flowInfo.sin6_flowinfo = 1U;
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(flowInfo), sizeof(flowInfo)),
                 Domain::ErrorCodes::Unauthorized,
                 "nonzero IPv6 peer flow information was accepted");
    auto scope = exactBound;
    scope.sin6_scope_id = 1U;
    requireError(endpoint.validateBoundAddress(
                     asSockaddr(scope), sizeof(scope)),
                 Domain::ErrorCodes::IntegrityFailure,
                 "scoped IPv6 loopback was accepted as bound");

    const auto ipv4Loopback = makeIpv4({127U, 0U, 0U, 1U}, 28888U);
    requireError(endpoint.validatePeerAddress(
                     asSockaddr(ipv4Loopback), sizeof(ipv4Loopback)),
                 Domain::ErrorCodes::Unauthorized,
                 "IPv4 peer was accepted by IPv6 endpoint");
}

} // namespace

int main()
{
    try {
        constructionAcceptsOnlyTheTwoExactLiterals();
        ipv4ConstructionHasExactNativeBytesAndCanonicalText();
        ipv6ConstructionHasExactNativeBytesAndCanonicalText();
        ipv4BoundAndPeerValidationRejectsEveryVariant();
        ipv6BoundAndPeerValidationRejectsEveryVariant();
        std::cout << "Dashboard loopback endpoint tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard loopback endpoint tests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard loopback endpoint tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
