#pragma once

#include "DashboardAcceptSlot.h"
#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class DashboardAcceptSlotSetIdentity;

enum class DashboardAcceptReapDisposition : std::uint8_t {
    AcceptedAndPaused,
    AcceptedAndDrained,
    FailureReissued,
    FailureDrained,
};

enum class DashboardAcceptResumeDisposition : std::uint8_t {
    Reissued,
    ReturnedAfterClose,
};

enum class DashboardAcceptLifecycleFailureKind : std::uint8_t {
    Cancelled,
    TransportClosed,
    Unauthorized,
    LimitExceeded,
    InternalFailure,
    Other,
};

struct DashboardAcceptLifecycleFailure final {
    DashboardAcceptLifecycleFailureKind kind{
        DashboardAcceptLifecycleFailureKind::Other};
    bool retryable{};

    bool operator==(const DashboardAcceptLifecycleFailure&) const = default;
};

// One-shot return authority for the exact successful AcceptEx slot. Keeping
// acceptance paused until this token returns prevents overload responses from
// recursively creating more accepted work than the external admission owner
// can bound. A live token is an ownership obligation: dropping it fails fast.
class DashboardAcceptResumeToken final {
public:
    DashboardAcceptResumeToken(const DashboardAcceptResumeToken&) = delete;
    DashboardAcceptResumeToken& operator=(
        const DashboardAcceptResumeToken&) = delete;
    DashboardAcceptResumeToken(DashboardAcceptResumeToken&& other) noexcept;
    DashboardAcceptResumeToken& operator=(
        DashboardAcceptResumeToken&& other) noexcept;
    ~DashboardAcceptResumeToken() noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    friend class DashboardAcceptSlotSet;

    DashboardAcceptResumeToken(
        std::shared_ptr<const DashboardAcceptSlotSetIdentity> identity,
        std::size_t slotIndex,
        std::uint64_t sequence) noexcept;

    void invalidate() noexcept;

    std::shared_ptr<const DashboardAcceptSlotSetIdentity> identity_;
    std::size_t slotIndex_{};
    std::uint64_t sequence_{};
    bool valid_{};
};

// One closed completion result. A successful open-generation completion owns
// both the accepted socket and its exact resume obligation; failed completions
// retain the typed accept, automatic-retry, and cancellation diagnostics that
// affected the listener generation.
class DashboardAcceptReapResult final {
public:
    DashboardAcceptReapResult(const DashboardAcceptReapResult&) = delete;
    DashboardAcceptReapResult& operator=(
        const DashboardAcceptReapResult&) = delete;
    DashboardAcceptReapResult(DashboardAcceptReapResult&&) noexcept = default;
    DashboardAcceptReapResult& operator=(
        DashboardAcceptReapResult&&) noexcept = default;
    ~DashboardAcceptReapResult() noexcept = default;

    [[nodiscard]] DashboardAcceptReapDisposition disposition() const noexcept
    {
        return disposition_;
    }

    [[nodiscard]] bool hasAcceptedConnection() const noexcept
    {
        return acceptedConnection_.has_value();
    }

    [[nodiscard]] std::optional<DashboardAcceptedConnection>
    takeAcceptedConnection() noexcept;

    [[nodiscard]] bool hasResumeToken() const noexcept
    {
        return resumeToken_.has_value();
    }

    [[nodiscard]] std::optional<DashboardAcceptResumeToken>
    takeResumeToken() noexcept;

    [[nodiscard]] const Domain::Error* acceptFailure() const noexcept
    {
        return acceptFailure_.has_value()
            ? std::addressof(*acceptFailure_)
            : nullptr;
    }

    [[nodiscard]] const Domain::Error* reissueFailure() const noexcept
    {
        return reissueFailure_.has_value()
            ? std::addressof(*reissueFailure_)
            : nullptr;
    }

    [[nodiscard]] const DashboardAcceptLifecycleFailure*
    cancellationFailure() const noexcept
    {
        return cancellationFailure_.has_value()
            ? std::addressof(*cancellationFailure_)
            : nullptr;
    }

    // True only when this exact slot had a successful CancelIoEx request,
    // ERROR_NOT_FOUND completion-race acknowledgement, or was explicitly
    // covered by the listener-handle close before its completion was reaped.
    [[nodiscard]] bool cancellationRequestedForSlot() const noexcept
    {
        return cancellationRequestedForSlot_;
    }

private:
    friend class DashboardAcceptSlotSet;

    DashboardAcceptReapResult(
        DashboardAcceptReapDisposition disposition,
        std::optional<DashboardAcceptedConnection> acceptedConnection,
        std::optional<DashboardAcceptResumeToken> resumeToken,
        std::optional<Domain::Error> acceptFailure,
        std::optional<Domain::Error> reissueFailure,
        std::optional<DashboardAcceptLifecycleFailure>
            cancellationFailure,
        bool cancellationRequestedForSlot) noexcept;

