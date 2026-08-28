#pragma once

#include "DashboardAcceptSlot.h"
#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Result.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Narrow native seam for one connected overlapped socket. The accepted socket
// remains owned by DashboardAcceptedConnection; this API never closes it.
class IDashboardConnectionSocketApi {
public:
    virtual ~IDashboardConnectionSocketApi() noexcept = default;

    [[nodiscard]] virtual int receive(
        SOCKET socket,
        WSABUF* buffers,
        DWORD bufferCount,
        DWORD& transferredBytes,
        DWORD& flags,
        OVERLAPPED* operation) noexcept = 0;

    [[nodiscard]] virtual int send(
        SOCKET socket,
        WSABUF* buffers,
        DWORD bufferCount,
        DWORD& transferredBytes,
        DWORD flags,
        OVERLAPPED* operation) noexcept = 0;

    [[nodiscard]] virtual BOOL cancel(
        SOCKET socket,
        OVERLAPPED* operation) noexcept = 0;

    [[nodiscard]] virtual int shutdownSocket(
        SOCKET socket,
        int how) noexcept = 0;

    [[nodiscard]] virtual int lastSocketError() noexcept = 0;
    [[nodiscard]] virtual DWORD lastSystemError() noexcept = 0;
};

class DashboardConnectionSocketSystemApi final
    : public IDashboardConnectionSocketApi {
public:
    [[nodiscard]] int receive(
        SOCKET socket,
        WSABUF* buffers,
        DWORD bufferCount,
        DWORD& transferredBytes,
        DWORD& flags,
        OVERLAPPED* operation) noexcept override;

    [[nodiscard]] int send(
        SOCKET socket,
        WSABUF* buffers,
        DWORD bufferCount,
        DWORD& transferredBytes,
        DWORD flags,
        OVERLAPPED* operation) noexcept override;

    [[nodiscard]] BOOL cancel(
        SOCKET socket,
        OVERLAPPED* operation) noexcept override;

    [[nodiscard]] int shutdownSocket(
        SOCKET socket,
        int how) noexcept override;

    [[nodiscard]] int lastSocketError() noexcept override;
    [[nodiscard]] DWORD lastSystemError() noexcept override;
};

enum class DashboardConnectionSocketState : std::uint8_t {
    Idle,
    ReceiveIssued,
    SendIssued,
    ReceiveCancellationRequested,
    SendCancellationRequested,
};

enum class DashboardConnectionSocketOperationKind : std::uint8_t {
    Receive,
    Send,
};

enum class DashboardConnectionSocketIssueDisposition : std::uint8_t {
    // Association with IOCP means even immediate native success still owns an
    // outstanding completion and all operation storage remains live.
    CompletedSynchronously,
    Pending,
};

enum class DashboardConnectionSocketCancellationDisposition : std::uint8_t {
    Requested,
    AlreadyRequested,
    CompletionMayHaveWon,
};

enum class DashboardConnectionSocketShutdownDisposition : std::uint8_t {
    Requested,
    AlreadyRequested,
    AlreadyClosed,
};

class DashboardConnectionSocketReapResult final {
public:
    DashboardConnectionSocketReapResult(
        DashboardConnectionSocketOperationKind operationKind,
        DWORD transferredBytes) noexcept;

    [[nodiscard]] DashboardConnectionSocketOperationKind operationKind()
        const noexcept
    {
        return operationKind_;
    }

    [[nodiscard]] DWORD transferredBytes() const noexcept
    {
        return transferredBytes_;
    }

private:
    DashboardConnectionSocketOperationKind operationKind_{};
    DWORD transferredBytes_{};
};

class DashboardConnectionSocketSnapshot final {
public:
    DashboardConnectionSocketSnapshot(
        DashboardConnectionSocketState state,
        bool shutdownRequested,
        std::size_t receivedByteCount,
        std::size_t activeBufferLength) noexcept;

    [[nodiscard]] DashboardConnectionSocketState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] bool shutdownRequested() const noexcept
    {
        return shutdownRequested_;
    }

    [[nodiscard]] std::size_t receivedByteCount() const noexcept
    {
        return receivedByteCount_;
    }

    [[nodiscard]] std::size_t activeBufferLength() const noexcept
    {
        return activeBufferLength_;
    }

private:
    DashboardConnectionSocketState state_{};
    bool shutdownRequested_{};
    std::size_t receivedByteCount_{};
    std::size_t activeBufferLength_{};
};

// Interface-first per-connection transport boundary. The lifecycle owner can
// be tested without a native socket while the production implementation keeps
// the accepted socket and its one stable OVERLAPPED under RAII ownership.
class IDashboardConnectionIo {
public:
    virtual ~IDashboardConnectionIo() noexcept = default;

    [[nodiscard]] virtual DashboardIoCompletionKey completionKey()
        const noexcept = 0;
    [[nodiscard]] virtual OVERLAPPED* borrowedOperation() noexcept = 0;
    [[nodiscard]] virtual DashboardConnectionSocketState state()
        const noexcept = 0;
    [[nodiscard]] virtual DashboardConnectionSocketSnapshot snapshot()
        const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<
        DashboardConnectionSocketIssueDisposition>
    issueReceive() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<
        DashboardConnectionSocketIssueDisposition>
    issueSend(std::span<const std::byte> bytes) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<
        DashboardConnectionSocketCancellationDisposition>
    requestCancellation() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<
        DashboardConnectionSocketShutdownDisposition>
    shutdownBoth() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<DashboardConnectionSocketReapResult>
    reap(
        DashboardIoCompletionKey completionKey,
        DWORD transferredBytes,
        OVERLAPPED* completedOperation,
        DWORD nativeError) noexcept = 0;

