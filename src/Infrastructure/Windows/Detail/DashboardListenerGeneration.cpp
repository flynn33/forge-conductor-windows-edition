#include "DashboardListenerGeneration.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

class DashboardListenerGenerationProcessFailFast final
    : public IDashboardListenerGenerationFailFast {
public:
    void failFast() noexcept override { std::terminate(); }
};

class DashboardListenerGenerationDeadlineScheduler final
    : public IDashboardListenerGenerationDeadlineScheduler {
public:
    explicit DashboardListenerGenerationDeadlineScheduler(
        WindowsDashboardDeadlineScheduler& scheduler) noexcept
        : scheduler_{std::addressof(scheduler)}
    {
    }

    [[nodiscard]] Domain::Result<WindowsDashboardDeadline> schedule(
        WindowsDashboardDeadlineRequest request) noexcept override
    {
        return scheduler_->schedule(std::move(request));
    }

    [[nodiscard]] bool cancel(
        const std::uint64_t registrationId,
        const std::uint64_t armSequence) noexcept override
    {
        return scheduler_->cancel(registrationId, armSequence);
    }

private:
    WindowsDashboardDeadlineScheduler* scheduler_{};
};

[[nodiscard]] Domain::Error generationError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidGenerationError()
{
    return generationError(
        Domain::ErrorCodes::InvalidRequest,
        "A dashboard listener generation requires nonzero exact identity and complete bounded owners.");
}

[[nodiscard]] Domain::Error internalGenerationError()
{
    return generationError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard listener generation failed safely.");
}

[[nodiscard]] Domain::Error transitionGateError()
{
    return generationError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard listener generation received authority from another transition gate.");
}

[[nodiscard]] Domain::Error acceptCancellationError(
    const DashboardAcceptLifecycleFailure failure)
{
    std::string_view code{Domain::ErrorCodes::InternalFailure};
    switch (failure.kind) {
    case DashboardAcceptLifecycleFailureKind::Cancelled:
        code = Domain::ErrorCodes::Cancelled;
        break;
    case DashboardAcceptLifecycleFailureKind::TransportClosed:
        code = Domain::ErrorCodes::TransportClosed;
        break;
    case DashboardAcceptLifecycleFailureKind::Unauthorized:
        code = Domain::ErrorCodes::Unauthorized;
        break;
    case DashboardAcceptLifecycleFailureKind::LimitExceeded:
        code = Domain::ErrorCodes::LimitExceeded;
        break;
    case DashboardAcceptLifecycleFailureKind::InternalFailure:
    case DashboardAcceptLifecycleFailureKind::Other:
        break;
    }
    return generationError(
        code,
        "The dashboard listener could not cancel every exact AcceptEx operation.",
        failure.retryable);
}

[[nodiscard]] Domain::Result<void> acceptCancellationResult(
    DashboardAcceptSlotSet& acceptSlots,
    const std::optional<DashboardAcceptLifecycleFailure> failure) noexcept
{
    if (!failure.has_value()) {
        return Domain::Result<void>::success();
    }
    try {
        auto full = acceptSlots.fullLifecycleFailure();
        if (full.has_value()) {
            return Domain::Result<void>::failure(std::move(*full));
        }
        return Domain::Result<void>::failure(
            acceptCancellationError(*failure));
    } catch (...) {
        return Domain::Result<void>::failure(internalGenerationError());
    }
}

[[nodiscard]] Domain::MonotonicTimePoint boundedDeadline(
    const Domain::MonotonicTimePoint now,
    const std::chrono::seconds lifetime) noexcept
{
    const auto duration = std::chrono::duration_cast<
        Domain::MonotonicTimePoint::duration>(lifetime);
    const auto maximum = (Domain::MonotonicTimePoint::max)();
    return now > maximum - duration ? maximum : now + duration;
}

void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

} // namespace

DashboardConnectionRegistryGenerationControl::
DashboardConnectionRegistryGenerationControl(
    DashboardConnectionRegistry& registry) noexcept
    : registry_{std::addressof(registry)}
{
}

std::size_t DashboardConnectionRegistryGenerationControl::
connectionCountForGeneration(const std::uint64_t generationId) const noexcept
{
    return registry_->connectionCountForGeneration(generationId);
}

void DashboardConnectionRegistryGenerationControl::
beginShutdownGeneration(const std::uint64_t generationId) noexcept
{
    static_cast<void>(registry_->beginShutdownGeneration(generationId));
}

