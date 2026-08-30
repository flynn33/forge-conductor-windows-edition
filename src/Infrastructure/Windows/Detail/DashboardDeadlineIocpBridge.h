#pragma once

#include "DashboardDeadlineNotificationMailbox.h"
#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardDeadlineIocpFailureKind : std::uint8_t {
    InvalidRequest,
    Conflict,
    LimitExceeded,
    TransportClosed,
    IntegrityFailure,
    InternalFailure,
    Other,
};

// Allocation-free failure classification retained for status and shutdown
// diagnostics. The full string-owning error is available through the explicit
// copy path on DashboardDeadlineIocpBridge.
struct DashboardDeadlineIocpFailure final {
    DashboardDeadlineIocpFailureKind kind{
        DashboardDeadlineIocpFailureKind::Other};
    bool retryable{};

    bool operator==(const DashboardDeadlineIocpFailure&) const = default;
};

// Optional one-shot process edge for the first fatal bridge transition.
// Standalone bridge users may omit it. Managed composition binds before any
// deadline can be published and retains the observer strongly through either
// this edge or final routing shutdown. The callback runs after the bridge
// mutex is released and must only latch failure and wake its external driver.
class IDashboardDeadlineIocpBridgeFailureObserver {
public:
    virtual ~IDashboardDeadlineIocpBridgeFailureObserver() noexcept =
        default;

    virtual void dashboardDeadlineIocpBridgeFailed(
        DashboardDeadlineIocpFailure failure) noexcept = 0;
};

enum class DashboardDeadlineIocpReapDisposition : std::uint8_t {
    DeadlineDelivered,
    RetiredNotificationDrained,
};

class DashboardDeadlineIocpReapResult final {
public:
    [[nodiscard]] DashboardDeadlineIocpReapDisposition disposition()
        const noexcept
    {
        return disposition_;
    }

    [[nodiscard]] const WindowsDashboardDeadline* deadline() const noexcept
    {
        return deadline_.has_value()
            ? std::addressof(*deadline_)
            : nullptr;
    }

private:
    friend class DashboardDeadlineIocpBridge;

    DashboardDeadlineIocpReapResult(
        DashboardDeadlineIocpReapDisposition disposition,
        std::optional<WindowsDashboardDeadline> deadline) noexcept;

    DashboardDeadlineIocpReapDisposition disposition_{
        DashboardDeadlineIocpReapDisposition::RetiredNotificationDrained};
    std::optional<WindowsDashboardDeadline> deadline_;
};

class DashboardDeadlineIocpSnapshot final {
public:
    [[nodiscard]] std::size_t registeredOwnerCount() const noexcept
    {
        return registeredOwnerCount_;
    }

    [[nodiscard]] std::size_t postedOperationCount() const noexcept
    {
        return postedOperationCount_;
    }

    [[nodiscard]] std::size_t retiredAwaitingReapCount() const noexcept
    {
        return retiredAwaitingReapCount_;
    }

    [[nodiscard]] std::size_t maximumOwnerCount() const noexcept
    {
        return maximumOwnerCount_;
    }

    [[nodiscard]] std::uint64_t successfulPostCount() const noexcept
    {
        return successfulPostCount_;
    }

    [[nodiscard]] std::uint64_t coalescedSignalCount() const noexcept
    {
        return coalescedSignalCount_;
    }

    [[nodiscard]] std::uint64_t deliveredDeadlineCount() const noexcept
    {
        return deliveredDeadlineCount_;
    }

    [[nodiscard]] std::uint64_t drainedRetiredCount() const noexcept
    {
        return drainedRetiredCount_;
    }

    [[nodiscard]] bool isShutdown() const noexcept { return shutdown_; }
    [[nodiscard]] bool isFatal() const noexcept { return fatal_; }

    [[nodiscard]] const DashboardDeadlineIocpFailure* failure()
        const noexcept
    {
        return failure_.has_value()
            ? std::addressof(*failure_)
            : nullptr;
    }

private:
    friend class DashboardDeadlineIocpBridge;

