#pragma once

#include "ForgeConductor/Domain/Result.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Strong completion-key value used for both native handle association and
// synthetic packets. The runtime registry, not this primitive, owns the
// object or generation identified by the value.
class DashboardIoCompletionKey final {
public:
    explicit constexpr DashboardIoCompletionKey(
        const std::uintptr_t value) noexcept
        : value_{value}
    {
    }

    [[nodiscard]] constexpr std::uintptr_t value() const noexcept
    {
        return value_;
    }

    bool operator==(const DashboardIoCompletionKey&) const = default;

private:
    std::uintptr_t value_{};
};

struct DashboardIoCompletionPacket final {
    DWORD transferredBytes{};
    DashboardIoCompletionKey completionKey{0U};
    OVERLAPPED* operation{};

    bool operator==(const DashboardIoCompletionPacket&) const = default;
};

// Fixed, allocation-free result used only while posting reserved shutdown
// controls. Public admitted packets retain the richer typed-error result below.
class DashboardIoCompletionControlPostResult final {
public:
    [[nodiscard]] bool succeeded() const noexcept { return succeeded_; }
    [[nodiscard]] DWORD nativeError() const noexcept { return nativeError_; }

private:
    friend class DashboardIoCompletionPort;

    DashboardIoCompletionControlPostResult(
        bool succeeded,
        DWORD nativeError) noexcept;

    bool succeeded_{};
    DWORD nativeError_{ERROR_GEN_FAILURE};
};

enum class DashboardIoCompletionDequeueDisposition : std::uint8_t {
    Succeeded,
    IoFailed,
    TimedOut,
    FatalError,
};

// GetQueuedCompletionStatus returns FALSE for three materially different
// states. A non-null OVERLAPPED is a dequeued failed I/O packet. A null
// OVERLAPPED plus WAIT_TIMEOUT is an ordinary bounded wait expiry. Every other
// null-OVERLAPPED failure is fatal to the worker. Only packet dispositions
// expose packet fields.
class DashboardIoCompletionDequeueResult final {
public:
    [[nodiscard]] DashboardIoCompletionDequeueDisposition disposition()
        const noexcept
    {
        return disposition_;
    }

    [[nodiscard]] bool hasPacket() const noexcept
    {
        return packet_.has_value();
    }

    [[nodiscard]] const DashboardIoCompletionPacket* packet() const noexcept
    {
        return packet_.has_value() ? std::addressof(*packet_) : nullptr;
    }

    [[nodiscard]] DWORD nativeError() const noexcept { return nativeError_; }

private:
    friend class DashboardIoCompletionPort;

    DashboardIoCompletionDequeueResult(
        DashboardIoCompletionDequeueDisposition disposition,
        std::optional<DashboardIoCompletionPacket> packet,
        DWORD nativeError) noexcept;

    DashboardIoCompletionDequeueDisposition disposition_{
        DashboardIoCompletionDequeueDisposition::FatalError};
    std::optional<DashboardIoCompletionPacket> packet_;
    DWORD nativeError_{ERROR_GEN_FAILURE};
};

// Narrow injectable seam for the five kernel calls owned by the shared
// dashboard completion-port primitive.
class IDashboardIoCompletionPortApi {
public:
    virtual ~IDashboardIoCompletionPortApi() = default;

    [[nodiscard]] virtual HANDLE createIoCompletionPort(
        HANDLE fileHandle,
        HANDLE existingCompletionPort,
        ULONG_PTR completionKey,
        DWORD concurrentThreadCount) noexcept = 0;
    [[nodiscard]] virtual BOOL postQueuedCompletionStatus(
        HANDLE completionPort,
        DWORD transferredBytes,
        ULONG_PTR completionKey,
        OVERLAPPED* operation) noexcept = 0;
    [[nodiscard]] virtual BOOL getQueuedCompletionStatus(
        HANDLE completionPort,
        DWORD& transferredBytes,
        ULONG_PTR& completionKey,
        OVERLAPPED*& operation,
        DWORD timeoutMilliseconds) noexcept = 0;
    [[nodiscard]] virtual BOOL closeHandle(HANDLE handle) noexcept = 0;
    [[nodiscard]] virtual DWORD lastError() noexcept = 0;
};

class DashboardIoCompletionPortSystemApi final
    : public IDashboardIoCompletionPortApi {
public:
    [[nodiscard]] HANDLE createIoCompletionPort(
        HANDLE fileHandle,
        HANDLE existingCompletionPort,
        ULONG_PTR completionKey,
        DWORD concurrentThreadCount) noexcept override;
    [[nodiscard]] BOOL postQueuedCompletionStatus(
        HANDLE completionPort,
        DWORD transferredBytes,
        ULONG_PTR completionKey,
        OVERLAPPED* operation) noexcept override;
    [[nodiscard]] BOOL getQueuedCompletionStatus(
        HANDLE completionPort,
        DWORD& transferredBytes,
        ULONG_PTR& completionKey,
        OVERLAPPED*& operation,
        DWORD timeoutMilliseconds) noexcept override;
    [[nodiscard]] BOOL closeHandle(HANDLE handle) noexcept override;
    [[nodiscard]] DWORD lastError() noexcept override;
};

// Owns exactly one process-shared dashboard IOCP. It does not own workers,
// registered operations, association targets, or any packet queue outside the
// Windows kernel. The port handle is intentionally never exposed or released.
class DashboardIoCompletionPort final {
public:
    static constexpr DWORD RequiredConcurrencyThreadCount = 4U;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardIoCompletionPort>>
    create() noexcept;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardIoCompletionPort>>
    create(std::shared_ptr<IDashboardIoCompletionPortApi> api) noexcept;

    ~DashboardIoCompletionPort() noexcept;

    DashboardIoCompletionPort(const DashboardIoCompletionPort&) = delete;
    DashboardIoCompletionPort& operator=(
        const DashboardIoCompletionPort&) = delete;
    DashboardIoCompletionPort(DashboardIoCompletionPort&&) = delete;
    DashboardIoCompletionPort& operator=(DashboardIoCompletionPort&&) = delete;

    // Associates borrowed objects. Their lifetime remains with their native
    // socket or handle owner and must exceed every outstanding operation.
    [[nodiscard]] Domain::Result<void> associateHandle(
        HANDLE handle,
        DashboardIoCompletionKey completionKey) const noexcept;

    [[nodiscard]] Domain::Result<void> associateSocket(
        SOCKET socket,
        DashboardIoCompletionKey completionKey) const noexcept;

    [[nodiscard]] Domain::Result<void> post(
        DWORD transferredBytes,
        DashboardIoCompletionKey completionKey,
        OVERLAPPED* operation) const noexcept;

    // Posts the reserved zero-byte/null-operation shutdown shape without
    // constructing a string-owning Domain::Error on native failure.
    [[nodiscard]] DashboardIoCompletionControlPostResult postControl(
        DashboardIoCompletionKey completionKey) const noexcept;

    // The caller supplies a finite wait selected by the bounded worker loop.
    [[nodiscard]] DashboardIoCompletionDequeueResult dequeue(
        DWORD timeoutMilliseconds) const noexcept;

private:
    DashboardIoCompletionPort(
        std::shared_ptr<IDashboardIoCompletionPortApi> api,
        HANDLE port) noexcept;

    std::shared_ptr<IDashboardIoCompletionPortApi> api_;
    HANDLE port_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
