#pragma once

#include "DashboardIoCompletionPort.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Process-owned completion dispatch boundary. Implementations must perform
// only nonblocking socket-state transitions and must not retain packet fields
// beyond the call unless the corresponding operation storage has an explicit
// longer-lived owner.
class IDashboardIocpCompletionSink {
public:
    virtual ~IDashboardIocpCompletionSink() noexcept = default;

    virtual void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept = 0;

    // Delivers the first fatal dequeue failure exactly once. The callback must
    // be nonblocking; it may request kernel shutdown, but the external
    // composition owner remains responsible for joining the workers.
    virtual void fatal(DWORD nativeError) noexcept = 0;
};

class DashboardIocpWorkerSnapshot final {
public:
    [[nodiscard]] std::size_t startedWorkerCount() const noexcept
    {
        return startedWorkerCount_;
    }

    [[nodiscard]] std::size_t exitedWorkerCount() const noexcept
    {
        return exitedWorkerCount_;
    }

    [[nodiscard]] bool isShuttingDown() const noexcept { return shuttingDown_; }

    [[nodiscard]] bool controlPostFailed() const noexcept
    {
        return controlPostFailed_;
    }

    [[nodiscard]] std::optional<DWORD> fatalNativeError() const noexcept
    {
        return fatalNativeError_;
    }

private:
    friend class DashboardIocpWorkerKernel;

    DashboardIocpWorkerSnapshot(
        std::size_t startedWorkerCount,
        std::size_t exitedWorkerCount,
        bool shuttingDown,
        bool controlPostFailed,
        std::optional<DWORD> fatalNativeError) noexcept;

    std::size_t startedWorkerCount_{};
    std::size_t exitedWorkerCount_{};
    bool shuttingDown_{};
    bool controlPostFailed_{};
    std::optional<DWORD> fatalNativeError_;
};

// Owns the one process-shared dashboard IOCP and exactly four persistent I/O
// workers. The Windows queue remains only a completion mechanism: callers may
// post solely for work already admitted into an independently bounded owner.
// Its composition-root owner must outlive every completion callback and must
// destroy the kernel only from a non-worker thread so all workers can be
// joined without detachment.
class DashboardIocpWorkerKernel final {
public:
    static constexpr std::size_t WorkerCount = 4U;
    static constexpr DWORD WorkerWaitMilliseconds = 250U;
    static constexpr auto WorkerStartupTimeout = std::chrono::seconds{5};
    static constexpr auto ShutdownDrainTimeout = std::chrono::seconds{5};
    static constexpr std::uintptr_t ShutdownKeyValue =
        (std::numeric_limits<std::uintptr_t>::max)();

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardIocpWorkerKernel>>
    create(
        std::unique_ptr<DashboardIoCompletionPort> port,
        std::shared_ptr<IDashboardIocpCompletionSink> sink) noexcept;

    ~DashboardIocpWorkerKernel() noexcept;

    DashboardIocpWorkerKernel(const DashboardIocpWorkerKernel&) = delete;
    DashboardIocpWorkerKernel& operator=(
        const DashboardIocpWorkerKernel&) = delete;
    DashboardIocpWorkerKernel(DashboardIocpWorkerKernel&&) = delete;
    DashboardIocpWorkerKernel& operator=(
        DashboardIocpWorkerKernel&&) = delete;

    [[nodiscard]] Domain::Result<void> associateHandle(
        HANDLE handle,
        DashboardIoCompletionKey completionKey) noexcept;

    [[nodiscard]] Domain::Result<void> associateSocket(
        SOCKET socket,
        DashboardIoCompletionKey completionKey) noexcept;

    [[nodiscard]] Domain::Result<void> postAdmitted(
        DWORD transferredBytes,
        DashboardIoCompletionKey completionKey,
        OVERLAPPED* operation) noexcept;

    [[nodiscard]] DashboardIocpWorkerSnapshot snapshot() const noexcept;

    // The composition root first stops listener/connection admission, cancels
    // native operations, and reaps their completions. Only then may it call
    // shutdown to post four control packets and join all four workers.
    void beginShutdown() noexcept;
    void shutdown() noexcept;

private:
    class Impl;

    explicit DashboardIocpWorkerKernel(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

static_assert(
    DashboardIocpWorkerKernel::WorkerCount ==
    DashboardIoCompletionPort::RequiredConcurrencyThreadCount);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
