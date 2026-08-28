#pragma once

#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardConnectionEventFailureKind : std::uint8_t {
    InvalidRequest,
    Conflict,
    LimitExceeded,
    TransportClosed,
    IntegrityFailure,
    InternalFailure,
    Other,
};

// Allocation-free failure retained by snapshots and passed to the runtime
// fatal sink. String-owning diagnostics remain available through the explicit
// fullFailure() copy path.
struct DashboardConnectionEventFailure final {
    DashboardConnectionEventFailureKind kind{
        DashboardConnectionEventFailureKind::Other};
    bool retryable{};

    bool operator==(const DashboardConnectionEventFailure&) const = default;
};

struct DashboardConnectionEventFatalNotification final {
    std::uint64_t ownerRegistrationId{};
    DashboardConnectionEventFailure failure;

    bool operator==(
        const DashboardConnectionEventFatalNotification&) const = default;
};

// Process-runtime callback for a connection event bridge that can no longer
// preserve its exact bounded completion contract. Implementations must remain
// nonblocking. The bridge invokes this only after releasing its own mutex.
class IDashboardConnectionEventFatalSink {
public:
    virtual ~IDashboardConnectionEventFatalSink() noexcept = default;

    virtual void fatal(
        DashboardConnectionEventFatalNotification notification) noexcept = 0;
};

enum class DashboardConnectionEventReapDisposition : std::uint8_t {
    PayloadDelivered,
    RetiredNotificationDrained,
};

// Move-only ownership of one atomically reaped connection event packet. The
// optional handler result can leave this object exactly once; the SSE bit is a
// coalesced observation and carries no queued frame.
class DashboardConnectionEventReapResult final {
public:
    DashboardConnectionEventReapResult(
        const DashboardConnectionEventReapResult&) = delete;
    DashboardConnectionEventReapResult& operator=(
        const DashboardConnectionEventReapResult&) = delete;
    DashboardConnectionEventReapResult(
        DashboardConnectionEventReapResult&&) noexcept = default;
    DashboardConnectionEventReapResult& operator=(
        DashboardConnectionEventReapResult&&) noexcept = default;
    ~DashboardConnectionEventReapResult() = default;

    [[nodiscard]] DashboardConnectionEventReapDisposition disposition()
        const noexcept
    {
        return disposition_;
    }

    [[nodiscard]] bool hasHandlerCompletion() const noexcept
    {
        return handlerCompletion_.has_value();
    }

    [[nodiscard]] const DashboardHandlerCompletion* handlerCompletion()
        const noexcept
    {
        return handlerCompletion_.has_value()
            ? std::addressof(*handlerCompletion_)
            : nullptr;
    }

    [[nodiscard]] std::optional<DashboardHandlerCompletion>
    takeHandlerCompletion() noexcept;

    [[nodiscard]] bool sseReady() const noexcept { return sseReady_; }

private:
    friend class DashboardConnectionEventBridge;

    DashboardConnectionEventReapResult(
        DashboardConnectionEventReapDisposition disposition,
        std::optional<DashboardHandlerCompletion> handlerCompletion,
        bool sseReady) noexcept;

    DashboardConnectionEventReapDisposition disposition_{
        DashboardConnectionEventReapDisposition::
            RetiredNotificationDrained};
    std::optional<DashboardHandlerCompletion> handlerCompletion_;
    bool sseReady_{};
};

class DashboardConnectionEventSnapshot final {
public:
    [[nodiscard]] std::uint64_t ownerRegistrationId() const noexcept
    {
        return ownerRegistrationId_;
    }

    [[nodiscard]] std::size_t postedOperationCount() const noexcept
    {
        return postedOperationCount_;
    }

    [[nodiscard]] std::size_t handlerPayloadCount() const noexcept
    {
        return handlerPayloadCount_;
    }

    [[nodiscard]] bool sseReadyLatched() const noexcept
    {
        return sseReadyLatched_;
    }

    [[nodiscard]] bool tombstoneAwaitingReap() const noexcept
    {
        return tombstoneAwaitingReap_;
    }

    [[nodiscard]] std::uint64_t successfulPostCount() const noexcept
    {
        return successfulPostCount_;
    }

    [[nodiscard]] std::uint64_t coalescedSignalCount() const noexcept
    {
        return coalescedSignalCount_;
    }

    [[nodiscard]] std::uint64_t deliveredPayloadCount() const noexcept
    {
        return deliveredPayloadCount_;
    }

    [[nodiscard]] std::uint64_t drainedTombstoneCount() const noexcept
    {
        return drainedTombstoneCount_;
    }

    [[nodiscard]] bool isShutdown() const noexcept { return shutdown_; }
    [[nodiscard]] bool isFatal() const noexcept { return fatal_; }

    [[nodiscard]] bool fullyDrained() const noexcept
    {
        return postedOperationCount_ == 0U;
    }

    [[nodiscard]] const DashboardConnectionEventFailure* failure()
        const noexcept
    {
        return failure_.has_value() ? std::addressof(*failure_) : nullptr;
    }

private:
    friend class DashboardConnectionEventBridge;

