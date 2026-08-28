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

#include <cstddef>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Narrow query/error seam for Winsock extension discovery. Production uses
// DashboardWinsockExtensionSystemApi. Tests inject the exact pointers returned
// by WSAIoctl without exposing them through DashboardWinsockExtensions.
class IDashboardWinsockExtensionApi {
public:
    virtual ~IDashboardWinsockExtensionApi() = default;

    [[nodiscard]] virtual int ioctl(
        SOCKET socket,
        DWORD controlCode,
        const void* inputBuffer,
        DWORD inputBufferLength,
        void* outputBuffer,
        DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept = 0;
    [[nodiscard]] virtual int lastError() noexcept = 0;
};

class DashboardWinsockExtensionSystemApi final
    : public IDashboardWinsockExtensionApi {
public:
    [[nodiscard]] int ioctl(
        SOCKET socket,
        DWORD controlCode,
        const void* inputBuffer,
        DWORD inputBufferLength,
        void* outputBuffer,
        DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept override;
    [[nodiscard]] int lastError() noexcept override;
};

enum class DashboardAcceptIssueDisposition : unsigned char {
    CompletedSynchronously,
    Pending,
};

// An owned copy of one address extracted from the AcceptEx output buffer.
// Copying removes any lifetime dependency on the reusable accept-slot buffer.
class DashboardAcceptedSocketAddress final {
public:
    [[nodiscard]] const sockaddr* nativeAddress() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&storage_);
    }
    [[nodiscard]] int nativeAddressLength() const noexcept { return length_; }
    [[nodiscard]] int addressFamily() const noexcept
    {
        return nativeAddress()->sa_family;
    }

private:
    friend class DashboardWinsockExtensions;

    DashboardAcceptedSocketAddress(
        const sockaddr* address,
        int addressLength) noexcept;

    sockaddr_storage storage_{};
    int length_{};
};

class DashboardAcceptedAddresses final {
public:
    [[nodiscard]] const DashboardAcceptedSocketAddress& local() const noexcept
    {
        return local_;
    }
    [[nodiscard]] const DashboardAcceptedSocketAddress& remote() const noexcept
    {
        return remote_;
    }

private:
    friend class DashboardWinsockExtensions;

    DashboardAcceptedAddresses(
        DashboardAcceptedSocketAddress local,
        DashboardAcceptedSocketAddress remote) noexcept;

    DashboardAcceptedSocketAddress local_;
    DashboardAcceptedSocketAddress remote_;
};

// Owns a validated pair of Winsock extension functions. Raw extension-function
// pointers are confined to the implementation object and cannot be observed or
// invoked by callers.
class DashboardWinsockExtensions final {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardWinsockExtensions>>
    discover(SOCKET discoverySocket) noexcept;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardWinsockExtensions>>
    discover(
        SOCKET discoverySocket,
        std::shared_ptr<IDashboardWinsockExtensionApi> api) noexcept;

    ~DashboardWinsockExtensions() noexcept;

    DashboardWinsockExtensions(const DashboardWinsockExtensions&) = delete;
    DashboardWinsockExtensions& operator=(
        const DashboardWinsockExtensions&) = delete;
    DashboardWinsockExtensions(DashboardWinsockExtensions&&) = delete;
    DashboardWinsockExtensions& operator=(
        DashboardWinsockExtensions&&) = delete;

    // AcceptEx must complete when a connection arrives, not wait for request
    // payload. Therefore receiveDataLength is required to be exactly zero.
    // Both successful dispositions are issued operations and remain owned by
    // the IOCP completion path; synchronous success does not permit early
    // OVERLAPPED or accept-slot reuse.
    [[nodiscard]] Domain::Result<DashboardAcceptIssueDisposition> issueAccept(
        SOCKET listenerSocket,
        SOCKET acceptedSocket,
        int addressFamily,
        void* outputBuffer,
        DWORD outputBufferLength,
        DWORD receiveDataLength,
        DWORD localAddressLength,
        DWORD remoteAddressLength,
        OVERLAPPED& operation) noexcept;

    // The three framing lengths must be exactly those supplied to issueAccept.
    // All storage and arithmetic are validated before the native extractor is
    // called, and the returned address ranges are validated before copying.
    [[nodiscard]] Domain::Result<DashboardAcceptedAddresses> extractAddresses(
        int addressFamily,
        const void* outputBuffer,
        DWORD outputBufferLength,
        DWORD receiveDataLength,
        DWORD localAddressLength,
        DWORD remoteAddressLength) const noexcept;

    static constexpr DWORD RequiredReceiveDataLength = 0U;
    static constexpr DWORD RequiredAddressPadding = 16U;
    static constexpr DWORD Ipv4AddressRegionLength =
        static_cast<DWORD>(sizeof(sockaddr_in)) + RequiredAddressPadding;
    static constexpr DWORD Ipv6AddressRegionLength =
        static_cast<DWORD>(sizeof(sockaddr_in6)) + RequiredAddressPadding;

private:
    class Impl;

    explicit DashboardWinsockExtensions(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
