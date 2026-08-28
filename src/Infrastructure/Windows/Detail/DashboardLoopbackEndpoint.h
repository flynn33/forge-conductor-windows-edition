#pragma once

#include "ForgeConductor/Domain/Result.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardLoopbackAddressFamily : std::uint8_t {
    Ipv4,
    Ipv6,
};

// Immutable native endpoint used by one dashboard listener generation. The
// factory recognizes only the two literal loopback spellings fixed by the
// dashboard protocol and constructs their sockaddr values without name
// resolution or Winsock process ownership.
class DashboardLoopbackEndpoint final {
public:
    static constexpr std::size_t MaximumCanonicalAuthorityCharacters = 15U;

    [[nodiscard]] static Domain::Result<DashboardLoopbackEndpoint> create(
        std::string_view literalHost,
        std::uint16_t port) noexcept;

    DashboardLoopbackEndpoint(const DashboardLoopbackEndpoint&) = default;
    DashboardLoopbackEndpoint(DashboardLoopbackEndpoint&&) noexcept = default;
    DashboardLoopbackEndpoint& operator=(
        const DashboardLoopbackEndpoint&) = default;
    DashboardLoopbackEndpoint& operator=(
        DashboardLoopbackEndpoint&&) noexcept = default;
    ~DashboardLoopbackEndpoint() noexcept = default;

    [[nodiscard]] DashboardLoopbackAddressFamily addressFamily()
        const noexcept
    {
        return family_;
    }

    [[nodiscard]] std::string_view host() const noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::string_view authority() const noexcept
    {
        return {authority_.data(), authorityLength_};
    }

    [[nodiscard]] const sockaddr* nativeAddress() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&nativeAddress_);
    }

    [[nodiscard]] int nativeAddressLength() const noexcept
    {
        return nativeAddressLength_;
    }

    // Validates the exact address and configured local port returned by bind,
    // getsockname, or AcceptEx local-address extraction.
    [[nodiscard]] Domain::Result<void> validateBoundAddress(
        const sockaddr* address,
        int addressLength) const noexcept;

    // Validates a connected peer in the same explicit address family. A peer
    // must use the exact loopback address and a nonzero ephemeral/source port;
    // it is not expected to use the listener's local port.
    [[nodiscard]] Domain::Result<void> validatePeerAddress(
        const sockaddr* address,
        int addressLength) const noexcept;

    [[nodiscard]] bool operator==(
        const DashboardLoopbackEndpoint& other) const noexcept
    {
        return family_ == other.family_ && port_ == other.port_;
    }

private:
    [[nodiscard]] static Domain::Result<DashboardLoopbackEndpoint> createIpv4(
        std::uint16_t port);

    [[nodiscard]] static Domain::Result<DashboardLoopbackEndpoint> createIpv6(
        std::uint16_t port);

    DashboardLoopbackEndpoint(
        DashboardLoopbackAddressFamily family,
        std::uint16_t port,
        sockaddr_storage nativeAddress,
        int nativeAddressLength,
        std::array<char, MaximumCanonicalAuthorityCharacters + 1U> authority,
        std::size_t authorityLength) noexcept;

    DashboardLoopbackAddressFamily family_{};
    std::uint16_t port_{};
    sockaddr_storage nativeAddress_{};
    int nativeAddressLength_{};
    std::array<char, MaximumCanonicalAuthorityCharacters + 1U> authority_{};
    std::size_t authorityLength_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
