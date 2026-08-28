#pragma once

#include "DashboardAcceptSlotSet.h"
#include "DashboardAdmissionController.h"
#include "DashboardConnectionRegistry.h"
#include "DashboardConnectionRuntimeServices.h"
#include "DashboardConnectionState.h"
#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Closed ownership transferred only when the global short-connection
// admission is unavailable. Holding this work keeps the exact AcceptEx slot
// paused, which bounds concurrent overload responders to four per listener
// generation and eight process-wide while active and retiring generations
// overlap. Completing or simply destroying it closes the accepted socket
// before returning the exact one-shot token to its listener generation.
class DashboardAdmissionOverloadWork final {
public:
    DashboardAdmissionOverloadWork(
        const DashboardAdmissionOverloadWork&) = delete;
    DashboardAdmissionOverloadWork& operator=(
        const DashboardAdmissionOverloadWork&) = delete;
    DashboardAdmissionOverloadWork(
        DashboardAdmissionOverloadWork&& other) noexcept;
    DashboardAdmissionOverloadWork& operator=(
        DashboardAdmissionOverloadWork&&) = delete;
    ~DashboardAdmissionOverloadWork() noexcept;

    [[nodiscard]] SOCKET borrowedNativeSocket() const noexcept;

    // Preserves the exact one-shot listener resume token while invalidating
    // the accepted client handle so a failed CancelIoEx/shutdown pair cannot
    // leave the kernel operation attached to an open socket indefinitely.
    void closeNativeSocket() noexcept;

    [[nodiscard]] const Domain::Error& admissionFailure() const noexcept
    {
        return admissionFailure_;
    }

    [[nodiscard]] std::uint64_t originGenerationId() const noexcept
    {
        return originGenerationId_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint admittedAt() const noexcept
    {
        return admittedAt_;
    }

    [[nodiscard]] bool ownsCompletionObligation() const noexcept
    {
        return acceptSlots_ != nullptr && resumeToken_.has_value() &&
            resumeToken_->valid();
    }

    // Generation shutdown closes the exact originating accept set before it
    // cancels this responder's send. The work keeps its token until the send
    // completion is reaped, so complete() then returns it without reissuing.
    // Calls on one work item are serialized by its responder owner.
    [[nodiscard]] std::optional<DashboardAcceptLifecycleFailure>
    closeOriginAdmission() noexcept;

    // The overload responder calls this only after its fixed response send is
    // fully reaped or deliberately abandoned. Destruction provides the same
    // close-then-resume guarantee as a final safety net.
    [[nodiscard]] Domain::Result<DashboardAcceptResumeDisposition>
    complete() noexcept;

private:
    friend class DashboardAcceptedConnectionHandoff;

    DashboardAdmissionOverloadWork(
        std::uint64_t originGenerationId,
        Domain::MonotonicTimePoint admittedAt,
        DashboardAcceptSlotSet& acceptSlots,
        DashboardAcceptedConnection acceptedConnection,
        DashboardAcceptResumeToken resumeToken,
        Domain::Error admissionFailure) noexcept;

    void completeWithoutResult() noexcept;

    const std::uint64_t originGenerationId_{};
    const Domain::MonotonicTimePoint admittedAt_{};
    DashboardAcceptSlotSet* acceptSlots_{};
    std::optional<DashboardAcceptedConnection> acceptedConnection_;
    std::optional<DashboardAcceptResumeToken> resumeToken_;
    Domain::Error admissionFailure_;
};

// Separate overload transport boundary. Implementations own at most one work
// item per withheld accept token, send only the fixed pre-encoded 503, and may
// never transfer the work item outside the bounded eight-entry process owner.
// Calls can arrive concurrently from the four IOCP workers.
class IDashboardAdmissionOverloadResponder {
public:
    virtual ~IDashboardAdmissionOverloadResponder() noexcept = default;

    virtual void respond(DashboardAdmissionOverloadWork work) noexcept = 0;

    // Cancels only fixed sends originating from one retiring generation.
    // Native operation storage remains owned until exact IOCP reap.
    [[nodiscard]] virtual std::size_t cancelGeneration(
        std::uint64_t generationId) noexcept = 0;

    // Delivers fixed-capacity terminal notifications only after a listener
    // transition guard has been released. respond() and cancelGeneration()
    // may stage notifications but never invoke the observer synchronously.
    virtual void drainTerminalGenerationNotifications() noexcept = 0;
};

// Process-wide observer edge emitted only after an overload work item has
// returned its exact paused accept token. Implementations must not poll and
// must not retain generation-owned resources.
class IDashboardAdmissionOverloadDrainObserver {
public:
    virtual ~IDashboardAdmissionOverloadDrainObserver() noexcept = default;

