#pragma once

#include "DashboardListeningSocket.h"
#include "DashboardWinsockExtensions.h"

#include "ForgeConductor/Domain/Result.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Narrow native seam for the two post-issue operations owned by an accept
// slot. Socket-option and kernel error domains remain separate so an error is
// always read from the API that produced it.
class IDashboardAcceptSlotApi {
public:
    virtual ~IDashboardAcceptSlotApi() = default;

    [[nodiscard]] virtual int updateAcceptContext(
        SOCKET acceptedSocket,
        SOCKET listenerSocket) noexcept = 0;
    [[nodiscard]] virtual BOOL cancelAccept(
        SOCKET listenerSocket,
        OVERLAPPED* operation) noexcept = 0;
    [[nodiscard]] virtual int lastSocketError() noexcept = 0;
    [[nodiscard]] virtual DWORD lastSystemError() noexcept = 0;
};

class DashboardAcceptSlotSystemApi final : public IDashboardAcceptSlotApi {
public:
    [[nodiscard]] int updateAcceptContext(
        SOCKET acceptedSocket,
        SOCKET listenerSocket) noexcept override;
    [[nodiscard]] BOOL cancelAccept(
        SOCKET listenerSocket,
        OVERLAPPED* operation) noexcept override;
    [[nodiscard]] int lastSocketError() noexcept override;
    [[nodiscard]] DWORD lastSystemError() noexcept override;
};

enum class DashboardAcceptSlotState : std::uint8_t {
    Idle,
    Issued,
    CancellationRequested,
};

enum class DashboardAcceptCancellationDisposition : std::uint8_t {
    Requested,
    AlreadyRequested,
    CompletionMayHaveWon,
};

// Move-only handoff produced only after the matching AcceptEx completion has
// been reaped, SO_UPDATE_ACCEPT_CONTEXT has succeeded, and both native address
// values have been copied out of reusable slot storage.
class DashboardAcceptedConnection final {
public:
    DashboardAcceptedConnection(const DashboardAcceptedConnection&) = delete;
    DashboardAcceptedConnection& operator=(
        const DashboardAcceptedConnection&) = delete;
    DashboardAcceptedConnection(DashboardAcceptedConnection&&) noexcept =
        default;
    DashboardAcceptedConnection& operator=(
        DashboardAcceptedConnection&&) noexcept = default;
    ~DashboardAcceptedConnection() noexcept = default;

    [[nodiscard]] SOCKET borrowedNativeSocket() const noexcept
    {
        return socket_.get();
    }

    [[nodiscard]] const DashboardAcceptedAddresses& addresses() const noexcept
    {
        return addresses_;
    }

private:
    friend class DashboardAcceptSlot;

    DashboardAcceptedConnection(
        UniqueDashboardSocket socket,
        DashboardAcceptedAddresses addresses) noexcept;

    UniqueDashboardSocket socket_;
    DashboardAcceptedAddresses addresses_;
};

// One heap-stable reusable AcceptEx operation. The future four-slot set owns
// slot lifetime through every issued operation. Cancellation changes only the
// typed state and asks the kernel to cancel; it never releases OVERLAPPED,
// address storage, or accepted-socket ownership before exact-pointer reaping.
class DashboardAcceptSlot final {
public:
    static constexpr DWORD Ipv4AddressBufferLength =
        DashboardWinsockExtensions::Ipv4AddressRegionLength * 2U;
    static constexpr DWORD Ipv6AddressBufferLength =
        DashboardWinsockExtensions::Ipv6AddressRegionLength * 2U;
    static constexpr DWORD MaximumAddressBufferLength =
        Ipv6AddressBufferLength;

    [[nodiscard]] static Domain::Result<std::unique_ptr<DashboardAcceptSlot>>
    create() noexcept;

    [[nodiscard]] static Domain::Result<std::unique_ptr<DashboardAcceptSlot>>
    create(std::shared_ptr<IDashboardAcceptSlotApi> api) noexcept;

    DashboardAcceptSlot(const DashboardAcceptSlot&) = delete;
    DashboardAcceptSlot& operator=(const DashboardAcceptSlot&) = delete;
    DashboardAcceptSlot(DashboardAcceptSlot&&) = delete;
    DashboardAcceptSlot& operator=(DashboardAcceptSlot&&) = delete;
    // Destroying kernel-referenced OVERLAPPED storage would be a use-after-free.
    // The future slot set must cancel and reap before releasing its slots; a
    // violated ownership contract fails fast instead of freeing live storage.
    ~DashboardAcceptSlot() noexcept;

    [[nodiscard]] DashboardAcceptSlotState state() const noexcept;

    // Stable for this slot's entire heap lifetime. It is borrowed by IOCP and
    // must be used verbatim when dispatching the matching completion.
    [[nodiscard]] OVERLAPPED* borrowedOperation() noexcept
    {
        return &operation_;
    }

    [[nodiscard]] DWORD activeAddressBufferLength() const noexcept;

    [[nodiscard]] Domain::Result<DashboardAcceptIssueDisposition> issue(
        DashboardWinsockRuntime& runtime,
        DashboardWinsockExtensions& extensions,
        const DashboardListeningSocket& listener) noexcept;

    [[nodiscard]] Domain::Result<DashboardAcceptCancellationDisposition>
    requestCancellation() noexcept;

    // Both completion methods require the exact borrowed OVERLAPPED pointer.
    // A successful completion may legitimately follow a cancellation request.
    [[nodiscard]] Domain::Result<DashboardAcceptedConnection> reapSuccessful(
        DashboardWinsockExtensions& extensions,
        OVERLAPPED* completedOperation) noexcept;

    [[nodiscard]] Domain::Result<void> reapFailed(
        OVERLAPPED* completedOperation,
        DWORD nativeError) noexcept;

private:
    explicit DashboardAcceptSlot(
        std::shared_ptr<IDashboardAcceptSlotApi> api) noexcept;

    [[nodiscard]] Domain::Result<void> validateCompletion(
        const OVERLAPPED* completedOperation) const noexcept;
    void resetAfterReap() noexcept;

    std::shared_ptr<IDashboardAcceptSlotApi> api_;
    alignas(sockaddr_storage)
        std::array<std::byte, MaximumAddressBufferLength> addressBuffer_{};
    OVERLAPPED operation_{};
    std::optional<UniqueDashboardSocket> acceptedSocket_;
    SOCKET listenerSocket_{INVALID_SOCKET};
    int addressFamily_{AF_UNSPEC};
    DWORD addressRegionLength_{};
    DWORD addressBufferLength_{};
    std::optional<DashboardLoopbackEndpoint> listenerEndpoint_;
    std::atomic<DashboardAcceptSlotState> state_{
        DashboardAcceptSlotState::Idle};
    mutable std::mutex mutex_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