Domain::Result<std::unique_ptr<DashboardListenerGenerationAcceptOwner>>
DashboardListenerGenerationAcceptOwner::create(
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<DashboardAcceptSlotSet> acceptSlots,
    std::unique_ptr<DashboardAcceptedConnectionHandoff> handoff,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication>
        applicationPolicy,
    DashboardIocpWorkerKernel& kernel) noexcept
{
    using CreateResult = Domain::Result<std::unique_ptr<
        DashboardListenerGenerationAcceptOwner>>;
    if (identity.registrationId == 0U ||
        identity.completionKey.value() == 0U ||
        identity.completionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue ||
        acceptSlots == nullptr || handoff == nullptr ||
        applicationPolicy == nullptr ||
        handoff->generationId() != identity.registrationId ||
        !(acceptSlots->completionKey() == identity.completionKey) ||
        !(handoff->completionKey() == identity.completionKey) ||
        handoff->acceptSlotSetIdentity() != acceptSlots.get() ||
        handoff->applicationPolicyIdentity() !=
            applicationPolicy.get()) {
        return CreateResult::failure(invalidGenerationError());
    }
    try {
        return CreateResult::success(
            std::unique_ptr<DashboardListenerGenerationAcceptOwner>{
                new DashboardListenerGenerationAcceptOwner{
                    identity,
                    std::move(acceptSlots),
                    std::move(handoff),
                    std::move(applicationPolicy),
                    kernel}});
    } catch (...) {
        return CreateResult::failure(internalGenerationError());
    }
}

DashboardListenerGenerationAcceptOwner::
DashboardListenerGenerationAcceptOwner(
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<DashboardAcceptSlotSet> acceptSlots,
    std::unique_ptr<DashboardAcceptedConnectionHandoff> handoff,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication>
        applicationPolicy,
    DashboardIocpWorkerKernel& kernel) noexcept
    : identity_{identity},
      acceptSlots_{std::move(acceptSlots)},
      handoff_{std::move(handoff)},
      applicationPolicy_{std::move(applicationPolicy)},
      kernel_{std::addressof(kernel)}
{
}

Domain::Result<void> DashboardListenerGenerationAcceptOwner::start() noexcept
{
    return acceptSlots_->start(*kernel_);
}

Domain::Result<void>
DashboardListenerGenerationAcceptOwner::closeAdmission() noexcept
{
    return acceptCancellationResult(
        *acceptSlots_,
        acceptSlots_->closeAdmissionAndRequestCancellation());
}

Domain::Result<void>
DashboardListenerGenerationAcceptOwner::forceCloseListener() noexcept
{
    return acceptCancellationResult(
        *acceptSlots_,
        acceptSlots_->forceCloseListenerAndRequestCancellation());
}