    // Nonfatal process-shutdown edge. The fixed overload owner publishes this
    // before cancelling live work so the coordinator closes listener admission
    // first even when registry auxiliary shutdown reaches this owner first.
    virtual void overloadOwnerBeganShutdown() noexcept = 0;

    // Generation-independent fatal edge for malformed fixed-key completion or
    // IOCP failure even when the overload pool owns no generation work.
    virtual void overloadOwnerBecameTerminal() noexcept = 0;

    virtual void overloadGenerationMayHaveDrained(
        std::uint64_t generationId) noexcept = 0;

    // Allocation-free latch delivered before terminal work can return its
    // last generation-owned token. The coordinator uses it only to block
    // collection and rebind; it must not re-enter the listener transition.
    virtual void overloadGenerationTerminalPending(
        std::uint64_t generationId) noexcept = 0;

    // Provisional collection hold surrounding synchronous token return. It
    // closes the gap where resume can expose zero accept ownership before its
    // failure result is known and promoted to a terminal pending edge.
    virtual void overloadGenerationCompletionPending(
        std::uint64_t generationId) noexcept = 0;
    virtual void overloadGenerationCompletionSettled(
        std::uint64_t generationId) noexcept = 0;

    // Emitted when the process-wide fixed overload owner becomes terminal
    // while it owns work from this generation. The coordinator must shut down
    // the exact generation so listener AcceptEx and connection ownership use
    // their own bounded cancellation paths.
    virtual void overloadGenerationBecameTerminal(
        std::uint64_t generationId) noexcept = 0;
};

// Factory boundary used after exact short admission and identity allocation.
// A successful target owns the accepted socket and admission lease. A failed
// call consumes and releases both inputs before it returns.
class IDashboardAcceptedConnectionOwnerFactory {
public:
    virtual ~IDashboardAcceptedConnectionOwnerFactory() noexcept = default;

    // Opaque pointer identity only. Callers compare it for exact immutable
    // application-policy composition and never invoke through this value.
    [[nodiscard]] virtual const void* applicationPolicyIdentity()
        const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<
        std::shared_ptr<IDashboardConnectionDispatchTarget>>
    createOwner(
        std::uint64_t generationId,
        DashboardConnectionRuntimeIdentity identity,
        Domain::MonotonicTimePoint admittedAt,
        DashboardAdmissionController::Lease admissionLease,
        DashboardAcceptedConnection acceptedConnection) noexcept = 0;
};

// Production composition of the accepted socket, synthetic event bridge, and
// complete connection state. All borrowed process services outlive the
// factory and every owner it creates.
class DashboardAcceptedConnectionOwnerFactory final
    : public IDashboardAcceptedConnectionOwnerFactory {
public:
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardAcceptedConnectionOwnerFactory>>
    create(
        DashboardIocpWorkerKernel& kernel,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        DashboardConnectionResponseCatalog& responseCatalog) noexcept;

    ~DashboardAcceptedConnectionOwnerFactory() noexcept override = default;

    DashboardAcceptedConnectionOwnerFactory(
        const DashboardAcceptedConnectionOwnerFactory&) = delete;
    DashboardAcceptedConnectionOwnerFactory& operator=(
        const DashboardAcceptedConnectionOwnerFactory&) = delete;
    DashboardAcceptedConnectionOwnerFactory(
    DashboardAcceptedConnectionOwnerFactory&&) = delete;
    DashboardAcceptedConnectionOwnerFactory& operator=(
        DashboardAcceptedConnectionOwnerFactory&&) = delete;

    [[nodiscard]] const void* applicationPolicyIdentity()
        const noexcept override
    {
        return application_.get();
    }

    [[nodiscard]] Domain::Result<
        std::shared_ptr<IDashboardConnectionDispatchTarget>>
    createOwner(
        std::uint64_t generationId,
        DashboardConnectionRuntimeIdentity identity,
        Domain::MonotonicTimePoint admittedAt,
        DashboardAdmissionController::Lease admissionLease,
        DashboardAcceptedConnection acceptedConnection) noexcept override;

private:
    DashboardAcceptedConnectionOwnerFactory(
        DashboardIocpWorkerKernel& kernel,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        DashboardConnectionResponseCatalog& responseCatalog) noexcept;

    DashboardIocpWorkerKernel* kernel_{};
    WindowsDashboardDeadlineScheduler* deadlineScheduler_{};
    WindowsDashboardHandlerExecutor* handlerExecutor_{};
    DashboardConnectionRuntimeServices* runtimeServices_{};
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application_;
    DashboardConnectionResponseCatalog* responseCatalog_{};
};

// Narrow registration seam. DashboardConnectionRegistry remains the only
// production owner that inserts before start and retains failed-start targets
// until every native, event, and deadline obligation drains. Every failure
// path must request target shutdown or deliberately retain it for drain before
// returning; calls can arrive concurrently.
class IDashboardConnectionRegistrar {
public:
    virtual ~IDashboardConnectionRegistrar() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> registerConnection(
        std::shared_ptr<IDashboardConnectionDispatchTarget> target)
        noexcept = 0;
};

class DashboardConnectionRegistryRegistrar final
    : public IDashboardConnectionRegistrar {
public:
    explicit DashboardConnectionRegistryRegistrar(
        DashboardConnectionRegistry& registry) noexcept;

    [[nodiscard]] Domain::Result<void> registerConnection(
        std::shared_ptr<IDashboardConnectionDispatchTarget> target)
        noexcept override;

private:
    DashboardConnectionRegistry* registry_{};
};

enum class DashboardAcceptedConnectionHandoffDisposition : std::uint8_t {
    Registered,
    OverloadResponseTransferred,
};

// Generation-owned stateless handoff. It consumes exactly one successful
// AcceptedAndPaused reap result, obtains the process-global short lease,
// allocates a nonreusing identity, constructs complete connection ownership,
// and delegates insertion-before-start to the registry. The exact accept slot
// is resumed only after registration returns. Every earlier failure closes or
// transfers the accepted socket, releases or transfers admission, and returns
// the exact token before reporting the error.
class DashboardAcceptedConnectionHandoff final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<
        DashboardAcceptedConnectionHandoff>>
    create(
        std::uint64_t generationId,
        DashboardAcceptSlotSet& acceptSlots,
        DashboardAdmissionController& admissionController,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardAcceptedConnectionOwnerFactory>
            ownerFactory,
        std::shared_ptr<IDashboardConnectionRegistrar> registrar,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder) noexcept;