    DashboardConnectionEventSnapshot(
        std::uint64_t ownerRegistrationId,
        std::size_t postedOperationCount,
        std::size_t handlerPayloadCount,
        bool sseReadyLatched,
        bool tombstoneAwaitingReap,
        std::uint64_t successfulPostCount,
        std::uint64_t coalescedSignalCount,
        std::uint64_t deliveredPayloadCount,
        std::uint64_t drainedTombstoneCount,
        bool shutdown,
        bool fatal,
        std::optional<DashboardConnectionEventFailure> failure) noexcept;

    std::uint64_t ownerRegistrationId_{};
    std::size_t postedOperationCount_{};
    std::size_t handlerPayloadCount_{};
    bool sseReadyLatched_{};
    bool tombstoneAwaitingReap_{};
    std::uint64_t successfulPostCount_{};
    std::uint64_t coalescedSignalCount_{};
    std::uint64_t deliveredPayloadCount_{};
    std::uint64_t drainedTombstoneCount_{};
    bool shutdown_{};
    bool fatal_{};
    std::optional<DashboardConnectionEventFailure> failure_;
};

// One connection's heap-stable, capacity-one synthetic IOCP bridge. Handler
// completion and SSE-ready publication share exactly one stable OVERLAPPED and
// at most one queued packet. The process kernel is borrowed and must outlive
// this bridge and its exact drain obligation.
class DashboardConnectionEventBridge final
    : public IDashboardHandlerCompletionSink,
      public Dashboard::IDashboardSseReadySink,
      private std::enable_shared_from_this<DashboardConnectionEventBridge> {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardConnectionEventBridge>>
    create(
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey,
        std::uint64_t ownerRegistrationId,
        std::weak_ptr<IDashboardConnectionEventFatalSink> fatalSink) noexcept;

    DashboardConnectionEventBridge(
        const DashboardConnectionEventBridge&) = delete;
    DashboardConnectionEventBridge& operator=(
        const DashboardConnectionEventBridge&) = delete;
    DashboardConnectionEventBridge(
        DashboardConnectionEventBridge&&) = delete;
    DashboardConnectionEventBridge& operator=(
        DashboardConnectionEventBridge&&) = delete;
    ~DashboardConnectionEventBridge() noexcept override;

    // Consumes completion on every path. A live handler payload can share an
    // already-posted SSE packet; a second handler payload is structural
    // corruption and closes the bridge fatally.
    [[nodiscard]] bool tryPost(
        DashboardHandlerCompletion completion) noexcept override;

    // Latches one allocation-free SSE-ready bit. Repeated calls while ready is
    // already represented or a packet is posted never enqueue another packet.
    void signal() noexcept override;

    // Foreign operation addresses do not mutate this owner. Once the exact
    // address has been dequeued, malformed key/byte/error shapes consume its
    // posted obligation before reporting fatal integrity failure.
    [[nodiscard]] Domain::Result<DashboardConnectionEventReapResult> reap(
        DashboardIoCompletionKey completionKey,
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept;

    [[nodiscard]] DashboardConnectionEventSnapshot snapshot() const noexcept;

    // Explicit allocation-permitted copy. snapshot() remains fixed-size and
    // noexcept.
    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

    // Idempotently rejects/discards later payload. A packet already accepted
    // by the kernel becomes a tombstone and retains stable storage until reap.
    void shutdown() noexcept;

private:
    DashboardConnectionEventBridge(
        DashboardIocpWorkerKernel& kernel,
        DashboardIoCompletionKey completionKey,
        std::uint64_t ownerRegistrationId,
        std::weak_ptr<IDashboardConnectionEventFatalSink> fatalSink) noexcept;

    [[nodiscard]] bool postLocked(
        std::optional<DashboardConnectionEventFatalNotification>&
            notification,
        std::optional<DashboardHandlerCompletion>& discarded) noexcept;
    void transitionToShutdownLocked(
        std::optional<DashboardHandlerCompletion>& discarded) noexcept;
    [[nodiscard]] std::optional<DashboardConnectionEventFatalNotification>
    retainFatalFailureLocked(
        Domain::Error error,
        std::optional<DashboardHandlerCompletion>& discarded) noexcept;
    [[nodiscard]] DashboardConnectionEventSnapshot snapshotLocked()
        const noexcept;
    void notifyFatal(
        std::optional<DashboardConnectionEventFatalNotification>
            notification) noexcept;

    DashboardIocpWorkerKernel* kernel_{};
    DashboardIoCompletionKey completionKey_{0U};
    const std::uint64_t ownerRegistrationId_{};
    std::weak_ptr<IDashboardConnectionEventFatalSink> fatalSink_;
    OVERLAPPED operation_{};
    std::optional<DashboardHandlerCompletion> handlerCompletion_;
    std::optional<Domain::Error> firstFailure_;
    std::optional<DashboardConnectionEventFailure> firstFailureSnapshot_;
    std::uint64_t successfulPostCount_{};
    std::uint64_t coalescedSignalCount_{};
    std::uint64_t deliveredPayloadCount_{};
    std::uint64_t drainedTombstoneCount_{};
    bool sseReadyLatched_{};
    bool posted_{};
    bool tombstone_{};
    bool shutdown_{};
    bool fatal_{};
    mutable std::mutex mutex_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