Domain::Result<void> DashboardListenerGenerationAcceptOwner::consume(
    const DashboardIoCompletionPacket packet,
    const DWORD nativeError) noexcept
{
    auto reaped = acceptSlots_->reap(
        packet.completionKey,
        packet.transferredBytes,
        packet.operation,
        nativeError);
    if (!reaped) {
        return Domain::Result<void>::failure(std::move(reaped).error());
    }

    auto result = std::move(reaped).value();
    if (result.disposition() ==
        DashboardAcceptReapDisposition::AcceptedAndPaused) {
        auto handedOff = handoff_->consume(std::move(result));
        if (!handedOff) {
            return Domain::Result<void>::failure(
                std::move(handedOff).error());
        }
        return Domain::Result<void>::success();
    }

    if (result.disposition() ==
        DashboardAcceptReapDisposition::FailureDrained) {
        try {
            const auto* const acceptFailure = result.acceptFailure();
            if (result.cancellationFailure() != nullptr) {
                return acceptCancellationResult(
                    *acceptSlots_, *result.cancellationFailure());
            }
            if (result.cancellationRequestedForSlot() &&
                result.reissueFailure() == nullptr &&
                acceptFailure != nullptr &&
                acceptFailure->code == Domain::ErrorCodes::Cancelled) {
                // ERROR_OPERATION_ABORTED is the expected exact completion
                // only for this specifically cancelled slot. It drains
                // native ownership without shortening connection grace.
                return Domain::Result<void>::success();
            }
            if (acceptFailure != nullptr &&
                acceptFailure->code == Domain::ErrorCodes::Cancelled) {
                return Domain::Result<void>::failure(generationError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A dashboard AcceptEx operation aborted without exact per-slot cancellation provenance."));
            }
            if (result.reissueFailure() != nullptr) {
                return Domain::Result<void>::failure(
                    Domain::Error{*result.reissueFailure()});
            }
            if (acceptFailure != nullptr) {
                return Domain::Result<void>::failure(
                    Domain::Error{*acceptFailure});
            }
            return Domain::Result<void>::failure(generationError(
                Domain::ErrorCodes::TransportClosed,
                "The dashboard listener generation stopped accepting connections."));
        } catch (...) {
            return Domain::Result<void>::failure(
                internalGenerationError());
        }
    }

    // AcceptedAndDrained owns and closes the accepted socket when result
    // leaves this scope. FailureReissued has already restored its exact slot.
    return Domain::Result<void>::success();
}

bool DashboardListenerGenerationAcceptOwner::fullyDrained() const noexcept
{
    return acceptSlots_->snapshot().fullyDrained();
}

DashboardListenerGenerationSnapshot::DashboardListenerGenerationSnapshot(
    const DashboardListenerGenerationLifecycle lifecycle,
    const std::uint64_t registrationId,
    const DashboardIoCompletionKey completionKey,
    std::optional<WindowsDashboardDeadline> retirementDeadline,
    std::optional<WindowsDashboardDeadline> cancellationReapDeadline,
    const std::uint64_t completionCount,
    const std::uint64_t staleDeadlineCount,
    const bool retirementCancellationRequested,
    const bool listenerForceCloseRequested,
    const std::uint64_t failFastCount,
    const bool hasFailure) noexcept
    : lifecycle_{lifecycle},
      registrationId_{registrationId},
      completionKey_{completionKey},
      retirementDeadline_{std::move(retirementDeadline)},
      cancellationReapDeadline_{std::move(cancellationReapDeadline)},
      completionCount_{completionCount},
      staleDeadlineCount_{staleDeadlineCount},
      retirementCancellationRequested_{
          retirementCancellationRequested},
      listenerForceCloseRequested_{listenerForceCloseRequested},
      failFastCount_{failFastCount},
      hasFailure_{hasFailure}
{
}

Domain::Result<std::shared_ptr<DashboardListenerGeneration>>
DashboardListenerGeneration::create(
    const DashboardConnectionRuntimeIdentity identity,
    DashboardListenerCompletionKeyLease completionKeyLease,
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl,
    std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate) noexcept
{
    try {
        return createInternal(
            std::optional<DashboardListenerCompletionKeyLease>{
                std::move(completionKeyLease)},
            identity,
            std::move(acceptOwner),
            std::make_shared<
                DashboardListenerGenerationDeadlineScheduler>(
                    deadlineScheduler),
            runtimeServices,
            std::move(connectionControl),
            std::move(overloadResponder),
            std::move(transitionGate),
            std::make_shared<
                DashboardListenerGenerationProcessFailFast>());
    } catch (...) {
        return Domain::Result<std::shared_ptr<
            DashboardListenerGeneration>>::failure(
            internalGenerationError());
    }
}

Domain::Result<std::shared_ptr<DashboardListenerGeneration>>
DashboardListenerGeneration::create(
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl,
    std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate) noexcept
{
    try {
        return create(
            identity,
            std::move(acceptOwner),
            deadlineScheduler,
            runtimeServices,
            std::move(connectionControl),
            std::move(overloadResponder),
            std::move(transitionGate),
            std::make_shared<
                DashboardListenerGenerationProcessFailFast>());
    } catch (...) {
        return Domain::Result<std::shared_ptr<
            DashboardListenerGeneration>>::failure(
            internalGenerationError());
    }
}

Domain::Result<std::shared_ptr<DashboardListenerGeneration>>
DashboardListenerGeneration::create(
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl,
    std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate,
    std::shared_ptr<IDashboardListenerGenerationFailFast> failFast) noexcept
{
    try {
        return create(
            identity,
            std::move(acceptOwner),
            std::make_shared<
                DashboardListenerGenerationDeadlineScheduler>(
                    deadlineScheduler),
            runtimeServices,
            std::move(connectionControl),
            std::move(overloadResponder),
            std::move(transitionGate),
            std::move(failFast));
    } catch (...) {
        return Domain::Result<std::shared_ptr<
            DashboardListenerGeneration>>::failure(
            internalGenerationError());
    }
}

Domain::Result<std::shared_ptr<DashboardListenerGeneration>>
DashboardListenerGeneration::create(
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
    std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
        deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl,
    std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate,
    std::shared_ptr<IDashboardListenerGenerationFailFast> failFast) noexcept
{
    return createInternal(
        std::nullopt,
        identity,
        std::move(acceptOwner),
        std::move(deadlineScheduler),
        runtimeServices,
        std::move(connectionControl),
        std::move(overloadResponder),
        std::move(transitionGate),
        std::move(failFast));
}

Domain::Result<std::shared_ptr<DashboardListenerGeneration>>
DashboardListenerGeneration::createInternal(
    std::optional<DashboardListenerCompletionKeyLease> completionKeyLease,
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
    std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
        deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl,
    std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate,
    std::shared_ptr<IDashboardListenerGenerationFailFast> failFast) noexcept
{
    using CreateResult = Domain::Result<std::shared_ptr<
        DashboardListenerGeneration>>;
    const bool leaseInvalid = completionKeyLease.has_value() &&
        (!completionKeyLease->ownsSlot() ||
         completionKeyLease->completionKey() != identity.completionKey);
    if (leaseInvalid || identity.registrationId == 0U ||
        identity.completionKey.value() == 0U ||
        identity.completionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue ||
        acceptOwner == nullptr || !(acceptOwner->identity() == identity) ||
        deadlineScheduler == nullptr ||
        connectionControl == nullptr ||
        overloadResponder == nullptr ||
        transitionGate == nullptr || failFast == nullptr) {
        return CreateResult::failure(invalidGenerationError());
    }
    try {
        return CreateResult::success(
            std::shared_ptr<DashboardListenerGeneration>{
                new DashboardListenerGeneration{
                    std::move(completionKeyLease),
                    identity,
                    std::move(acceptOwner),
                    deadlineScheduler,
                    runtimeServices,
                    std::move(connectionControl),
                    std::move(overloadResponder),
                    std::move(transitionGate),
                    std::move(failFast)}});
    } catch (...) {
        return CreateResult::failure(internalGenerationError());
    }
}

DashboardListenerGeneration::DashboardListenerGeneration(
    std::optional<DashboardListenerCompletionKeyLease> completionKeyLease,
    const DashboardConnectionRuntimeIdentity identity,
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
    std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
        deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl,
    std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder,
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate,
    std::shared_ptr<IDashboardListenerGenerationFailFast> failFast) noexcept
    : completionKeyLease_{std::move(completionKeyLease)},
      identity_{identity},
      acceptOwner_{std::move(acceptOwner)},
      deadlineScheduler_{std::move(deadlineScheduler)},
      runtimeServices_{std::addressof(runtimeServices)},
      connectionControl_{std::move(connectionControl)},
      overloadResponder_{std::move(overloadResponder)},
      transitionGate_{std::move(transitionGate)},
      failFast_{std::move(failFast)}
{
}

DashboardListenerGeneration::~DashboardListenerGeneration() noexcept =
    default;

DashboardIoCompletionKey DashboardListenerGeneration::completionKey()
    const noexcept
{
    return identity_.completionKey;
}

std::uint64_t DashboardListenerGeneration::registrationId() const noexcept
{
    return identity_.registrationId;
}

Domain::Result<void> DashboardListenerGeneration::bindDrainObserver(
    std::weak_ptr<IDashboardListenerGenerationDrainObserver> observer)
    noexcept
{
    try {
        const auto pinned = observer.lock();
        if (pinned == nullptr) {
            return Domain::Result<void>::failure(generationError(
                Domain::ErrorCodes::InvalidRequest,
                "A listener generation requires a live drain observer."));
        }
        const std::scoped_lock lock{mutex_};
        if (observerBound_) {
            return Domain::Result<void>::failure(generationError(
                Domain::ErrorCodes::Conflict,
                "A listener generation drain observer is one-shot."));
        }
        observerBound_ = true;
        drainObserver_ = std::move(observer);
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalGenerationError());
    }
}

