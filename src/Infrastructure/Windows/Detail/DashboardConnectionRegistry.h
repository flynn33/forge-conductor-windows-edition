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

// Fixed listener-generation, overload-response, or process-shutdown deadline
// owner. The shared bridge and registry retain its exact mailbox generation so
// all 44 scheduler
// entries use one IOCP key and one retirement race boundary.
class IDashboardAuxiliaryDeadlineTarget {
public:
    virtual ~IDashboardAuxiliaryDeadlineTarget() noexcept = default;

    [[nodiscard]] virtual std::uint64_t registrationId() const noexcept = 0;
    virtual void dispatchDeadline(
        WindowsDashboardDeadline deadline) noexcept = 0;
    virtual void beginShutdown() noexcept = 0;
};

// Process boundary for a fatal IOCP dequeue failure. Once the shared worker
// kernel cannot deliver completions, native OVERLAPPED and deadline watchdog
// ownership cannot be proved drainable. Production terminates; focused tests
// inject a recorder that returns so retained diagnostics can be asserted.
class IDashboardConnectionRegistryFailFast {
public:
    virtual ~IDashboardConnectionRegistryFailFast() noexcept = default;
    virtual void failFast() noexcept = 0;
};

// Process-wide nonblocking edge emitted only when exact connection removal
// changes one listener generation to zero registered connections. A retiring
// generation uses it to collect its accept/deadline/router ownership without
// polling. Implementations must not retain callback arguments.
class IDashboardConnectionGenerationDrainObserver {
public:
    virtual ~IDashboardConnectionGenerationDrainObserver() noexcept = default;

    virtual void generationConnectionsMayHaveDrained(
        std::uint64_t generationId) noexcept = 0;
};

// Optional process-wide one-shot edge emitted when graceful or hard shutdown
// observes or reaches zero registered connections. Standalone registry users
// may omit this observer. Managed composition must bind it before shutdown and
// retain it strongly through final routing drain; after a successful bind,
// expiry fails closed. Implementations must remain nonblocking and must not
// retain callback arguments.
class IDashboardConnectionRegistryDrainObserver {
public:
    virtual ~IDashboardConnectionRegistryDrainObserver() noexcept = default;

    virtual void registryConnectionsMayHaveDrained() noexcept = 0;
};

// Repeatable process-shutdown routing edge. Each exact routing-ownership
// reduction advances the canonical monotonic snapshot revision. A bounded
// capacity-one dispatcher publishes the greatest pending revision after
// releasing registry and deadline-routing locks, so intermediate revisions may
// coalesce but delivered revisions never regress. A shutdown driver compares
// callback revisions with snapshot().routingProgressRevision() before sleeping,
// so progress racing its readiness check cannot be lost. Binding is one-shot;
// managed composition binds before shutdown and retains the observer strongly
// until deadline routing is finalized. Once bound, expiry fails closed once.
class IDashboardConnectionRegistryRoutingProgressObserver {
public:
    virtual ~IDashboardConnectionRegistryRoutingProgressObserver() noexcept =
        default;

