#pragma once

#include "DashboardConnectionEventBridge.h"
#include "DashboardConnectionState.h"
#include "DashboardDeadlineIocpBridge.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardConnectionRegistryFailureKind : std::uint8_t {
    InvalidRequest,
    Conflict,
    LimitExceeded,
    TransportClosed,
    IntegrityFailure,
    InternalFailure,
    Other,
};

// Fixed-size failure retained by the noexcept registry snapshot. The optional
// string-owning diagnostic is available only through fullFailure().
struct DashboardConnectionRegistryFailure final {
    DashboardConnectionRegistryFailureKind kind{
        DashboardConnectionRegistryFailureKind::Other};
    bool retryable{};
    std::optional<DWORD> nativeError;

    bool operator==(const DashboardConnectionRegistryFailure&) const = default;
};

class DashboardConnectionRegistrySnapshot final {
public:
    [[nodiscard]] std::size_t registeredConnectionCount() const noexcept
    {
        return registeredConnectionCount_;
    }

    [[nodiscard]] std::size_t maximumConnectionCount() const noexcept
    {
        return maximumConnectionCount_;
    }

    [[nodiscard]] DashboardIoCompletionKey deadlineCompletionKey()
        const noexcept
    {
        return deadlineCompletionKey_;
    }

    [[nodiscard]] bool deadlineBridgeBound() const noexcept
    {
        return deadlineBridgeBound_;
    }

    [[nodiscard]] std::uint64_t connectionDispatchCount() const noexcept
    {
        return connectionDispatchCount_;
    }

    [[nodiscard]] std::uint64_t deadlineDispatchCount() const noexcept
    {
        return deadlineDispatchCount_;
    }

    [[nodiscard]] std::uint64_t retiredDeadlineDrainCount() const noexcept
    {
        return retiredDeadlineDrainCount_;
    }

    [[nodiscard]] std::uint64_t removedConnectionCount() const noexcept
    {
        return removedConnectionCount_;
    }

    [[nodiscard]] std::uint64_t fatalNotificationCount() const noexcept
    {
        return fatalNotificationCount_;
    }

    [[nodiscard]] bool deadlineRoutingInProgress() const noexcept
    {
        return deadlineRoutingInProgress_;
    }

    [[nodiscard]] bool isShuttingDown() const noexcept { return shutdown_; }
    [[nodiscard]] bool isFatal() const noexcept { return fatal_; }

    [[nodiscard]] const DashboardConnectionRegistryFailure* failure()
        const noexcept
    {
        return failure_.has_value() ? std::addressof(*failure_) : nullptr;
    }

private:
    friend class DashboardConnectionRegistry;

    DashboardConnectionRegistrySnapshot(
        std::size_t registeredConnectionCount,
        std::size_t maximumConnectionCount,
        DashboardIoCompletionKey deadlineCompletionKey,
        bool deadlineBridgeBound,
        std::uint64_t connectionDispatchCount,
        std::uint64_t deadlineDispatchCount,
        std::uint64_t retiredDeadlineDrainCount,
        std::uint64_t removedConnectionCount,
        std::uint64_t fatalNotificationCount,
        bool deadlineRoutingInProgress,
        bool shutdown,
        bool fatal,
        std::optional<DashboardConnectionRegistryFailure> failure) noexcept;

    std::size_t registeredConnectionCount_{};
    std::size_t maximumConnectionCount_{};
    DashboardIoCompletionKey deadlineCompletionKey_{0U};
    bool deadlineBridgeBound_{};
    std::uint64_t connectionDispatchCount_{};
    std::uint64_t deadlineDispatchCount_{};
    std::uint64_t retiredDeadlineDrainCount_{};
    std::uint64_t removedConnectionCount_{};
    std::uint64_t fatalNotificationCount_{};
    bool deadlineRoutingInProgress_{};
    bool shutdown_{};
    bool fatal_{};
    std::optional<DashboardConnectionRegistryFailure> failure_;
};