Domain::Result<void> DashboardListenerGeneration::startAdmission(
    DashboardListenerGenerationTransitionGate::Guard& transition) noexcept
{
    if (!transition.belongsTo(*transitionGate_)) {
        return Domain::Result<void>::failure(transitionGateError());
    }
    {
        const std::scoped_lock lock{mutex_};
        if (lifecycle_ != DashboardListenerGenerationLifecycle::Prepared) {
            return Domain::Result<void>::failure(generationError(
                Domain::ErrorCodes::Conflict,
                "A dashboard listener generation starts admission exactly once."));
        }
        lifecycle_ = DashboardListenerGenerationLifecycle::Admitting;
    }

    auto started = acceptOwner_->start();
    if (!started) {
        auto error = std::move(started).error();
        retainFailure(Domain::Error{error});
        {
            const std::scoped_lock lock{mutex_};
            lifecycle_ =
                DashboardListenerGenerationLifecycle::ShuttingDown;
        }
        forceCloseAcceptAndArmWatchdog(runtimeServices_->monotonicNow());
        deferTransitionWork(transition);
        return Domain::Result<void>::failure(std::move(error));
    }
    return Domain::Result<void>::success();
}

Domain::Result<void> DashboardListenerGeneration::beginRetirement(
    DashboardListenerGenerationTransitionGate::Guard& transition) noexcept
{
    if (!transition.belongsTo(*transitionGate_)) {
        return Domain::Result<void>::failure(transitionGateError());
    }
    {
        const std::scoped_lock lock{mutex_};
        if (lifecycle_ == DashboardListenerGenerationLifecycle::Retiring) {
            return Domain::Result<void>::success();
        }
        if (lifecycle_ != DashboardListenerGenerationLifecycle::Admitting) {
            return Domain::Result<void>::failure(generationError(
                Domain::ErrorCodes::Conflict,
                "Only an admitting dashboard listener can begin retirement."));
        }
    }

    auto scheduled = deadlineScheduler_->schedule(
        WindowsDashboardDeadlineRequest{
            identity_.registrationId,
            WindowsDashboardDeadlineKind::ListenerRetirement,
            runtimeServices_->monotonicNow() + RetirementLifetime});
    if (!scheduled) {
        auto error = std::move(scheduled).error();
        retainFailure(Domain::Error{error});
        return Domain::Result<void>::failure(std::move(error));
    }

    {
        const std::scoped_lock lock{mutex_};
        if (lifecycle_ != DashboardListenerGenerationLifecycle::Admitting ||
            retirementDeadline_.has_value()) {
            const auto unexpected = std::move(scheduled).value();
            static_cast<void>(deadlineScheduler_->cancel(
                unexpected.registrationId, unexpected.armSequence));
            return Domain::Result<void>::failure(generationError(
                Domain::ErrorCodes::Conflict,
                "The dashboard listener changed state while retirement was armed."));
        }
        retirementDeadline_.emplace(std::move(scheduled).value());
        lifecycle_ = DashboardListenerGenerationLifecycle::Retiring;
    }

    // The coordinator retains transition while invoking this method. New
    // admission is already live, and old completions cannot enter handoff
    // until this close has made their successful reaps drain-only.
    closeAcceptAdmission();
    deferTransitionWork(transition);
    return Domain::Result<void>::success();
}