    ~DashboardAcceptedConnectionHandoff() noexcept = default;

    DashboardAcceptedConnectionHandoff(
        const DashboardAcceptedConnectionHandoff&) = delete;
    DashboardAcceptedConnectionHandoff& operator=(
        const DashboardAcceptedConnectionHandoff&) = delete;
    DashboardAcceptedConnectionHandoff(
    DashboardAcceptedConnectionHandoff&&) = delete;
    DashboardAcceptedConnectionHandoff& operator=(
        DashboardAcceptedConnectionHandoff&&) = delete;

    [[nodiscard]] std::uint64_t generationId() const noexcept
    {
        return generationId_;
    }

    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept
    {
        return acceptSlots_->completionKey();
    }

    [[nodiscard]] const DashboardAcceptSlotSet* acceptSlotSetIdentity()
        const noexcept
    {
        return acceptSlots_;
    }

    [[nodiscard]] const void* applicationPolicyIdentity() const noexcept
    {
        return applicationPolicyIdentity_;
    }

    [[nodiscard]] Domain::Result<
        DashboardAcceptedConnectionHandoffDisposition>
    consume(DashboardAcceptReapResult accepted) noexcept;

private:
    DashboardAcceptedConnectionHandoff(
        std::uint64_t generationId,
        DashboardAcceptSlotSet& acceptSlots,
        DashboardAdmissionController& admissionController,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardAcceptedConnectionOwnerFactory>
            ownerFactory,
        const void* applicationPolicyIdentity,
        std::shared_ptr<IDashboardConnectionRegistrar> registrar,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder) noexcept;

    [[nodiscard]] Domain::Result<
        DashboardAcceptedConnectionHandoffDisposition>
    failAfterLocalOwnership(
        Domain::Error error,
        std::optional<DashboardAcceptedConnection>& acceptedConnection,
        std::optional<DashboardAcceptResumeToken>& resumeToken,
        DashboardAdmissionController::Lease* admissionLease) noexcept;

    const std::uint64_t generationId_{};
    DashboardAcceptSlotSet* acceptSlots_{};
    DashboardAdmissionController* admissionController_{};
    DashboardConnectionRuntimeServices* runtimeServices_{};
    std::shared_ptr<IDashboardAcceptedConnectionOwnerFactory> ownerFactory_;
    const void* const applicationPolicyIdentity_{};
    std::shared_ptr<IDashboardConnectionRegistrar> registrar_;
    std::shared_ptr<IDashboardAdmissionOverloadResponder> overloadResponder_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