    DashboardAcceptReapDisposition disposition_{
        DashboardAcceptReapDisposition::FailureDrained};
    std::optional<DashboardAcceptedConnection> acceptedConnection_;
    std::optional<DashboardAcceptResumeToken> resumeToken_;
    std::optional<Domain::Error> acceptFailure_;
    std::optional<Domain::Error> reissueFailure_;
    std::optional<DashboardAcceptLifecycleFailure> cancellationFailure_;
    bool cancellationRequestedForSlot_{};
};

class DashboardAcceptSlotSetSnapshot final {
public:
    [[nodiscard]] std::size_t issuedCount() const noexcept
    {
        return issuedCount_;
    }

    [[nodiscard]] std::size_t idleCount() const noexcept
    {
        return idleCount_;
    }

    [[nodiscard]] std::size_t cancellationRequestedCount() const noexcept
    {
        return cancellationRequestedCount_;
    }

    [[nodiscard]] std::size_t drainedCount() const noexcept
    {
        return drainedCount_;
    }

    // Open-generation successful accepts whose callers still own the exact
    // one-shot resume obligation.
    [[nodiscard]] std::size_t pausedCount() const noexcept
    {
        return pausedCount_;
    }

    // Closed-generation obligations awaiting exact token return; no native
    // accept remains issued for these slots.
    [[nodiscard]] std::size_t awaitingReturnCount() const noexcept
    {
        return awaitingReturnCount_;
    }

    // Includes both open-generation paused obligations and closed-generation
    // obligations that must return before the set is fully drained.
    [[nodiscard]] std::size_t outstandingResumeTokenCount() const noexcept
    {
        return pausedCount_ + awaitingReturnCount_;
    }

    [[nodiscard]] bool startAttempted() const noexcept
    {
        return startAttempted_;
    }

    [[nodiscard]] bool listenerAssociated() const noexcept
    {
        return listenerAssociated_;
    }

    [[nodiscard]] bool listenerForceClosed() const noexcept
    {
        return listenerForceClosed_;
    }

    [[nodiscard]] bool admissionOpen() const noexcept
    {
        return admissionOpen_;
    }

    [[nodiscard]] bool fullyDrained() const noexcept;

    [[nodiscard]] const DashboardAcceptLifecycleFailure*
    lifecycleFailure() const noexcept
    {
        return lifecycleFailure_.has_value()
            ? std::addressof(*lifecycleFailure_)
            : nullptr;
    }

private:
    friend class DashboardAcceptSlotSet;

    DashboardAcceptSlotSetSnapshot(
        std::size_t issuedCount,
        std::size_t idleCount,
        std::size_t pausedCount,
        std::size_t awaitingReturnCount,
        std::size_t cancellationRequestedCount,
        std::size_t drainedCount,
        bool startAttempted,
        bool listenerAssociated,
        bool listenerForceClosed,
        bool admissionOpen,
        std::optional<DashboardAcceptLifecycleFailure>
            lifecycleFailure) noexcept;

    std::size_t issuedCount_{};
    std::size_t idleCount_{};
    std::size_t pausedCount_{};
    std::size_t awaitingReturnCount_{};
    std::size_t cancellationRequestedCount_{};
    std::size_t drainedCount_{};
    bool startAttempted_{};
    bool listenerAssociated_{};
    bool listenerForceClosed_{};
    bool admissionOpen_{};
    std::optional<DashboardAcceptLifecycleFailure> lifecycleFailure_;
};

// Owns the four heap-stable AcceptEx operations for exactly one immutable
// listener/provider generation. Extension functions are always discovered
// from the owned listener; callers cannot inject a pre-discovered extension
// owner from another provider. DashboardWinsockRuntime and the process IOCP
// kernel are composition-owned and must outlive this set and its drain.
class DashboardAcceptSlotSet final {
public:
    static constexpr std::size_t SlotCount = 4U;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardAcceptSlotSet>>
    create(
        DashboardWinsockRuntime& runtime,
        DashboardListeningSocket listener,
        DashboardIoCompletionKey completionKey) noexcept;

    // Test seam: native APIs may be injected, but extension ownership still
    // originates from discovery against the exact owned listener socket.
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardAcceptSlotSet>>
    create(
        DashboardWinsockRuntime& runtime,
        DashboardListeningSocket listener,
        DashboardIoCompletionKey completionKey,
        std::shared_ptr<IDashboardWinsockExtensionApi> extensionApi,
        std::shared_ptr<IDashboardAcceptSlotApi> slotApi) noexcept;

    DashboardAcceptSlotSet(const DashboardAcceptSlotSet&) = delete;
    DashboardAcceptSlotSet& operator=(const DashboardAcceptSlotSet&) = delete;
    DashboardAcceptSlotSet(DashboardAcceptSlotSet&&) = delete;
    DashboardAcceptSlotSet& operator=(DashboardAcceptSlotSet&&) = delete;
    ~DashboardAcceptSlotSet() noexcept;

    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept
    {
        return completionKey_;
    }