    virtual void registryRoutingMayHaveProgressed(
        std::uint64_t revision) noexcept = 0;
};

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

    [[nodiscard]] std::size_t registeredAuxiliaryDeadlineTargetCount()
        const noexcept
    {
        return registeredAuxiliaryDeadlineTargetCount_;
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

    [[nodiscard]] std::uint64_t routingProgressRevision() const noexcept
    {
        return routingProgressRevision_;
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
        std::size_t registeredAuxiliaryDeadlineTargetCount,
        DashboardIoCompletionKey deadlineCompletionKey,
        bool deadlineBridgeBound,
        std::uint64_t connectionDispatchCount,
        std::uint64_t deadlineDispatchCount,
        std::uint64_t retiredDeadlineDrainCount,
        std::uint64_t removedConnectionCount,
        std::uint64_t fatalNotificationCount,
        std::uint64_t routingProgressRevision,
        bool deadlineRoutingInProgress,
        bool shutdown,
        bool fatal,
        std::optional<DashboardConnectionRegistryFailure> failure) noexcept;

    std::size_t registeredConnectionCount_{};
    std::size_t maximumConnectionCount_{};
    std::size_t registeredAuxiliaryDeadlineTargetCount_{};
    DashboardIoCompletionKey deadlineCompletionKey_{0U};
    bool deadlineBridgeBound_{};
    std::uint64_t connectionDispatchCount_{};
    std::uint64_t deadlineDispatchCount_{};
    std::uint64_t retiredDeadlineDrainCount_{};
    std::uint64_t removedConnectionCount_{};
    std::uint64_t fatalNotificationCount_{};
    std::uint64_t routingProgressRevision_{};
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
      public IDashboardConnectionEventFatalSink,
      public IDashboardConnectionDrainObserver,
      public std::enable_shared_from_this<DashboardConnectionRegistry> {
public:
    static constexpr std::size_t MaximumConnectionCount = 40U;
    static constexpr std::size_t MaximumAuxiliaryDeadlineTargetCount = 4U;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardConnectionRegistry>>
    create(DashboardIoCompletionKey deadlineCompletionKey) noexcept;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardConnectionRegistry>>
    create(
        DashboardIoCompletionKey deadlineCompletionKey,
        std::shared_ptr<IDashboardConnectionRegistryFailFast> failFast)
        noexcept;

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

    [[nodiscard]] Domain::Result<void> bindGenerationDrainObserver(
        std::weak_ptr<IDashboardConnectionGenerationDrainObserver> observer)
        noexcept;

    [[nodiscard]] Domain::Result<void> bindShutdownDrainObserver(
        std::weak_ptr<IDashboardConnectionRegistryDrainObserver> observer)
        noexcept;

    [[nodiscard]] Domain::Result<void> bindRoutingProgressObserver(
        std::weak_ptr<
            IDashboardConnectionRegistryRoutingProgressObserver> observer)
        noexcept;

    // Captures immutable identity, registers and stores the exact fixed bridge
    // handle, inserts, then starts outside the lock so immediate native and
    // deadline completions can route safely. Production identities come from
    // the process-wide nonreusing allocator; concurrently created owners may
    // arrive here out of numeric order. A start failure retires its deadline
    // handle immediately, requests owner shutdown, and retains it until drained.
    [[nodiscard]] Domain::Result<void> registerConnection(
        std::shared_ptr<IDashboardConnectionDispatchTarget> target) noexcept;

    // Registers one of two listener generations, the fixed overload-response
    // pool, or the process shutdown-drain owner with the same exact bridge used
    // by connections. The target may arm its scheduler entry only after
    // registration succeeds.
    [[nodiscard]] Domain::Result<void> registerAuxiliaryDeadlineTarget(
        std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> target) noexcept;

    // Retires the exact mailbox generation before erasing its target. The
    // caller first cancels any live scheduler arm and drains the owner.
    [[nodiscard]] bool unregisterAuxiliaryDeadlineTarget(
        const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
        noexcept;

    // Removes only an exact key/registration/generation/target identity whose
    // terminal state was observed outside the registry lock.
    [[nodiscard]] bool removeIfDrained(
        const std::shared_ptr<IDashboardConnectionDispatchTarget>& target)
        noexcept;

    [[nodiscard]] std::size_t connectionCountForGeneration(
        std::uint64_t generationId) const noexcept;

    // Force-closes only the exact listener generation at its retirement
    // deadline. The generation owner serializes accepted handoffs against the
    // transition that invokes this method, so no later owner can escape the
    // fixed snapshot. Returns the number of owners notified.
    [[nodiscard]] std::size_t beginShutdownGeneration(
        std::uint64_t generationId) noexcept;

    void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept override;

    void fatal(DWORD nativeError) noexcept override;
    void fatal(
        DashboardConnectionEventFatalNotification notification)
        noexcept override;
    void connectionMayHaveDrained(
        DashboardIoCompletionKey completionKey,
        std::uint64_t registrationId,
        std::uint64_t generationId) noexcept override;

    [[nodiscard]] DashboardConnectionRegistrySnapshot snapshot()
        const noexcept;

    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

    // Closes connection registration and asks every current target to retain
    // only an already-started immutable complete-response send. Targets are
    // invoked after releasing the registry mutex. Existing exact deadline
    // retirement and drain-edge removal remain active.
    void beginGracefulShutdown() noexcept;

    // Idempotently snapshots at most forty connection targets and four
    // auxiliary targets, then requests shutdown only after releasing the
    // registry lock. Auxiliary owners retain exact deadline routing until
    // their owning coordinator drains and unregisters them.
    void beginShutdown() noexcept;

    // May be called only after all connection and auxiliary owners have been
    // exactly removed and every posted deadline tombstone has reaped. Returns
    // false without mutation while any routing ownership remains.
    [[nodiscard]] bool finalizeDeadlineRouting() noexcept;

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

    struct AuxiliaryDeadlineEntry final {
        std::uint64_t registrationId{};
        DashboardDeadlineNotificationHandle deadlineHandle{};
        Entry::DeadlineOwnerLifecycle deadlineOwnerLifecycle{
            Entry::DeadlineOwnerLifecycle::Retired};
        std::shared_ptr<IDashboardAuxiliaryDeadlineTarget> target;
    };

    explicit DashboardConnectionRegistry(
        DashboardIoCompletionKey deadlineCompletionKey,
        std::shared_ptr<IDashboardConnectionRegistryFailFast> failFast)
        noexcept;

    [[nodiscard]] Entry* findByKeyLocked(
        DashboardIoCompletionKey key) noexcept;
    [[nodiscard]] Entry* findByRegistrationIdLocked(
        std::uint64_t registrationId) noexcept;
    [[nodiscard]] Entry* findVacantLocked() noexcept;
    [[nodiscard]] AuxiliaryDeadlineEntry*
    findAuxiliaryByRegistrationIdLocked(
        std::uint64_t registrationId) noexcept;
    [[nodiscard]] AuxiliaryDeadlineEntry* findVacantAuxiliaryLocked()
        noexcept;
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
    [[nodiscard]] bool retireAuxiliaryDeadlineOwnerIdentity(
        std::uint64_t registrationId,
        const std::shared_ptr<IDashboardAuxiliaryDeadlineTarget>& target)
        noexcept;

    void retainFatalFailureLocked(
        Domain::Error error,
        std::optional<DWORD> nativeError = std::nullopt) noexcept;
    void failRouting(
        Domain::Error error,
        std::optional<DWORD> nativeError = std::nullopt) noexcept;
    void failMissingShutdownDrainObserver() noexcept;
    void failMissingRoutingProgressObserver() noexcept;
    [[nodiscard]] bool
    advanceRoutingProgressLocked() noexcept;
    void dispatchRoutingProgress() noexcept;
    [[nodiscard]] DashboardConnectionRegistrySnapshot snapshotLocked()
        const noexcept;

    const DashboardIoCompletionKey deadlineCompletionKey_{0U};
    const std::shared_ptr<IDashboardConnectionRegistryFailFast> failFast_;
    std::array<Entry, MaximumConnectionCount> entries_{};
    std::array<
        AuxiliaryDeadlineEntry,
        MaximumAuxiliaryDeadlineTargetCount> auxiliaryDeadlineEntries_{};
    std::shared_ptr<DashboardDeadlineIocpBridge> deadlineBridge_;
    std::weak_ptr<IDashboardConnectionGenerationDrainObserver>
        generationDrainObserver_;
    std::weak_ptr<IDashboardConnectionRegistryDrainObserver>
        shutdownDrainObserver_;
    std::weak_ptr<IDashboardConnectionRegistryRoutingProgressObserver>
        routingProgressObserver_;
    std::optional<Domain::Error> firstFailure_;
    std::optional<DashboardConnectionRegistryFailure> firstFailureSnapshot_;
    std::size_t registeredConnectionCount_{};
    std::size_t registeredAuxiliaryDeadlineTargetCount_{};
    std::uint64_t connectionDispatchCount_{};
    std::uint64_t deadlineDispatchCount_{};
    std::uint64_t retiredDeadlineDrainCount_{};
    std::uint64_t removedConnectionCount_{};
    std::uint64_t fatalNotificationCount_{};
    std::uint64_t routingProgressRevision_{};
    std::uint64_t routingProgressPendingRevision_{};
    std::uint64_t routingProgressDeliveredRevision_{};
    bool deadlineBridgeEverBound_{};
    bool generationDrainObserverEverBound_{};
    bool shutdownDrainObserverEverBound_{};
    bool routingProgressObserverEverBound_{};
    bool routingProgressObserverFailureReported_{};
    bool routingProgressDispatchInProgress_{};
    bool shutdownDrainNotificationSent_{};
    bool gracefulShutdownCallbacksStarted_{};
    bool shutdownCallbacksStarted_{};
    bool shutdown_{};
    bool fatal_{};
    std::atomic_bool deadlineRoutingInProgress_{};
    // Serializes one exact deadline reap plus target pin against exact owner
    // retirement. Acquire this before mutex_; callbacks and shared-owner
    // destruction must occur only after both locks are released.
    mutable std::mutex deadlineRoutingMutex_;
    // Serializes graceful and hard callback fan-outs without holding the
    // registry data mutex. Recursive acquisition permits a callback to request
    // hard escalation on the same thread; the hard latch then stops the
    // remaining graceful fan-out before another target can observe it late.
    mutable std::recursive_mutex shutdownFanoutMutex_;
    mutable std::mutex mutex_;
};

static_assert(DashboardConnectionRegistry::MaximumConnectionCount == 40U);
static_assert(
    DashboardConnectionRegistry::MaximumAuxiliaryDeadlineTargetCount == 4U);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