    [[nodiscard]] virtual std::span<const std::byte> receivedBytes()
        const noexcept = 0;
};

// Heap-stable owner for exactly one accepted socket and one native OVERLAPPED
// operation. Receive storage is owned. Send storage is borrowed and its exact
// pointer/length must remain valid until the matching completion is reaped.
// At most one receive or send can be outstanding. The IOCP kernel and routing
// registry identified by completionKey must outlive every issued operation.
class DashboardConnectionSocket final : public IDashboardConnectionIo {
public:
    static constexpr std::size_t ReceiveBufferLength = 16U * 1024U;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardConnectionSocket>>
    create(
        DashboardAcceptedConnection acceptedConnection,
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey) noexcept;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardConnectionSocket>>
    create(
        DashboardAcceptedConnection acceptedConnection,
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey,
        std::shared_ptr<IDashboardConnectionSocketApi> api) noexcept;

    DashboardConnectionSocket(const DashboardConnectionSocket&) = delete;
    DashboardConnectionSocket& operator=(
        const DashboardConnectionSocket&) = delete;
    DashboardConnectionSocket(DashboardConnectionSocket&&) = delete;
    DashboardConnectionSocket& operator=(DashboardConnectionSocket&&) =
        delete;

    // Destroying kernel-referenced OVERLAPPED or borrowed send metadata would
    // be a use-after-free. Every issue must be cancelled if needed and reaped.
    ~DashboardConnectionSocket() noexcept override;

    [[nodiscard]] SOCKET borrowedNativeSocket() const noexcept
    {
        return acceptedConnection_.borrowedNativeSocket();
    }

    [[nodiscard]] const DashboardAcceptedAddresses& addresses() const noexcept
    {
        return acceptedConnection_.addresses();
    }

    [[nodiscard]] DashboardIoCompletionKey completionKey()
        const noexcept override
    {
        return completionKey_;
    }

    // Stable for this owner's entire heap lifetime.
    [[nodiscard]] OVERLAPPED* borrowedOperation() noexcept override
    {
        return &operation_;
    }

    [[nodiscard]] DashboardConnectionSocketState state()
        const noexcept override;
    [[nodiscard]] DashboardConnectionSocketSnapshot snapshot()
        const noexcept override;

    [[nodiscard]] Domain::Result<DashboardConnectionSocketIssueDisposition>
    issueReceive() noexcept override;

    // bytes must be nonempty and remain stable through exact completion reap.
    // A partial successful send returns its transferred count; the caller owns
    // issuing the remaining suffix as a later operation.
    [[nodiscard]] Domain::Result<DashboardConnectionSocketIssueDisposition>
    issueSend(std::span<const std::byte> bytes) noexcept override;

    [[nodiscard]] Domain::Result<
        DashboardConnectionSocketCancellationDisposition>
    requestCancellation() noexcept override;

    [[nodiscard]] Domain::Result<DashboardConnectionSocketShutdownDisposition>
    shutdownBoth() noexcept override;

    // A foreign operation is rejected without mutation. Once the exact
    // operation pointer has been dequeued, every key/error/byte shape consumes
    // and resets it before returning, including malformed packets.
    [[nodiscard]] Domain::Result<DashboardConnectionSocketReapResult> reap(
        DashboardIoCompletionKey completionKey,
        DWORD transferredBytes,
        OVERLAPPED* completedOperation,
        DWORD nativeError) noexcept override;

    // Available only after a successful nonempty receive reap and only until
    // the next receive/send issue. The returned view borrows this owner.
    [[nodiscard]] std::span<const std::byte> receivedBytes()
        const noexcept override;

private:
    DashboardConnectionSocket(
        DashboardAcceptedConnection acceptedConnection,
        DashboardIoCompletionKey completionKey,
        std::shared_ptr<IDashboardConnectionSocketApi> api) noexcept;

    void resetIssuedOperationLocked() noexcept;

    DashboardAcceptedConnection acceptedConnection_;
    const DashboardIoCompletionKey completionKey_;
    std::shared_ptr<IDashboardConnectionSocketApi> api_;
    std::array<std::byte, ReceiveBufferLength> receiveBuffer_{};
    OVERLAPPED operation_{};
    WSABUF activeBuffer_{};
    DWORD receiveFlags_{};
    DWORD immediateTransferredBytes_{};
    const std::byte* borrowedSendBytes_{};
    std::size_t borrowedSendLength_{};
    std::size_t receivedByteCount_{};
    std::atomic<DashboardConnectionSocketState> state_{
        DashboardConnectionSocketState::Idle};
    std::atomic_bool shutdownRequested_{};
    mutable std::mutex mutex_;
};

static_assert(
    DashboardConnectionSocket::ReceiveBufferLength <=
    static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));

} // namespace ForgeConductor::Infrastructure::Windows::Detail