Domain::Result<void>
DashboardListenerGeneration::beginGracefulShutdown(
    DashboardListenerGenerationTransitionGate::Guard& transition) noexcept
{
    if (!transition.belongsTo(*transitionGate_)) {
        return Domain::Result<void>::failure(transitionGateError());
    }

    std::optional<WindowsDashboardDeadline> cancelledRetirement;
    {
        const std::scoped_lock lock{mutex_};
        if (gracefulShutdownRequested_ || listenerForceCloseRequested_ ||
            lifecycle_ == DashboardListenerGenerationLifecycle::Fatal ||
            lifecycle_ == DashboardListenerGenerationLifecycle::Drained) {
            return Domain::Result<void>::success();
        }
        gracefulShutdownRequested_ = true;
        lifecycle_ = DashboardListenerGenerationLifecycle::ShuttingDown;
        static_cast<void>(retirementCancellation_.request_stop());
        if (retirementDeadline_.has_value()) {
            cancelledRetirement.emplace(*retirementDeadline_);
            retirementDeadline_.reset();
        }
    }

    if (cancelledRetirement.has_value()) {
        static_cast<void>(deadlineScheduler_->cancel(
            cancelledRetirement->registrationId,
            cancelledRetirement->armSequence));
    }

    // Graceful process shutdown closes only new admission. Existing
    // connections keep their independent response lifecycle until the
    // process drain owner either observes zero or requests hard escalation.
    closeAcceptAdmission();
    static_cast<void>(overloadResponder_->cancelGeneration(
        identity_.registrationId));
    deferTransitionWork(transition);
    return Domain::Result<void>::success();
}

