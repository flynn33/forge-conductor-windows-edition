#pragma once

#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// One immutable process-fixed completion-key owner. Listener generations and
// the overload responder set implement this boundary; dynamic connection and
// deadline keys remain owned by the fallback connection registry.
class IDashboardFixedIocpCompletionTarget {
public:
    virtual ~IDashboardFixedIocpCompletionTarget() noexcept = default;

    [[nodiscard]] virtual DashboardIoCompletionKey completionKey()
        const noexcept = 0;

    virtual void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept = 0;

    // A process IOCP failure is terminal for every fixed owner. The callback
    // must latch failure and request cancellation without joining workers.
    virtual void fatal(DWORD nativeError) noexcept = 0;

    // Graceful process shutdown closes admission and requests cancellation;
    // already dequeued packets may continue routing until exact drain.
    virtual void beginShutdown() noexcept = 0;
};

class DashboardIocpCompletionRouterSnapshot final {
public:
    [[nodiscard]] std::size_t fixedTargetCount() const noexcept
    {
        return fixedTargetCount_;
    }

    [[nodiscard]] std::size_t maximumFixedTargetCount() const noexcept
    {
        return maximumFixedTargetCount_;
    }

    [[nodiscard]] std::uint64_t fixedDispatchCount() const noexcept
    {
        return fixedDispatchCount_;
    }

    [[nodiscard]] std::uint64_t fallbackDispatchCount() const noexcept
    {
        return fallbackDispatchCount_;
    }

    [[nodiscard]] std::uint64_t fatalNotificationCount() const noexcept
    {
        return fatalNotificationCount_;
    }

    [[nodiscard]] bool registrationOpen() const noexcept
    {
        return registrationOpen_;
    }

    [[nodiscard]] bool shutdownRequested() const noexcept
    {
        return shutdownRequested_;
    }

    [[nodiscard]] std::optional<DWORD> fatalNativeError() const noexcept
    {
        return fatalNativeError_;
    }

private:
    friend class DashboardIocpCompletionRouter;

    DashboardIocpCompletionRouterSnapshot(
        std::size_t fixedTargetCount,
        std::size_t maximumFixedTargetCount,
        std::uint64_t fixedDispatchCount,
        std::uint64_t fallbackDispatchCount,
        std::uint64_t fatalNotificationCount,
        bool registrationOpen,
        bool shutdownRequested,
        std::optional<DWORD> fatalNativeError) noexcept;

    std::size_t fixedTargetCount_{};
    std::size_t maximumFixedTargetCount_{};
    std::uint64_t fixedDispatchCount_{};
    std::uint64_t fallbackDispatchCount_{};
    std::uint64_t fatalNotificationCount_{};
    bool registrationOpen_{};
    bool shutdownRequested_{};
    std::optional<DWORD> fatalNativeError_;
};

// Process-owned nonblocking key router. Exactly two listener generations and
// one shared overload responder set can be registered concurrently. Every
// other key is delegated to the dynamic connection/deadline registry. Shared
// owners are pinned under the router mutex and invoked only after unlock.
class DashboardIocpCompletionRouter final
    : public IDashboardIocpCompletionSink {
public:
    static constexpr std::size_t MaximumFixedTargetCount = 3U;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardIocpCompletionRouter>>
    create(
        DashboardIoCompletionKey fallbackOwnedCompletionKey,
        std::shared_ptr<IDashboardIocpCompletionSink> fallback) noexcept;

    DashboardIocpCompletionRouter(
        const DashboardIocpCompletionRouter&) = delete;
    DashboardIocpCompletionRouter& operator=(
        const DashboardIocpCompletionRouter&) = delete;
    DashboardIocpCompletionRouter(
        DashboardIocpCompletionRouter&&) = delete;
    DashboardIocpCompletionRouter& operator=(
        DashboardIocpCompletionRouter&&) = delete;
    ~DashboardIocpCompletionRouter() noexcept override = default;

    [[nodiscard]] DashboardIoCompletionKey fallbackOwnedCompletionKey()
        const noexcept
    {
        return fallbackOwnedCompletionKey_;
    }

    [[nodiscard]] Domain::Result<void> registerFixedTarget(
        std::shared_ptr<IDashboardFixedIocpCompletionTarget> target)
        noexcept;

    // Removes only the exact key-and-owner identity. A target is unregistered
    // after all operations for that key have drained; a concurrently dequeued
    // callback retains its own shared pin through consume.
    [[nodiscard]] bool unregisterFixedTarget(
        const std::shared_ptr<IDashboardFixedIocpCompletionTarget>& target)
        noexcept;

    void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept override;

    void fatal(DWORD nativeError) noexcept override;

    // Closes future fixed-target registration and requests nonblocking
    // shutdown on every current fixed owner. The composition root separately
    // shuts down the fallback connection registry before joining the kernel.
    void beginShutdown() noexcept;

    [[nodiscard]] DashboardIocpCompletionRouterSnapshot snapshot()
        const noexcept;

private:
    struct Entry final {
        DashboardIoCompletionKey key{0U};
        std::shared_ptr<IDashboardFixedIocpCompletionTarget> target;
    };

    explicit DashboardIocpCompletionRouter(
        DashboardIoCompletionKey fallbackOwnedCompletionKey,
        std::shared_ptr<IDashboardIocpCompletionSink> fallback) noexcept;

    [[nodiscard]] Entry* findKeyLocked(
        DashboardIoCompletionKey key) noexcept;
    [[nodiscard]] Entry* findVacantLocked() noexcept;

    const DashboardIoCompletionKey fallbackOwnedCompletionKey_{0U};
    const std::shared_ptr<IDashboardIocpCompletionSink> fallback_;
    std::array<Entry, MaximumFixedTargetCount> entries_{};
    std::size_t fixedTargetCount_{};
    std::uint64_t fixedDispatchCount_{};
    std::uint64_t fallbackDispatchCount_{};
    std::uint64_t fatalNotificationCount_{};
    bool registrationOpen_{true};
    bool shutdownRequested_{};
    std::optional<DWORD> fatalNativeError_;
    mutable std::mutex mutex_;
};

static_assert(
    DashboardIocpCompletionRouter::MaximumFixedTargetCount == 3U);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