    // Associates the listener before issuing any AcceptEx. A failure after one
    // or more issues closes admission and requests cancellation for every
    // issued slot. The caller must retain this owner and reap those packets.
    [[nodiscard]] Domain::Result<void> start(
        DashboardIocpWorkerKernel& kernel) noexcept;

    // Idempotently closes admission. Idle slots drain immediately, issued
    // slots remain until exact completion, and paused slots remain retained
    // until their one-shot tokens return without issuing new native work.
    [[nodiscard]] std::optional<DashboardAcceptLifecycleFailure>
    closeAdmissionAndRequestCancellation() noexcept;

    // Performs one final bounded cancellation pass, invalidates the owned
    // listener handle, and records exact per-slot listener-close provenance.
    // Slot and OVERLAPPED storage remains owned until matching IOCP reaps.
    [[nodiscard]] std::optional<DashboardAcceptLifecycleFailure>
    forceCloseListenerAndRequestCancellation() noexcept;

    // Returns the one exact paused slot to native acceptance. When admission
    // closed first, returning the token drains the slot without issuing or
    // cancelling native work.
    [[nodiscard]] Domain::Result<DashboardAcceptResumeDisposition> resume(
        DashboardAcceptResumeToken&& token) noexcept;

    // nativeError is ERROR_SUCCESS for a successful packet and the exact
    // failed-I/O error otherwise. The key, pointer, and zero-byte AcceptEx
    // contract are validated before any slot state is changed.
    [[nodiscard]] Domain::Result<DashboardAcceptReapResult> reap(
        DashboardIoCompletionKey completionKey,
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept;

    [[nodiscard]] DashboardAcceptSlotSetSnapshot snapshot() const noexcept;

    // Copies the first retained full lifecycle failure for diagnostics. The
    // bounded snapshot above remains allocation-free and noexcept.
    [[nodiscard]] std::optional<Domain::Error> fullLifecycleFailure() const;

private:
    enum class SlotLifecycle : std::uint8_t {
        Idle,
        Issued,
        Paused,
        AwaitingReturnClosed,
        CancellationRequested,
        Drained,
    };

    using SlotOwners =
        std::array<std::unique_ptr<DashboardAcceptSlot>, SlotCount>;

    DashboardAcceptSlotSet(
        DashboardWinsockRuntime& runtime,
        DashboardListeningSocket listener,
        std::unique_ptr<DashboardWinsockExtensions> extensions,
        SlotOwners slots,
        std::shared_ptr<const DashboardAcceptSlotSetIdentity> identity,
        DashboardIoCompletionKey completionKey) noexcept;

    [[nodiscard]] Domain::Result<DashboardAcceptIssueDisposition> issueLocked(
        std::size_t index) noexcept;
    [[nodiscard]] std::optional<DashboardAcceptLifecycleFailure>
    closeAdmissionAndRequestCancellationLocked(
        bool retryFailedCancellation = false) noexcept;
    [[nodiscard]] DashboardAcceptLifecycleFailure
    retainLifecycleFailureLocked(Domain::Error error) noexcept;
    [[nodiscard]] Domain::Result<std::size_t> findIssuedSlotLocked(
        OVERLAPPED* operation) const noexcept;
    [[nodiscard]] DashboardAcceptReapResult finishFailureLocked(
        std::size_t index,
        Domain::Error acceptFailure,
        bool cancellationRequestedForSlot) noexcept;
    [[nodiscard]] DashboardAcceptSlotSetSnapshot snapshotLocked()
        const noexcept;

    DashboardWinsockRuntime* runtime_{};
    DashboardListeningSocket listener_;
    std::unique_ptr<DashboardWinsockExtensions> extensions_;
    SlotOwners slots_;
    std::shared_ptr<const DashboardAcceptSlotSetIdentity> identity_;
    std::array<SlotLifecycle, SlotCount> lifecycles_{};
    std::array<std::uint8_t, SlotCount> cancellationAttemptCounts_{};
    std::array<std::uint64_t, SlotCount> resumeSequences_{};
    DashboardIoCompletionKey completionKey_{0U};
    bool startAttempted_{};
    bool listenerAssociated_{};
    bool listenerForceClosed_{};
    bool admissionOpen_{};
    bool terminalClosed_{};
    std::optional<Domain::Error> firstLifecycleFailure_;
    std::optional<DashboardAcceptLifecycleFailure>
        firstLifecycleFailureSnapshot_;
    mutable std::mutex mutex_;
};

static_assert(
    DashboardAcceptSlotSet::SlotCount ==
    DashboardIocpWorkerKernel::WorkerCount);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