void DashboardListenerGeneration::consume(
    const DashboardIoCompletionPacket packet,
    const DWORD nativeError) noexcept
{
    {
        auto transition = transitionGate_->enter();
        {
            const std::scoped_lock lock{mutex_};
            incrementSaturating(completionCount_);
        }

        auto consumed = acceptOwner_->consume(packet, nativeError);
        if (!consumed) {
            retainFailure(std::move(consumed).error());
            {
                const std::scoped_lock lock{mutex_};
                if (lifecycle_ !=
                        DashboardListenerGenerationLifecycle::Fatal &&
                    lifecycle_ !=
                        DashboardListenerGenerationLifecycle::Drained) {
                    lifecycle_ = DashboardListenerGenerationLifecycle::
                        ShuttingDown;
                }
            }
            forceCloseAcceptAndArmWatchdog(
                runtimeServices_->monotonicNow());
            // Handoff can report a post-registration accept-resume failure.
            // The transition gate remains held, so closing the exact
            // generation here includes both pre-existing connections and
            // that newly registered owner in one force-close snapshot.
            static_cast<void>(overloadResponder_->cancelGeneration(
                identity_.registrationId));
            connectionControl_->beginShutdownGeneration(
                identity_.registrationId);
        }
    }
    completeTransitionWork();
}

void DashboardListenerGeneration::dispatchDeadline(
    const WindowsDashboardDeadline deadline) noexcept
{
    bool exactRetirement{};
    bool exactCancellationReap{};
    {
        auto transition = transitionGate_->enter();
        {
            const std::scoped_lock lock{mutex_};
            exactRetirement = retirementDeadline_.has_value() &&
                deadline.registrationId == identity_.registrationId &&
                deadline.armSequence == retirementDeadline_->armSequence &&
                deadline.kind ==
                    WindowsDashboardDeadlineKind::ListenerRetirement;
            exactCancellationReap =
                cancellationReapDeadline_.has_value() &&
                deadline.registrationId == identity_.registrationId &&
                deadline.armSequence ==
                    cancellationReapDeadline_->armSequence &&
                deadline.kind ==
                    WindowsDashboardDeadlineKind::ListenerRetirement;
            if (!exactRetirement && !exactCancellationReap) {
                incrementSaturating(staleDeadlineCount_);
                return;
            }
            if (exactCancellationReap) {
                cancellationReapDeadline_.reset();
            } else {
                retirementDeadline_.reset();
                static_cast<void>(retirementCancellation_.request_stop());
                if (lifecycle_ !=
                    DashboardListenerGenerationLifecycle::Drained) {
                    lifecycle_ = DashboardListenerGenerationLifecycle::
                        ShuttingDown;
                }
            }
        }

        if (exactCancellationReap) {
            if (!acceptOwner_->fullyDrained()) {
                try {
                    retainFailure(generationError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Dashboard listener AcceptEx ownership exceeded its cancellation-reap deadline."));
                } catch (...) {
                    // The process boundary remains mandatory even if an
                    // allocation-backed diagnostic cannot be retained.
                }
                requestFailFast();
            }
        } else {
            forceCloseAcceptAndArmWatchdog(deadline.deadline);
            // The transition remains held from exact deadline validation
            // through the registry snapshot, so no late successful old-
            // generation handoff can escape this force-close request.
            static_cast<void>(overloadResponder_->cancelGeneration(
                identity_.registrationId));
            connectionControl_->beginShutdownGeneration(
                identity_.registrationId);
        }
    }
    completeTransitionWork();
}

void DashboardListenerGeneration::fatal(const DWORD nativeError) noexcept
{
    try {
        retainFailure(generationError(
            Domain::ErrorCodes::TransportClosed,
            "The dashboard IOCP failed while a listener generation was active. Native error " +
                std::to_string(nativeError) + "."));
    } catch (...) {
        // Fatal cancellation and exact native drain do not depend on retaining
        // an allocation-backed diagnostic string.
    }
    closeAdmissionAndConnections(
        DashboardListenerGenerationLifecycle::Fatal);
}

void DashboardListenerGeneration::beginShutdown() noexcept
{
    closeAdmissionAndConnections(
        DashboardListenerGenerationLifecycle::ShuttingDown);
}

void DashboardListenerGeneration::closeAdmissionAndConnections(
    const DashboardListenerGenerationLifecycle lifecycle) noexcept
{
    {
        auto transition = transitionGate_->enter();
        {
            const std::scoped_lock lock{mutex_};
            if (lifecycle_ == DashboardListenerGenerationLifecycle::Drained) {
                return;
            }
            if (lifecycle_ != DashboardListenerGenerationLifecycle::Fatal ||
                lifecycle == DashboardListenerGenerationLifecycle::Fatal) {
                lifecycle_ = lifecycle;
            }
            static_cast<void>(retirementCancellation_.request_stop());
        }
        forceCloseAcceptAndArmWatchdog(runtimeServices_->monotonicNow());
        static_cast<void>(overloadResponder_->cancelGeneration(
            identity_.registrationId));
        connectionControl_->beginShutdownGeneration(identity_.registrationId);
    }
    completeTransitionWork();
}

void DashboardListenerGeneration::retainFailure(Domain::Error error) noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        if (!firstFailure_.has_value()) {
            firstFailure_.emplace(std::move(error));
        }
    } catch (...) {
        // A full diagnostic copy is optional; lifecycle state remains intact.
    }
}

