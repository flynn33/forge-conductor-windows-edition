#pragma once

#include "DashboardAcceptedConnectionHandoff.h"
#include "DashboardConnectionResponseCatalog.h"
#include "DashboardConnectionSocket.h"
#include "DashboardIocpCompletionRouter.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardOverloadAdmissionDisposition : std::uint8_t {
    ResponseStarted,
    ConnectionClosed,
    ClosedDuringShutdown,
};

enum class DashboardOverloadReapDisposition : std::uint8_t {
    PartialSendReissued,
    ResponseDelivered,
    ResponseAbandoned,
};

enum class DashboardOverloadResponderFailureKind : std::uint8_t {
    Cancelled,
    TransportClosed,
    Unauthorized,
    LimitExceeded,
    IntegrityFailure,
    InternalFailure,
    Other,
};

struct DashboardOverloadResponderFailure final {
    DashboardOverloadResponderFailureKind kind{
        DashboardOverloadResponderFailureKind::Other};
    bool retryable{};

    bool operator==(const DashboardOverloadResponderFailure&) const = default;
};

// Process boundary used only when native OVERLAPPED ownership has exceeded
// its bounded cancellation-reap deadline. Production terminates immediately;
// focused tests substitute an allocation-free recorder.
class IDashboardOverloadFailFast {
public:
    virtual ~IDashboardOverloadFailFast() noexcept = default;
    virtual void failFast() noexcept = 0;
};

// Synchronous borrowed-socket association boundary. The responder pins its
// exact work item while this call is in progress so cancellation cannot close
// and release the numeric SOCKET value before IOCP association returns.
class IDashboardOverloadSocketAssociator {
public:
    virtual ~IDashboardOverloadSocketAssociator() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> associateSocket(
        SOCKET socket,
        DashboardIoCompletionKey completionKey) noexcept = 0;
};

class DashboardOverloadResponderSnapshot final {
public:
    [[nodiscard]] std::size_t associatingCount() const noexcept
    {
        return associatingCount_;
    }

    [[nodiscard]] std::size_t sendingCount() const noexcept
    {
        return sendingCount_;
    }

    [[nodiscard]] std::size_t cancellationRequestedCount() const noexcept
    {
        return cancellationRequestedCount_;
    }

    [[nodiscard]] std::size_t activeCount() const noexcept
    {
        return associatingCount_ + sendingCount_ +
            cancellationRequestedCount_;
    }

    [[nodiscard]] std::size_t availableCount() const noexcept
    {
        return maximumCount_ - activeCount();
    }

    [[nodiscard]] std::size_t maximumCount() const noexcept
    {
        return maximumCount_;
    }

    [[nodiscard]] std::uint64_t deliveredCount() const noexcept
    {
        return deliveredCount_;
    }

    [[nodiscard]] std::uint64_t abandonedCount() const noexcept
    {
        return abandonedCount_;
    }

    [[nodiscard]] bool deadlineArmed() const noexcept
    {
        return deadlineArmed_;
    }

    [[nodiscard]] std::uint64_t expiredCount() const noexcept
    {
        return expiredCount_;
    }

    [[nodiscard]] std::uint64_t staleDeadlineCount() const noexcept
    {
        return staleDeadlineCount_;
    }

    [[nodiscard]] std::uint64_t failFastCount() const noexcept
    {
        return failFastCount_;
    }

    [[nodiscard]] bool shutdownRequested() const noexcept
    {
        return shutdownRequested_;
    }

    [[nodiscard]] bool fullyDrained() const noexcept
    {
        return shutdownRequested_ && activeCount() == 0U;
    }

    [[nodiscard]] const DashboardOverloadResponderFailure*
    lifecycleFailure() const noexcept
    {
        return lifecycleFailure_.has_value()
            ? std::addressof(*lifecycleFailure_)
            : nullptr;
    }

private:
    friend class DashboardOverloadResponderSet;