    DashboardDeadlineIocpSnapshot(
        std::size_t registeredOwnerCount,
        std::size_t postedOperationCount,
        std::size_t retiredAwaitingReapCount,
        std::size_t maximumOwnerCount,
        std::uint64_t successfulPostCount,
        std::uint64_t coalescedSignalCount,
        std::uint64_t deliveredDeadlineCount,
        std::uint64_t drainedRetiredCount,
        bool shutdown,
        bool fatal,
        std::optional<DashboardDeadlineIocpFailure> failure) noexcept;

    std::size_t registeredOwnerCount_{};
    std::size_t postedOperationCount_{};
    std::size_t retiredAwaitingReapCount_{};
    std::size_t maximumOwnerCount_{};
    std::uint64_t successfulPostCount_{};
    std::uint64_t coalescedSignalCount_{};
    std::uint64_t deliveredDeadlineCount_{};
    std::uint64_t drainedRetiredCount_{};
    bool shutdown_{};
    bool fatal_{};
    std::optional<DashboardDeadlineIocpFailure> failure_;
};

// Process-owned synthetic-completion bridge for all 44 bounded dashboard
// deadline owners. Every mailbox index has one independently heap-allocated,
// permanently addressed OVERLAPPED slot. A slot retains the exact mailbox
// generation handle from publication until its sole IOCP packet is consumed.
// DashboardIocpWorkerKernel and its completion dispatcher are borrowed and
// must outlive this bridge and every outstanding synthetic packet.
class DashboardDeadlineIocpBridge final
    : public IWindowsDashboardDeadlineSink {
public:
    class FailureNotificationDeferral final {
    public:
        FailureNotificationDeferral() noexcept = default;
        FailureNotificationDeferral(
            FailureNotificationDeferral&& other) noexcept;
        FailureNotificationDeferral& operator=(
            FailureNotificationDeferral&& other) noexcept;
        FailureNotificationDeferral(
            const FailureNotificationDeferral&) = delete;
        FailureNotificationDeferral& operator=(
            const FailureNotificationDeferral&) = delete;
        ~FailureNotificationDeferral() noexcept;

        // Releases the exact dispatch deferral. The final release publishes
        // a pending first-fatal edge synchronously on this thread.
        void release() noexcept;

    private:
        friend class DashboardDeadlineIocpBridge;

        explicit FailureNotificationDeferral(
            DashboardDeadlineIocpBridge& owner) noexcept;

        DashboardDeadlineIocpBridge* owner_{};
    };

    static constexpr std::size_t SlotCount =
        DashboardDeadlineNotificationMailbox::HardMaximumOwnerCount;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardDeadlineIocpBridge>>
    create(
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey) noexcept;

    DashboardDeadlineIocpBridge(const DashboardDeadlineIocpBridge&) = delete;
    DashboardDeadlineIocpBridge& operator=(
        const DashboardDeadlineIocpBridge&) = delete;
    DashboardDeadlineIocpBridge(DashboardDeadlineIocpBridge&&) = delete;
    DashboardDeadlineIocpBridge& operator=(
        DashboardDeadlineIocpBridge&&) = delete;
    ~DashboardDeadlineIocpBridge() noexcept override;

    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept
    {
        return completionKey_;
    }

    [[nodiscard]] Domain::Result<void> bindFailureObserver(
        std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver> observer)
        noexcept;

    // Prevents the managed observer callback from overlapping an external
    // routing transaction. Acquisition waits for any callback already in
    // progress; bridge mutations continue to latch fatal state while held.
    // Destroy or release the token only after every external routing/state
    // lock owned by that transaction has been released. The token borrows the
    // bridge and must not outlive it.
    [[nodiscard]] FailureNotificationDeferral
    deferFailureNotificationDispatch() noexcept;

    [[nodiscard]] Domain::Result<DashboardDeadlineNotificationHandle>
    registerOwner(std::uint64_t registrationId) noexcept;

    [[nodiscard]] Domain::Result<void> retireOwner(
        const DashboardDeadlineNotificationHandle& handle) noexcept;

    // Called by the single deadline scheduler worker. Publication is bounded
    // and nonblocking. Only the empty-to-pending transition posts to IOCP;
    // later arms for that owner replace the immutable mailbox value in place.
    void signal(WindowsDashboardDeadline deadline) noexcept override;

    // The completion dispatcher routes the bridge's exact key here. The
    // bridge still validates key, zero-byte synthetic shape, operation
    // identity, and the generation-bearing mailbox handle before delivery.
    [[nodiscard]] Domain::Result<DashboardDeadlineIocpReapResult> reap(
        DashboardIoCompletionKey completionKey,
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept;

    [[nodiscard]] DashboardDeadlineIocpSnapshot snapshot() const noexcept;

    // Explicit allocation-permitted diagnostic copy. snapshot() remains a
    // fixed-size, allocation-free, noexcept status path.
    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

    // Idempotently closes registration/publication and converts live posted
    // slots to tombstones. Already-posted packets remain an exact bounded
    // drain obligation and must be reaped before destruction.
    void shutdown() noexcept;

private:
    class FailureDispatchGuard;

    enum class SlotLifecycle : std::uint8_t {
        Vacant,
        Registered,
        Posted,
        PostedRetired,
    };

    struct NotificationSlot final {
        OVERLAPPED operation{};
        DashboardDeadlineNotificationHandle handle{};
        SlotLifecycle lifecycle{SlotLifecycle::Vacant};
    };

    using SlotOwners =
        std::array<std::unique_ptr<NotificationSlot>, SlotCount>;

    DashboardDeadlineIocpBridge(
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey,
        std::unique_ptr<DashboardDeadlineNotificationMailbox> mailbox,
        SlotOwners slots) noexcept;

    [[nodiscard]] Domain::Result<std::size_t> findOperationLocked(
        OVERLAPPED* operation) const noexcept;
    [[nodiscard]] bool validatePostedHandleLocked(
        std::size_t index) const noexcept;
    void drainExactPostedHandleLocked(std::size_t index) noexcept;
    void transitionToShutdownLocked() noexcept;
    void retainFatalFailureLocked(Domain::Error error) noexcept;
    void dispatchFatalFailureIfRequired() noexcept;
    void releaseFailureNotificationDeferral() noexcept;
    [[nodiscard]] DashboardDeadlineIocpSnapshot snapshotLocked()
        const noexcept;

    DashboardIocpWorkerKernel* kernel_{};
    DashboardIoCompletionKey completionKey_{0U};
    std::unique_ptr<DashboardDeadlineNotificationMailbox> mailbox_;
    SlotOwners slots_;
    std::optional<Domain::Error> firstFailure_;
    std::optional<DashboardDeadlineIocpFailure> firstFailureSnapshot_;
    std::weak_ptr<IDashboardDeadlineIocpBridgeFailureObserver>
        failureObserver_;
    std::uint64_t successfulPostCount_{};
    std::uint64_t coalescedSignalCount_{};
    std::uint64_t deliveredDeadlineCount_{};
    std::uint64_t drainedRetiredCount_{};
    bool shutdown_{};
    bool fatal_{};
    bool failureObserverEverBound_{};
    bool failureNotificationSent_{};
    mutable std::mutex mutex_;
    std::size_t failureNotificationDeferralCount_{};
    bool failureNotificationDispatchInProgress_{};
    std::condition_variable failureNotificationDispatchChanged_;
    mutable std::mutex failureNotificationDispatchMutex_;
};

static_assert(
    DashboardDeadlineIocpBridge::SlotCount ==
    WindowsDashboardDeadlineScheduler::HardMaximumScheduledCount);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