void DashboardListenerGeneration::closeAcceptAdmission() noexcept
{
    auto closed = acceptOwner_->closeAdmission();
    if (!closed) {
        retainFailure(std::move(closed).error());
    }
}

void DashboardListenerGeneration::forceCloseAcceptAndArmWatchdog(
    const Domain::MonotonicTimePoint watchdogBase) noexcept
{
    std::optional<WindowsDashboardDeadline> cancelledRetirement;
    bool startWatchdog{};
    {
        const std::scoped_lock lock{mutex_};
        listenerForceCloseRequested_ = true;
        if (retirementDeadline_.has_value()) {
            cancelledRetirement.emplace(*retirementDeadline_);
            retirementDeadline_.reset();
            static_cast<void>(retirementCancellation_.request_stop());
        }
        if (!cancellationReapWatchdogStarted_) {
            cancellationReapWatchdogStarted_ = true;
            startWatchdog = true;
        }
    }

    if (cancelledRetirement.has_value()) {
        static_cast<void>(deadlineScheduler_->cancel(
            cancelledRetirement->registrationId,
            cancelledRetirement->armSequence));
    }

    auto closed = acceptOwner_->forceCloseListener();
    if (!closed) {
        retainFailure(std::move(closed).error());
    }
    if (acceptOwner_->fullyDrained()) {
        const std::scoped_lock lock{mutex_};
        acceptOwnershipDrained_ = true;
        return;
    }
    if (!startWatchdog) {
        return;
    }

    auto scheduled = deadlineScheduler_->schedule(
        WindowsDashboardDeadlineRequest{
            identity_.registrationId,
            WindowsDashboardDeadlineKind::ListenerRetirement,
            boundedDeadline(
                watchdogBase,
                CancellationReapLifetime)});
    if (!scheduled) {
        auto error = std::move(scheduled).error();
        const auto acceptsNowDrained = acceptOwner_->fullyDrained();
        bool watchdogStillRequired{};
        {
            const std::scoped_lock lock{mutex_};
            if (acceptsNowDrained) {
                acceptOwnershipDrained_ = true;
            }
            watchdogStillRequired = lifecycle_ !=
                    DashboardListenerGenerationLifecycle::Drained &&
                !acceptOwnershipDrained_;
        }
        if (watchdogStillRequired) {
            retainFailure(std::move(error));
            requestFailFast();
        }
        return;
    }

    auto armed = std::move(scheduled).value();
    const auto acceptsNowDrained = acceptOwner_->fullyDrained();
    bool cancelUncommittedArm{};
    {
        const std::scoped_lock lock{mutex_};
        if (acceptsNowDrained) {
            acceptOwnershipDrained_ = true;
        }
        if (lifecycle_ == DashboardListenerGenerationLifecycle::Drained ||
            acceptOwnershipDrained_) {
            cancelUncommittedArm = true;
        } else {
            cancellationReapDeadline_.emplace(armed);
        }
    }
    if (cancelUncommittedArm) {
        static_cast<void>(deadlineScheduler_->cancel(
            armed.registrationId, armed.armSequence));
    }
}

void DashboardListenerGeneration::requestFailFast() noexcept
{
    const std::scoped_lock lock{mutex_};
    if (lifecycle_ != DashboardListenerGenerationLifecycle::Drained &&
        !acceptOwnershipDrained_ && failFastCount_ == 0U) {
        failFastPending_ = true;
    }
}