    DashboardOverloadResponderSnapshot(
        std::size_t associatingCount,
        std::size_t sendingCount,
        std::size_t cancellationRequestedCount,
        std::size_t maximumCount,
        std::uint64_t deliveredCount,
        std::uint64_t abandonedCount,
        bool deadlineArmed,
        std::uint64_t expiredCount,
        std::uint64_t staleDeadlineCount,
        std::uint64_t failFastCount,
        bool shutdownRequested,
        std::optional<DashboardOverloadResponderFailure>
            lifecycleFailure) noexcept;

    std::size_t associatingCount_{};
    std::size_t sendingCount_{};
    std::size_t cancellationRequestedCount_{};
    std::size_t maximumCount_{};
    std::uint64_t deliveredCount_{};
    std::uint64_t abandonedCount_{};
    bool deadlineArmed_{};
    std::uint64_t expiredCount_{};
    std::uint64_t staleDeadlineCount_{};
    std::uint64_t failFastCount_{};
    bool shutdownRequested_{};
    std::optional<DashboardOverloadResponderFailure> lifecycleFailure_;
};

// Process-owned fixed 503 transport. Exactly eight heap-stable entries match
// the maximum four withheld AcceptEx tokens from each of the active and
// retiring listener generations. No responder thread or user-space queue
// exists. Each live entry owns the handoff work, immutable response bytes,
// exact OVERLAPPED, and WSABUF until matching IOCP reap.
class DashboardOverloadResponderSet final
    : public IDashboardFixedIocpCompletionTarget,
      public IDashboardAdmissionOverloadResponder,
      public IDashboardAuxiliaryDeadlineTarget {
public:
    static constexpr std::size_t Capacity =
        2U * DashboardAcceptSlotSet::SlotCount;
    static constexpr auto ResponseLifetime = std::chrono::seconds{15};
    static constexpr auto CancellationReapLifetime =
        std::chrono::seconds{5};

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardOverloadResponderSet>>
    create(
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey,
        std::uint64_t deadlineRegistrationId,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        const DashboardConnectionResponseCatalog& responseCatalog) noexcept;

    // Test seam. This native API does not own or close sockets; exact accepted
    // ownership remains inside DashboardAdmissionOverloadWork.
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardOverloadResponderSet>>
    create(
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey,
        std::uint64_t deadlineRegistrationId,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        const DashboardConnectionResponseCatalog& responseCatalog,
        std::shared_ptr<IDashboardConnectionSocketApi> socketApi,
        std::shared_ptr<IDashboardOverloadFailFast> failFast) noexcept;

    // Deterministic association seam for the exact borrowed-handle lifetime
    // race. The injected owner performs no socket close and must return before
    // this responder releases its association pin.
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardOverloadResponderSet>>
    create(
        DashboardIoCompletionKey completionKey,
        std::uint64_t deadlineRegistrationId,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        const DashboardConnectionResponseCatalog& responseCatalog,
        std::shared_ptr<IDashboardConnectionSocketApi> socketApi,
        std::shared_ptr<IDashboardOverloadFailFast> failFast,
        std::shared_ptr<IDashboardOverloadSocketAssociator>
            socketAssociator) noexcept;

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<IDashboardAdmissionOverloadDrainObserver> observer)
        noexcept;

    DashboardOverloadResponderSet(
        const DashboardOverloadResponderSet&) = delete;
    DashboardOverloadResponderSet& operator=(
        const DashboardOverloadResponderSet&) = delete;
    DashboardOverloadResponderSet(
        DashboardOverloadResponderSet&&) = delete;
    DashboardOverloadResponderSet& operator=(
        DashboardOverloadResponderSet&&) = delete;
    ~DashboardOverloadResponderSet() noexcept override;

    [[nodiscard]] DashboardIoCompletionKey completionKey()
        const noexcept override
    {
        return completionKey_;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return deadlineRegistrationId_;
    }

    void dispatchDeadline(
        WindowsDashboardDeadline deadline) noexcept override;

    // Typed ownership primitive for focused tests. All paths consume work:
    // successful issue retains it in one fixed slot; every other path closes
    // the socket and returns the exact paused accept token before returning.
    [[nodiscard]] Domain::Result<DashboardOverloadAdmissionDisposition> admit(
        DashboardAdmissionOverloadWork work) noexcept;

    // Handoff-facing one-way boundary. Failures are retained in the bounded
    // lifecycle diagnostic and trigger terminal cancellation when required.
    void respond(DashboardAdmissionOverloadWork work) noexcept override;

    [[nodiscard]] std::size_t cancelGeneration(
        std::uint64_t generationId) noexcept override;

    void drainTerminalGenerationNotifications() noexcept override;

    [[nodiscard]] Domain::Result<DashboardOverloadReapDisposition> reap(
        DashboardIoCompletionKey completionKey,
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept;

    void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept override;

    void fatal(DWORD nativeError) noexcept override;

    // Idempotently closes every held work item's exact originating accept
    // generation before cancelling sends. A live OVERLAPPED is never released;
    // exact IOCP reap remains mandatory before fullyDrained().
    void beginShutdown() noexcept override;

    [[nodiscard]] DashboardOverloadResponderSnapshot snapshot()
        const noexcept;

    [[nodiscard]] std::optional<Domain::Error> fullLifecycleFailure() const;

private:
    enum class SlotLifecycle : std::uint8_t {
        Empty,
        Associating,
        Sending,
        CancellationRequested,
    };

    struct Slot final {
        SlotLifecycle lifecycle{SlotLifecycle::Empty};
        std::optional<DashboardAdmissionOverloadWork> work;
        OVERLAPPED operation{};
        WSABUF activeBuffer{};
        DWORD immediateTransferredBytes{};
        std::size_t sendOffset{};
        Domain::MonotonicTimePoint deadlineAt{};
        Domain::MonotonicTimePoint cancellationDeadlineAt{};
        bool socketShutdownRequested{};
        bool cancellationRequested{};
        bool closeOriginAdmissionRequested{};
        bool nativeCancellationFailure{};
    };

    struct DetachedWork final {
        DetachedWork() noexcept = default;

        explicit DetachedWork(
            std::optional<DashboardAdmissionOverloadWork> value) noexcept
            : work{std::move(value)}
        {
        }

        DetachedWork(const DetachedWork&) = delete;
        DetachedWork& operator=(const DetachedWork&) = delete;
        DetachedWork(DetachedWork&&) noexcept = default;

        DetachedWork& operator=(DetachedWork&& other) noexcept
        {
            if (this != &other) {
                work.reset();
                if (other.work.has_value()) {
                    work.emplace(std::move(*other.work));
                    other.work.reset();
                }
            }
            return *this;
        }

        std::optional<DashboardAdmissionOverloadWork> work;
    };

    enum class TerminalGenerationNotificationLifecycle : std::uint8_t {
        Pending,
        Delivering,
        Delivered,
    };

    enum class TerminalOwnerNotificationLifecycle : std::uint8_t {
        None,
        Pending,
        Delivering,
        Delivered,
    };

    DashboardOverloadResponderSet(
        DashboardIoCompletionKey completionKey,
        std::uint64_t deadlineRegistrationId,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        DashboardConnectionResponseCatalog::ImmutableBytes responseBytes,
        std::shared_ptr<IDashboardConnectionSocketApi> socketApi,
        std::shared_ptr<IDashboardOverloadFailFast> failFast,
        std::shared_ptr<IDashboardOverloadSocketAssociator>
            socketAssociator) noexcept;

    [[nodiscard]] std::optional<std::size_t> findEmptyLocked()
        const noexcept;
    [[nodiscard]] std::optional<std::size_t> findOperationLocked(
        OVERLAPPED* operation) const noexcept;
    [[nodiscard]] Domain::Result<
        DashboardConnectionSocketIssueDisposition>
    issueSendLocked(std::size_t index) noexcept;
    void resetOperationLocked(std::size_t index) noexcept;
    [[nodiscard]] DetachedWork detachLocked(std::size_t index) noexcept;
    [[nodiscard]] bool requestCancellationLocked(
        Slot& slot,
        bool closeOriginAdmission) noexcept;
    void requestAllCancellationsLocked() noexcept;
    [[nodiscard]] std::optional<Domain::Error> finishDetached(
        DetachedWork&& detached,
        bool closeOriginAdmission) noexcept;
    void notifyGenerationMayHaveDrained(
        std::uint64_t generationId) noexcept;
    void notifyGenerationCompletionPending(
        std::uint64_t generationId) noexcept;
    void notifyGenerationCompletionSettled(
        std::uint64_t generationId) noexcept;
    void rememberTerminalGenerationLocked(
        std::uint64_t generationId) noexcept;
    void rememberTerminalOwnerLocked() noexcept;
    void rememberAllLiveTerminalGenerationsLocked() noexcept;
    [[nodiscard]] bool terminalGenerationNotificationDeliveredLocked(
        std::uint64_t generationId) const noexcept;
    void notifyPendingTerminalGenerationLatches() noexcept;
    void notifyPendingTerminalOwner() noexcept;
    void notifyPendingTerminalGenerations() noexcept;
    void beginTerminalShutdown(
        std::optional<std::uint64_t> generationId) noexcept;
    void retainFailure(Domain::Error error) noexcept;
    void retainFailureLocked(Domain::Error error) noexcept;
    void refreshDeadline() noexcept;
    [[nodiscard]] bool failDeadlineScheduleLocked(
        Domain::Error error) noexcept;
    [[nodiscard]] DashboardOverloadResponderSnapshot snapshotLocked(
        bool deadlineArmed) const noexcept;

    const DashboardIoCompletionKey completionKey_{0U};
    const std::uint64_t deadlineRegistrationId_{};
    WindowsDashboardDeadlineScheduler* deadlineScheduler_{};
    DashboardConnectionRuntimeServices* runtimeServices_{};
    const DashboardConnectionResponseCatalog::ImmutableBytes responseBytes_;
    const std::shared_ptr<IDashboardConnectionSocketApi> socketApi_;
    const std::shared_ptr<IDashboardOverloadFailFast> failFast_;
    const std::shared_ptr<IDashboardOverloadSocketAssociator>
        socketAssociator_;
    std::array<Slot, Capacity> slots_{};
    std::uint64_t deliveredCount_{};
    std::uint64_t abandonedCount_{};
    std::uint64_t expiredCount_{};
    std::uint64_t staleDeadlineCount_{};
    std::uint64_t failFastCount_{};
    bool shutdownRequested_{};
    bool terminalShutdownRequested_{};
    TerminalOwnerNotificationLifecycle terminalOwnerNotificationLifecycle_{
        TerminalOwnerNotificationLifecycle::None};
    std::optional<Domain::Error> firstLifecycleFailure_;
    std::optional<DashboardOverloadResponderFailure>
        firstLifecycleFailureSnapshot_;
    std::weak_ptr<IDashboardAdmissionOverloadDrainObserver> drainObserver_;
    bool drainObserverEverBound_{};
    std::array<std::uint64_t, Capacity> terminalGenerationIds_{};
    std::array<TerminalGenerationNotificationLifecycle, Capacity>
        terminalGenerationNotificationLifecycles_{};
    std::array<bool, Capacity> terminalGenerationPendingLatchDelivered_{};
    std::size_t terminalGenerationCount_{};
    std::optional<WindowsDashboardDeadline> currentDeadline_;
    // Serializes the public normal-shutdown publication and cancellation
    // protocol. The coordinator callback may re-enter cancelGeneration(), so
    // this mutex is never held together with mutex_.
    mutable std::mutex shutdownTransitionMutex_;
    // Lock order is deadlineMutex_ then mutex_. No scheduler callback is made
    // while mutex_ is held, and no caller acquires deadlineMutex_ from mutex_.
    mutable std::mutex deadlineMutex_;
    mutable std::mutex mutex_;
};

static_assert(
    DashboardOverloadResponderSet::Capacity ==
    2U * DashboardAcceptSlotSet::SlotCount);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