// Process-owned fixed registry and IOCP dispatcher for at most forty live
// dashboard connections. Entries are selected by exact typed completion key;
// deadline packets use one separate fixed key and bridge. No callback or
// shared owner destruction occurs while the registry mutex is held.
class DashboardConnectionRegistry final
    : public IDashboardIocpCompletionSink,
      public IDashboardConnectionEventFatalSink {
public:
    static constexpr std::size_t MaximumConnectionCount = 40U;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardConnectionRegistry>>
    create(DashboardIoCompletionKey deadlineCompletionKey) noexcept;

    DashboardConnectionRegistry(const DashboardConnectionRegistry&) = delete;
    DashboardConnectionRegistry& operator=(
        const DashboardConnectionRegistry&) = delete;
    DashboardConnectionRegistry(DashboardConnectionRegistry&&) = delete;
    DashboardConnectionRegistry& operator=(
        DashboardConnectionRegistry&&) = delete;
    ~DashboardConnectionRegistry() noexcept override;

    // Binding is one-shot for the registry lifetime. The concrete bridge is
    // process-owned here. Bridge calls occur outside the registry-state mutex;
    // exact reap and retirement intentionally retain deadlineRoutingMutex_.
    [[nodiscard]] Domain::Result<void> bindDeadlineBridge(
        std::shared_ptr<DashboardDeadlineIocpBridge> bridge) noexcept;

    // Captures immutable identity, registers and stores the exact fixed bridge
    // handle, inserts, then starts outside the lock so immediate native and
    // deadline completions can route safely. Production identities come from
    // the process-wide nonreusing allocator; concurrently created owners may
    // arrive here out of numeric order. A start failure retires its deadline
    // handle immediately, requests owner shutdown, and retains it until drained.
    [[nodiscard]] Domain::Result<void> registerConnection(
        std::shared_ptr<IDashboardConnectionDispatchTarget> target) noexcept;

    // Removes only an exact key/registration/generation/target identity whose
    // terminal state was observed outside the registry lock.
    [[nodiscard]] bool removeIfDrained(
        const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
        noexcept;

    void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept override;

    void fatal(DWORD nativeError) noexcept override;
    void fatal(
        DashboardConnectionEventFatalNotification notification)
        noexcept override;

    [[nodiscard]] DashboardConnectionRegistrySnapshot snapshot()
        const noexcept;

    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

    // Idempotently snapshots at most forty shared targets and the deadline
    // bridge, then requests shutdown only after releasing the registry lock.
    void beginShutdown() noexcept;

private:
    struct Entry final {
        enum class DeadlineOwnerLifecycle : std::uint8_t {
            Registered,
            Retiring,
            Retired,
        };

        DashboardIoCompletionKey key{0U};
        std::uint64_t registrationId{};
        std::uint64_t generationId{};
        DashboardDeadlineNotificationHandle deadlineHandle{};
        DeadlineOwnerLifecycle deadlineOwnerLifecycle{
            DeadlineOwnerLifecycle::Retired};
        std::shared_ptr<IDashboardConnectionDispatchTarget> target;
    };

    explicit DashboardConnectionRegistry(
        DashboardIoCompletionKey deadlineCompletionKey) noexcept;

    [[nodiscard]] Entry* findByKeyLocked(
        DashboardIoCompletionKey key) noexcept;
    [[nodiscard]] Entry* findByRegistrationIdLocked(
        std::uint64_t registrationId) noexcept;
    [[nodiscard]] Entry* findVacantLocked() noexcept;
    [[nodiscard]] bool hasDuplicateLocked(
        DashboardIoCompletionKey key,
        std::uint64_t registrationId,
        const IDashboardConnectionDispatchTarget* target) const noexcept;

    [[nodiscard]] bool removeIfDrainedIdentity(
        DashboardIoCompletionKey key,
        std::uint64_t registrationId,
        std::uint64_t generationId,
        const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
        noexcept;
    [[nodiscard]] bool retireDeadlineOwnerIdentity(
        DashboardIoCompletionKey key,
        std::uint64_t registrationId,
        std::uint64_t generationId,
        const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
        noexcept;

    void retainFatalFailureLocked(
        Domain::Error error,
        std::optional<DWORD> nativeError = std::nullopt) noexcept;
    void failRouting(
        Domain::Error error,
        std::optional<DWORD> nativeError = std::nullopt) noexcept;
    [[nodiscard]] DashboardConnectionRegistrySnapshot snapshotLocked()
        const noexcept;

    const DashboardIoCompletionKey deadlineCompletionKey_{0U};
    std::array<Entry, MaximumConnectionCount> entries_{};
    std::shared_ptr<DashboardDeadlineIocpBridge> deadlineBridge_;
    std::optional<Domain::Error> firstFailure_;
    std::optional<DashboardConnectionRegistryFailure> firstFailureSnapshot_;
    std::size_t registeredConnectionCount_{};
    std::uint64_t connectionDispatchCount_{};
    std::uint64_t deadlineDispatchCount_{};
    std::uint64_t retiredDeadlineDrainCount_{};
    std::uint64_t removedConnectionCount_{};
    std::uint64_t fatalNotificationCount_{};
    bool deadlineBridgeEverBound_{};
    bool shutdownCallbacksStarted_{};
    bool shutdown_{};
    bool fatal_{};
    std::atomic_bool deadlineRoutingInProgress_{};
    // Serializes one exact deadline reap plus target pin against exact owner
    // retirement. Acquire this before mutex_; callbacks and shared-owner
    // destruction must occur only after both locks are released.
    mutable std::mutex deadlineRoutingMutex_;
    mutable std::mutex mutex_;
};

static_assert(DashboardConnectionRegistry::MaximumConnectionCount == 40U);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