void DashboardListenerGeneration::completeTransitionWork() noexcept
{
    // respond() and cancelGeneration() only stage terminal overload edges
    // while this generation owns the shared transition gate. Deliver them
    // now, after that guard has released, so coordinated shutdown can safely
    // re-enter either active or retiring generation.
    overloadResponder_->drainTerminalGenerationNotifications();
    tryCompleteDrain();

    bool invokeBoundary{};
    {
        const std::scoped_lock lock{mutex_};
        if (lifecycle_ == DashboardListenerGenerationLifecycle::Drained ||
            acceptOwnershipDrained_) {
            failFastPending_ = false;
        } else if (failFastPending_ && failFastCount_ == 0U) {
            failFastPending_ = false;
            lifecycle_ = DashboardListenerGenerationLifecycle::Fatal;
            incrementSaturating(failFastCount_);
            invokeBoundary = true;
        }
    }
    if (invokeBoundary) {
        // The caller released both the generation mutex and transition gate
        // before reaching this process/test boundary. A returning test seam
        // may safely re-enter generation and coordinator APIs.
        failFast_->failFast();
    }
}

void DashboardListenerGeneration::deferTransitionWork(
    DashboardListenerGenerationTransitionGate::Guard& transition) noexcept
{
    auto retainedOwner = weak_from_this().lock();
    if (retainedOwner == nullptr) {
        std::terminate();
    }
    transition.deferAfterRelease(
        &DashboardListenerGeneration::completeTransitionWorkAfterRelease,
        std::static_pointer_cast<void>(std::move(retainedOwner)));
}

void DashboardListenerGeneration::completeTransitionWorkAfterRelease(
    void* const owner) noexcept
{
    if (owner == nullptr) {
        std::terminate();
    }
    static_cast<DashboardListenerGeneration*>(owner)->
        completeTransitionWork();
}

void DashboardListenerGeneration::tryCompleteDrain() noexcept
{
    const auto acceptOwnershipDrained = acceptOwner_->fullyDrained();
    if (acceptOwnershipDrained) {
        const std::scoped_lock lock{mutex_};
        acceptOwnershipDrained_ = true;
    }
    if (!acceptOwnershipDrained ||
        connectionControl_->connectionCountForGeneration(
            identity_.registrationId) != 0U) {
        return;
    }

    std::optional<WindowsDashboardDeadline> cancelledRetirement;
    std::optional<WindowsDashboardDeadline> cancelledCancellationReap;
    std::shared_ptr<IDashboardListenerGenerationDrainObserver> observer;
    {
        const std::scoped_lock lock{mutex_};
        if (lifecycle_ == DashboardListenerGenerationLifecycle::Drained &&
            drainNotificationSent_) {
            return;
        }
        if (retirementDeadline_.has_value()) {
            cancelledRetirement.emplace(*retirementDeadline_);
            retirementDeadline_.reset();
        }
        if (cancellationReapDeadline_.has_value()) {
            cancelledCancellationReap.emplace(
                *cancellationReapDeadline_);
            cancellationReapDeadline_.reset();
        }
        lifecycle_ = DashboardListenerGenerationLifecycle::Drained;
        static_cast<void>(retirementCancellation_.request_stop());
        if (!drainNotificationSent_) {
            drainNotificationSent_ = true;
            observer = drainObserver_.lock();
        }
    }

    if (cancelledRetirement.has_value()) {
        static_cast<void>(deadlineScheduler_->cancel(
            cancelledRetirement->registrationId,
            cancelledRetirement->armSequence));
    }
    if (cancelledCancellationReap.has_value()) {
        static_cast<void>(deadlineScheduler_->cancel(
            cancelledCancellationReap->registrationId,
            cancelledCancellationReap->armSequence));
    }
    notifyDrainObserver(std::move(observer));
}

void DashboardListenerGeneration::notifyDrainObserver(
    std::shared_ptr<IDashboardListenerGenerationDrainObserver> observer)
    noexcept
{
    if (observer != nullptr) {
        observer->generationMayHaveDrained(identity_.registrationId);
    }
}

bool DashboardListenerGeneration::fullyDrained() const noexcept
{
    const std::scoped_lock lock{mutex_};
    return lifecycle_ == DashboardListenerGenerationLifecycle::Drained;
}

void DashboardListenerGeneration::ownershipMayHaveDrained() noexcept
{
    tryCompleteDrain();
}

DashboardListenerGenerationSnapshot DashboardListenerGeneration::snapshot()
    const noexcept
{
    const std::scoped_lock lock{mutex_};
    return DashboardListenerGenerationSnapshot{
        lifecycle_,
        identity_.registrationId,
        identity_.completionKey,
        retirementDeadline_,
        cancellationReapDeadline_,
        completionCount_,
        staleDeadlineCount_,
        retirementCancellation_.stop_requested(),
        listenerForceCloseRequested_,
        failFastCount_,
        firstFailure_.has_value()};
}

std::optional<Domain::Error> DashboardListenerGeneration::fullFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
