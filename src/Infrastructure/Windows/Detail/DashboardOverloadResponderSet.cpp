#include "DashboardOverloadResponderSet.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

class DashboardOverloadProcessFailFast final
    : public IDashboardOverloadFailFast {
public:
    void failFast() noexcept override { std::terminate(); }
};

class DashboardOverloadKernelSocketAssociator final
    : public IDashboardOverloadSocketAssociator {
public:
    explicit DashboardOverloadKernelSocketAssociator(
        DashboardIocpWorkerKernel& kernel) noexcept
        : kernel_{std::addressof(kernel)}
    {
    }

    [[nodiscard]] Domain::Result<void> associateSocket(
        const SOCKET socket,
        const DashboardIoCompletionKey completionKey) noexcept override
    {
        return kernel_->associateSocket(socket, completionKey);
    }

private:
    DashboardIocpWorkerKernel* kernel_{};
};

[[nodiscard]] Domain::Error overloadError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard overload responder failed and its diagnostic "
            "could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error invalidOwnerError() noexcept
{
    return overloadError(
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard overload responder requires a live IOCP kernel, "
        "immutable 503 response, native socket and association owners, "
        "nonreserved completion key, and nonzero shared deadline identity.");
}

[[nodiscard]] Domain::Error integrityError(
    const std::string_view message) noexcept
{
    return overloadError(
        Domain::ErrorCodes::IntegrityFailure,
        std::string{message});
}

[[nodiscard]] Domain::Error fatalIocpError(
    const DWORD nativeError) noexcept
{
    try {
        std::string message{
            "The dashboard overload responder observed fatal IOCP error "};
        message += std::to_string(nativeError);
        message += '.';
        return overloadError(
            Domain::ErrorCodes::InternalFailure,
            std::move(message));
    } catch (...) {
        return overloadError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard overload responder observed a fatal IOCP error.");
    }
}

[[nodiscard]] Domain::Error socketError(
    const std::string_view action,
    const int nativeError) noexcept
{
    std::string_view code{Domain::ErrorCodes::InternalFailure};
    bool retryable{};
    switch (nativeError) {
    case WSA_OPERATION_ABORTED:
        code = Domain::ErrorCodes::Cancelled;
        break;
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET:
        code = Domain::ErrorCodes::TransportClosed;
        retryable = true;
        break;
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
    case WSAENOTCONN:
        code = Domain::ErrorCodes::TransportClosed;
        break;
    case WSAENOBUFS:
        code = Domain::ErrorCodes::LimitExceeded;
        retryable = true;
        break;
    case WSAEWOULDBLOCK:
        code = Domain::ErrorCodes::Conflict;
        retryable = true;
        break;
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        code = Domain::ErrorCodes::HostCapabilityUnavailable;
        retryable = true;
        break;
    default:
        break;
    }

    try {
        std::string message{action};
        message += " failed with Winsock error ";
        message += std::to_string(nativeError);
        message += '.';
        return overloadError(code, std::move(message), retryable);
    } catch (...) {
        return overloadError(
            code,
            "A dashboard overload socket operation failed.",
            retryable);
    }
}

[[nodiscard]] Domain::Error cancellationError(
    const DWORD nativeError) noexcept
{
    std::string_view code{Domain::ErrorCodes::InternalFailure};
    if (nativeError == ERROR_OPERATION_ABORTED) {
        code = Domain::ErrorCodes::Cancelled;
    } else if (nativeError == ERROR_INVALID_HANDLE) {
        code = Domain::ErrorCodes::TransportClosed;
    } else if (nativeError == ERROR_ACCESS_DENIED) {
        code = Domain::ErrorCodes::Unauthorized;
    }
    try {
        std::string message{
            "Cancel dashboard overload send failed with Win32 error "};
        message += std::to_string(nativeError);
        message += '.';
        return overloadError(code, std::move(message));
    } catch (...) {
        return overloadError(
            code,
            "A dashboard overload send could not be cancelled.");
    }
}

[[nodiscard]] Domain::Error completionNativeError(
    const DWORD nativeError) noexcept
{
    std::string_view code{Domain::ErrorCodes::InternalFailure};
    bool retryable{};
    switch (nativeError) {
    case ERROR_OPERATION_ABORTED:
        code = Domain::ErrorCodes::Cancelled;
        break;
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_ABORTED:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET:
        code = Domain::ErrorCodes::TransportClosed;
        retryable = true;
        break;
    case WSAENOTSOCK:
    case WSAESHUTDOWN:
    case WSAENOTCONN:
        code = Domain::ErrorCodes::TransportClosed;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case WSAENOBUFS:
        code = Domain::ErrorCodes::LimitExceeded;
        retryable = true;
        break;
    case WSANOTINITIALISED:
    case WSAENETDOWN:
        code = Domain::ErrorCodes::HostCapabilityUnavailable;
        retryable = true;
        break;
    default:
        break;
    }
    try {
        std::string message{
            "Reap dashboard overload send failed with native error "};
        message += std::to_string(nativeError);
        message += '.';
        return overloadError(code, std::move(message), retryable);
    } catch (...) {
        return overloadError(
            code,
            "A dashboard overload send completion failed.",
            retryable);
    }
}

[[nodiscard]] Domain::Error originCloseError(
    const DashboardAcceptLifecycleFailure failure) noexcept
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
    return overloadError(
        code,
        "The dashboard overload responder could not close one originating "
        "accept generation cleanly.",
        failure.retryable);
}

[[nodiscard]] DashboardOverloadResponderFailure classifyFailure(
    const Domain::Error& error) noexcept
{
    DashboardOverloadResponderFailureKind kind{
        DashboardOverloadResponderFailureKind::Other};
    if (error.code == Domain::ErrorCodes::Cancelled) {
        kind = DashboardOverloadResponderFailureKind::Cancelled;
    } else if (error.code == Domain::ErrorCodes::TransportClosed) {
        kind = DashboardOverloadResponderFailureKind::TransportClosed;
    } else if (error.code == Domain::ErrorCodes::Unauthorized) {
        kind = DashboardOverloadResponderFailureKind::Unauthorized;
    } else if (error.code == Domain::ErrorCodes::LimitExceeded) {
        kind = DashboardOverloadResponderFailureKind::LimitExceeded;
    } else if (error.code == Domain::ErrorCodes::IntegrityFailure) {
        kind = DashboardOverloadResponderFailureKind::IntegrityFailure;
    } else if (error.code == Domain::ErrorCodes::InternalFailure ||
               error.code ==
                   Domain::ErrorCodes::HostCapabilityUnavailable) {
        kind = DashboardOverloadResponderFailureKind::InternalFailure;
    }
    return DashboardOverloadResponderFailure{kind, error.retryable};
}

[[nodiscard]] bool isExpectedAbandonment(
    const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::TransportClosed ||
        error.code == Domain::ErrorCodes::LimitExceeded ||
        error.code == Domain::ErrorCodes::Conflict;
}

[[nodiscard]] Domain::Error normalizeIssueFailure(
    Domain::Error error) noexcept
{
    return error.code == Domain::ErrorCodes::Cancelled
        ? integrityError(
              "A dashboard overload send issue was aborted without an exact owner cancellation request.")
        : std::move(error);
}

[[nodiscard]] bool isExpectedCompletionAbandonment(
    const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::Cancelled ||
        error.code == Domain::ErrorCodes::TransportClosed ||
        error.code == Domain::ErrorCodes::LimitExceeded;
}

void incrementSaturated(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

[[nodiscard]] Domain::Error copyForRetention(
    const Domain::Error& error) noexcept
{
    return overloadError(error.code, error.message, error.retryable);
}

[[nodiscard]] Domain::MonotonicTimePoint overloadDeadlineAt(
    const Domain::MonotonicTimePoint admittedAt) noexcept
{
    const auto lifetime = std::chrono::duration_cast<
        Domain::MonotonicTimePoint::duration>(
        DashboardOverloadResponderSet::ResponseLifetime);
    const auto maximum = (Domain::MonotonicTimePoint::max)();
    return admittedAt > maximum - lifetime
        ? maximum
        : admittedAt + lifetime;
}

[[nodiscard]] Domain::MonotonicTimePoint cancellationDeadlineAt(
    const Domain::MonotonicTimePoint cancellationRequestedAt) noexcept
{
    const auto lifetime = std::chrono::duration_cast<
        Domain::MonotonicTimePoint::duration>(
        DashboardOverloadResponderSet::CancellationReapLifetime);
    const auto maximum = (Domain::MonotonicTimePoint::max)();
    return cancellationRequestedAt > maximum - lifetime
        ? maximum
        : cancellationRequestedAt + lifetime;
}

} // namespace

DashboardOverloadResponderSnapshot::DashboardOverloadResponderSnapshot(
    const std::size_t associatingCount,
    const std::size_t sendingCount,
    const std::size_t cancellationRequestedCount,
    const std::size_t settlingCount,
    const std::size_t maximumCount,
    const std::uint64_t deliveredCount,
    const std::uint64_t abandonedCount,
    const bool deadlineArmed,
    const std::uint64_t expiredCount,
    const std::uint64_t staleDeadlineCount,
    const std::uint64_t failFastCount,
    const bool shutdownRequested,
    const bool ordinaryShutdownSequenceCompleted,
    const bool terminalOwnerNotificationRequired,
    const bool terminalOwnerNotificationDelivered,
    const bool managedSourcePublicationFailed,
    const bool processDrainPublicationFailed,
    const bool shutdownTransitionInProgress,
    const bool terminalGenerationPublicationsCompleted,
    std::optional<DashboardOverloadResponderFailure>
        lifecycleFailure) noexcept
    : associatingCount_{associatingCount},
      sendingCount_{sendingCount},
      cancellationRequestedCount_{cancellationRequestedCount},
      settlingCount_{settlingCount},
      maximumCount_{maximumCount},
      deliveredCount_{deliveredCount},
      abandonedCount_{abandonedCount},
      deadlineArmed_{deadlineArmed},
      expiredCount_{expiredCount},
      staleDeadlineCount_{staleDeadlineCount},
      failFastCount_{failFastCount},
      shutdownRequested_{shutdownRequested},
      ordinaryShutdownSequenceCompleted_{
          ordinaryShutdownSequenceCompleted},
      terminalOwnerNotificationRequired_{
          terminalOwnerNotificationRequired},
      terminalOwnerNotificationDelivered_{
          terminalOwnerNotificationDelivered},
      managedSourcePublicationFailed_{managedSourcePublicationFailed},
      processDrainPublicationFailed_{processDrainPublicationFailed},
      shutdownTransitionInProgress_{shutdownTransitionInProgress},
      terminalGenerationPublicationsCompleted_{
          terminalGenerationPublicationsCompleted},
      lifecycleFailure_{std::move(lifecycleFailure)}
{
}

DashboardOverloadResponderSet::DashboardOverloadResponderSet(
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t deadlineRegistrationId,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    DashboardConnectionResponseCatalog::ImmutableBytes responseBytes,
    std::shared_ptr<IDashboardConnectionSocketApi> socketApi,
    std::shared_ptr<IDashboardOverloadFailFast> failFast,
    std::shared_ptr<IDashboardOverloadSocketAssociator>
        socketAssociator) noexcept
    : completionKey_{completionKey},
      deadlineRegistrationId_{deadlineRegistrationId},
      deadlineScheduler_{std::addressof(deadlineScheduler)},
      runtimeServices_{std::addressof(runtimeServices)},
      responseBytes_{std::move(responseBytes)},
      socketApi_{std::move(socketApi)},
      failFast_{std::move(failFast)},
      socketAssociator_{std::move(socketAssociator)}
{
}

DashboardOverloadResponderSet::~DashboardOverloadResponderSet() noexcept
{
    const std::scoped_lock deadlineLock{deadlineMutex_};
    if (currentDeadline_.has_value()) {
        static_cast<void>(deadlineScheduler_->cancel(
            deadlineRegistrationId_, currentDeadline_->armSequence));
        currentDeadline_.reset();
    }
    const std::scoped_lock lock{mutex_};
    for (const auto& slot : slots_) {
        if (slot.lifecycle != SlotLifecycle::Empty ||
            slot.work.has_value()) {
            std::terminate();
        }
    }
}

Domain::Result<std::shared_ptr<DashboardOverloadResponderSet>>
DashboardOverloadResponderSet::create(
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t deadlineRegistrationId,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    const DashboardConnectionResponseCatalog& responseCatalog) noexcept
{
    try {
        return create(
            kernel,
            completionKey,
            deadlineRegistrationId,
            deadlineScheduler,
            runtimeServices,
            responseCatalog,
            std::make_shared<DashboardConnectionSocketSystemApi>(),
            std::make_shared<DashboardOverloadProcessFailFast>());
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<DashboardOverloadResponderSet>>::failure(
            overloadError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard overload responder could not allocate its "
                "native socket API owner."));
    }
}

Domain::Result<void> DashboardOverloadResponderSet::bindDrainObserver(
    std::weak_ptr<IDashboardAdmissionOverloadDrainObserver> observer)
    noexcept
{
    try {
        if (observer.expired()) {
            return Domain::Result<void>::failure(overloadError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard overload responder requires a live drain observer."));
        }
        {
            const std::scoped_lock lock{mutex_};
            if (drainObserverEverBound_) {
                return Domain::Result<void>::failure(overloadError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard overload drain observer is already bound."));
            }
            drainObserver_ = std::move(observer);
            drainObserverEverBound_ = true;
        }
        notifyPendingTerminalGenerationLatches();
        notifyPendingTerminalGenerations();
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(overloadError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard overload drain observer could not be bound safely."));
    }
}

Domain::Result<void>
DashboardOverloadResponderSet::bindProcessDrainObserver(
    std::weak_ptr<IDashboardOverloadResponderSetDrainObserver> observer)
    noexcept
{
    try {
        auto pinned = observer.lock();
        if (pinned == nullptr) {
            return Domain::Result<void>::failure(overloadError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard overload responder requires a live process drain observer."));
        }

        const std::scoped_lock lock{mutex_};
        if (shutdownRequested_) {
            return Domain::Result<void>::failure(overloadError(
                Domain::ErrorCodes::TransportClosed,
                "The dashboard overload responder is closed to process drain observer binding."));
        }
        if (processDrainObserverEverBound_) {
            return Domain::Result<void>::failure(overloadError(
                Domain::ErrorCodes::Conflict,
                "The dashboard overload process drain observer is already bound."));
        }
        if (!drainObserverEverBound_ || drainObserver_.expired()) {
            return Domain::Result<void>::failure(overloadError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard overload process drain observer requires a live source drain observer to be bound first."));
        }
        processDrainObserver_ = std::move(observer);
        processDrainObserverEverBound_ = true;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(overloadError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard overload process drain observer could not be bound safely."));
    }
}

Domain::Result<std::shared_ptr<DashboardOverloadResponderSet>>
DashboardOverloadResponderSet::create(
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t deadlineRegistrationId,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    const DashboardConnectionResponseCatalog& responseCatalog,
    std::shared_ptr<IDashboardConnectionSocketApi> socketApi,
    std::shared_ptr<IDashboardOverloadFailFast> failFast) noexcept
{
    try {
        return create(
            completionKey,
            deadlineRegistrationId,
            deadlineScheduler,
            runtimeServices,
            responseCatalog,
            std::move(socketApi),
            std::move(failFast),
            std::make_shared<DashboardOverloadKernelSocketAssociator>(
                kernel));
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<DashboardOverloadResponderSet>>::failure(
            overloadError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard overload responder could not allocate its "
                "socket-association owner."));
    }
}

Domain::Result<std::shared_ptr<DashboardOverloadResponderSet>>
DashboardOverloadResponderSet::create(
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t deadlineRegistrationId,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    DashboardConnectionRuntimeServices& runtimeServices,
    const DashboardConnectionResponseCatalog& responseCatalog,
    std::shared_ptr<IDashboardConnectionSocketApi> socketApi,
    std::shared_ptr<IDashboardOverloadFailFast> failFast,
    std::shared_ptr<IDashboardOverloadSocketAssociator>
        socketAssociator) noexcept
{
    const auto responseBytes =
        responseCatalog.genericServiceUnavailable();
    if (completionKey.value() == 0U ||
        completionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue ||
        deadlineRegistrationId == 0U ||
        responseBytes == nullptr || responseBytes->empty() ||
        socketApi == nullptr || failFast == nullptr ||
        socketAssociator == nullptr) {
        return Domain::Result<
            std::shared_ptr<DashboardOverloadResponderSet>>::failure(
            invalidOwnerError());
    }

    try {
        return Domain::Result<
            std::shared_ptr<DashboardOverloadResponderSet>>::success(
            std::shared_ptr<DashboardOverloadResponderSet>{
                new DashboardOverloadResponderSet{
                    completionKey,
                    deadlineRegistrationId,
                    deadlineScheduler,
                    runtimeServices,
                    responseBytes,
                    std::move(socketApi),
                    std::move(failFast),
                    std::move(socketAssociator)}});
    } catch (...) {
        return Domain::Result<
            std::shared_ptr<DashboardOverloadResponderSet>>::failure(
            overloadError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard overload responder could not allocate its "
                "fixed eight-entry owner."));
    }
}

std::optional<std::size_t>
DashboardOverloadResponderSet::findEmptyLocked() const noexcept
{
    for (std::size_t index{}; index < slots_.size(); ++index) {
        if (slots_[index].lifecycle == SlotLifecycle::Empty &&
            !slots_[index].work.has_value()) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t>
DashboardOverloadResponderSet::findOperationLocked(
    OVERLAPPED* const operation) const noexcept
{
    if (operation == nullptr) {
        return std::nullopt;
    }
    for (std::size_t index{}; index < slots_.size(); ++index) {
        const auto& slot = slots_[index];
        if ((slot.lifecycle == SlotLifecycle::Sending ||
             slot.lifecycle == SlotLifecycle::CancellationRequested) &&
            slot.work.has_value() &&
            std::addressof(slot.operation) == operation) {
            return index;
        }
    }
    return std::nullopt;
}

Domain::Result<DashboardConnectionSocketIssueDisposition>
DashboardOverloadResponderSet::issueSendLocked(
    const std::size_t index) noexcept
{
    auto& slot = slots_[index];
    if (!slot.work.has_value() ||
        slot.work->borrowedNativeSocket() == INVALID_SOCKET ||
        slot.sendOffset >= responseBytes_->size()) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            integrityError(
                "A dashboard overload responder could not issue an invalid "
                "owned response suffix."));
    }

    const std::size_t remaining = responseBytes_->size() - slot.sendOffset;
    if (remaining > static_cast<std::size_t>(
            (std::numeric_limits<ULONG>::max)())) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::failure(
            integrityError(
                "The fixed dashboard overload response exceeded the native "
                "send length."));
    }

    slot.operation = {};
    slot.activeBuffer.buf = reinterpret_cast<char*>(
        const_cast<std::byte*>(responseBytes_->data() + slot.sendOffset));
    slot.activeBuffer.len = static_cast<ULONG>(remaining);
    slot.immediateTransferredBytes = 0U;
    slot.lifecycle = SlotLifecycle::Sending;
    const int status = socketApi_->send(
        slot.work->borrowedNativeSocket(),
        std::addressof(slot.activeBuffer),
        1U,
        slot.immediateTransferredBytes,
        0U,
        std::addressof(slot.operation));
    if (status == 0) {
        return Domain::Result<
            DashboardConnectionSocketIssueDisposition>::success(
            DashboardConnectionSocketIssueDisposition::
                CompletedSynchronously);
    }
    int nativeError{WSAEINVAL};
    if (status == SOCKET_ERROR) {
        nativeError = socketApi_->lastSocketError();
        if (nativeError == WSA_IO_PENDING) {
            return Domain::Result<
                DashboardConnectionSocketIssueDisposition>::success(
                DashboardConnectionSocketIssueDisposition::Pending);
        }
    }
    resetOperationLocked(index);
    return Domain::Result<
        DashboardConnectionSocketIssueDisposition>::failure(
        status == SOCKET_ERROR
            ? socketError("Issue dashboard overload send", nativeError)
            : integrityError(
                  "Issue dashboard overload send returned an undefined "
                  "native status."));
}

void DashboardOverloadResponderSet::resetOperationLocked(
    const std::size_t index) noexcept
{
    auto& slot = slots_[index];
    slot.operation = {};
    slot.activeBuffer = {};
    slot.immediateTransferredBytes = 0U;
}

DashboardOverloadResponderSet::DetachedWork
DashboardOverloadResponderSet::detachLocked(
    const std::size_t index) noexcept
{
    auto& slot = slots_[index];
    const bool tracksSettlement = slot.work.has_value();
    if (tracksSettlement) {
        ++settlingWorkCount_;
    }
    DetachedWork detached{std::move(slot.work), tracksSettlement};
    slot.work.reset();
    resetOperationLocked(index);
    slot.sendOffset = 0U;
    slot.deadlineAt = {};
    slot.cancellationDeadlineAt = {};
    slot.socketShutdownRequested = false;
    slot.cancellationRequested = false;
    slot.closeOriginAdmissionRequested = false;
    slot.nativeCancellationFailure = false;
    slot.lifecycle = SlotLifecycle::Empty;
    return detached;
}

bool DashboardOverloadResponderSet::requestCancellationLocked(
    Slot& slot,
    const bool closeOriginAdmission) noexcept
{
    if (!slot.work.has_value()) {
        return true;
    }
    if (!slot.cancellationRequested) {
        slot.cancellationDeadlineAt = cancellationDeadlineAt(
            runtimeServices_->monotonicNow());
    }
    slot.cancellationRequested = true;
    if (closeOriginAdmission &&
        !slot.closeOriginAdmissionRequested) {
        slot.closeOriginAdmissionRequested = true;
        const auto originFailure = slot.work->closeOriginAdmission();
        if (originFailure.has_value()) {
            retainFailureLocked(originCloseError(*originFailure));
        }
    }

    bool nativeCancellationHealthy{true};
    if (slot.lifecycle == SlotLifecycle::Sending) {
        if (socketApi_->cancel(
                slot.work->borrowedNativeSocket(),
                std::addressof(slot.operation)) != FALSE) {
            slot.lifecycle = SlotLifecycle::CancellationRequested;
        } else {
            const DWORD nativeError = socketApi_->lastSystemError();
            if (nativeError == ERROR_NOT_FOUND) {
                slot.lifecycle = SlotLifecycle::CancellationRequested;
            } else {
                retainFailureLocked(cancellationError(nativeError));
                slot.lifecycle = SlotLifecycle::CancellationRequested;
                nativeCancellationHealthy = false;
            }
        }
    }

    if (!slot.socketShutdownRequested &&
        slot.work->borrowedNativeSocket() != INVALID_SOCKET) {
        if (socketApi_->shutdownSocket(
                slot.work->borrowedNativeSocket(), SD_BOTH) == 0) {
            slot.socketShutdownRequested = true;
        } else {
            const int nativeError = socketApi_->lastSocketError();
            if (nativeError == WSAENOTCONN ||
                nativeError == WSAESHUTDOWN) {
                slot.socketShutdownRequested = true;
            } else {
                retainFailureLocked(socketError(
                    "Shut down dashboard overload socket", nativeError));
                nativeCancellationHealthy = false;
            }
        }
    }

    if (!nativeCancellationHealthy) {
        slot.nativeCancellationFailure = true;
        terminalShutdownRequested_ = true;
        rememberTerminalGenerationLocked(
            slot.work->originGenerationId());
        if (!slot.closeOriginAdmissionRequested) {
            slot.closeOriginAdmissionRequested = true;
            const auto originFailure = slot.work->closeOriginAdmission();
            if (originFailure.has_value()) {
                retainFailureLocked(originCloseError(*originFailure));
            }
        }
        slot.work->closeNativeSocket();
        slot.socketShutdownRequested = true;
    }
    return nativeCancellationHealthy;
}

void DashboardOverloadResponderSet::requestAllCancellationsLocked() noexcept
{
    for (auto& slot : slots_) {
        static_cast<void>(requestCancellationLocked(slot, true));
    }
}

std::optional<Domain::Error>
DashboardOverloadResponderSet::finishDetached(
    DetachedWork&& detached,
    const bool closeOriginAdmission) noexcept
{
    if (!detached.work.has_value()) {
        auto failure = integrityError(
            "A dashboard overload responder lost its owned handoff work.");
        settleDetachedWork(detached);
        return failure;
    }

    const auto generationId = detached.work->originGenerationId();
    notifyGenerationCompletionPending(generationId);
    std::optional<Domain::Error> firstFailure;
    if (closeOriginAdmission) {
        const auto closed = detached.work->closeOriginAdmission();
        if (closed.has_value()) {
            firstFailure.emplace(originCloseError(*closed));
            {
                const std::scoped_lock lock{mutex_};
                rememberTerminalGenerationLocked(generationId);
            }
            notifyPendingTerminalGenerationLatches();
        }
    }

    auto completed = detached.work->complete();
    if (!completed && !firstFailure.has_value()) {
        firstFailure.emplace(std::move(completed).error());
        {
            const std::scoped_lock lock{mutex_};
            rememberTerminalGenerationLocked(generationId);
        }
        notifyPendingTerminalGenerationLatches();
    }
    detached.work.reset();
    bool terminalNotificationDelivered{true};
    {
        const std::scoped_lock lock{mutex_};
        if (firstFailure.has_value()) {
            rememberTerminalGenerationLocked(generationId);
        }
        terminalNotificationDelivered =
            terminalGenerationNotificationDeliveredLocked(generationId);
    }
    notifyGenerationCompletionSettled(generationId);
    if (terminalNotificationDelivered) {
        notifyGenerationMayHaveDrained(generationId);
    }
    settleDetachedWork(detached);
    return firstFailure;
}

void DashboardOverloadResponderSet::settleDetachedWork(
    DetachedWork& detached) noexcept
{
    if (!detached.tracksSettlement) {
        return;
    }
    {
        const std::scoped_lock lock{mutex_};
        if (settlingWorkCount_ == 0U) {
            retainFailureLocked(integrityError(
                "The dashboard overload responder observed an unbalanced detached-work settlement."));
        } else {
            --settlingWorkCount_;
        }
        detached.tracksSettlement = false;
    }
    notifyProcessDrainObserverIfReady();
}

bool DashboardOverloadResponderSet::
terminalGenerationPublicationsCompletedLocked() const noexcept
{
    for (std::size_t index{}; index < terminalGenerationCount_; ++index) {
        if (terminalGenerationPendingNotificationLifecycles_[index] !=
                TerminalGenerationPendingNotificationLifecycle::Delivered ||
            terminalGenerationNotificationLifecycles_[index] !=
                TerminalGenerationNotificationLifecycle::Delivered) {
            return false;
        }
    }
    return true;
}

bool DashboardOverloadResponderSet::processDrainReadyLocked(
    const bool deadlineArmed) const noexcept
{
    const bool terminalOwnerNotificationRequired =
        terminalOwnerNotificationLifecycle_ !=
        TerminalOwnerNotificationLifecycle::None;
    const bool sourcePublicationCompleted =
        terminalOwnerNotificationRequired
        ? terminalOwnerNotificationLifecycle_ ==
            TerminalOwnerNotificationLifecycle::Delivered
        : ordinaryShutdownSequenceCompleted_;
    if (!sourcePublicationCompleted ||
        managedSourceDrainObserverFailureReported_ ||
        processDrainObserverPublicationFailed_ ||
        shutdownTransitionInProgress_ || settlingWorkCount_ != 0U ||
        deadlineArmed) {
        return false;
    }
    if (!terminalGenerationPublicationsCompletedLocked()) {
        return false;
    }
    for (const auto& slot : slots_) {
        if (slot.lifecycle != SlotLifecycle::Empty ||
            slot.work.has_value()) {
            return false;
        }
    }
    return true;
}

void DashboardOverloadResponderSet::notifyProcessDrainObserverIfReady()
    noexcept
{
    std::shared_ptr<IDashboardOverloadResponderSetDrainObserver> observer;
    bool failFastRequired{};
    {
        const std::scoped_lock deadlineLock{deadlineMutex_};
        const std::scoped_lock lock{mutex_};
        if (processDrainNotificationSent_ ||
            !processDrainObserverEverBound_ ||
            !processDrainReadyLocked(currentDeadline_.has_value())) {
            return;
        }
        observer = processDrainObserver_.lock();
        processDrainNotificationSent_ = true;
        if (observer == nullptr) {
            processDrainObserverPublicationFailed_ = true;
            retainFailureLocked(integrityError(
                "The dashboard overload process drain observer was not retained through the exact drain edge."));
            incrementSaturated(failFastCount_);
            failFastRequired = true;
        }
    }
    if (observer != nullptr) {
        observer->overloadRespondersMayHaveDrained();
    } else if (failFastRequired) {
        failFast_->failFast();
    }
}

bool DashboardOverloadResponderSet::
recordMissingManagedSourceDrainObserverLocked() noexcept
{
    if (!processDrainObserverEverBound_ ||
        managedSourceDrainObserverFailureReported_) {
        return false;
    }
    managedSourceDrainObserverFailureReported_ = true;
    retainFailureLocked(integrityError(
        "The managed dashboard overload source drain observer was not retained through its shutdown publication edge."));
    incrementSaturated(failFastCount_);
    return true;
}

void DashboardOverloadResponderSet::notifyGenerationMayHaveDrained(
    const std::uint64_t generationId) noexcept
{
    std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
    bool failFastRequired{};
    {
        const std::scoped_lock lock{mutex_};
        observer = drainObserver_.lock();
        if (observer == nullptr) {
            failFastRequired =
                recordMissingManagedSourceDrainObserverLocked();
        }
    }
    if (observer != nullptr) {
        observer->overloadGenerationMayHaveDrained(generationId);
    } else if (failFastRequired) {
        failFast_->failFast();
    }
}

void DashboardOverloadResponderSet::notifyGenerationCompletionPending(
    const std::uint64_t generationId) noexcept
{
    std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
    bool failFastRequired{};
    {
        const std::scoped_lock lock{mutex_};
        observer = drainObserver_.lock();
        if (observer == nullptr) {
            failFastRequired =
                recordMissingManagedSourceDrainObserverLocked();
        }
    }
    if (observer != nullptr) {
        observer->overloadGenerationCompletionPending(generationId);
    } else if (failFastRequired) {
        failFast_->failFast();
    }
}

void DashboardOverloadResponderSet::notifyGenerationCompletionSettled(
    const std::uint64_t generationId) noexcept
{
    std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
    bool failFastRequired{};
    {
        const std::scoped_lock lock{mutex_};
        observer = drainObserver_.lock();
        if (observer == nullptr) {
            failFastRequired =
                recordMissingManagedSourceDrainObserverLocked();
        }
    }
    if (observer != nullptr) {
        observer->overloadGenerationCompletionSettled(generationId);
    } else if (failFastRequired) {
        failFast_->failFast();
    }
}

void DashboardOverloadResponderSet::rememberTerminalGenerationLocked(
    const std::uint64_t generationId) noexcept
{
    rememberTerminalOwnerLocked();
    if (generationId == 0U) {
        return;
    }
    for (std::size_t index{}; index < terminalGenerationCount_; ++index) {
        if (terminalGenerationIds_[index] == generationId) {
            return;
        }
    }
    if (terminalGenerationCount_ == terminalGenerationIds_.size()) {
        retainFailureLocked(integrityError(
            "The fixed dashboard overload owner exceeded its bounded terminal-generation notification capacity."));
        return;
    }
    terminalGenerationIds_[terminalGenerationCount_] = generationId;
    terminalGenerationNotificationLifecycles_[terminalGenerationCount_] =
        TerminalGenerationNotificationLifecycle::Pending;
    terminalGenerationPendingNotificationLifecycles_[
        terminalGenerationCount_] =
        TerminalGenerationPendingNotificationLifecycle::Pending;
    ++terminalGenerationCount_;
}

void DashboardOverloadResponderSet::rememberTerminalOwnerLocked() noexcept
{
    if (terminalOwnerNotificationLifecycle_ ==
        TerminalOwnerNotificationLifecycle::None) {
        terminalOwnerNotificationLifecycle_ =
            TerminalOwnerNotificationLifecycle::Pending;
    }
}

void DashboardOverloadResponderSet::
rememberAllLiveTerminalGenerationsLocked() noexcept
{
    for (const auto& slot : slots_) {
        if (slot.work.has_value()) {
            rememberTerminalGenerationLocked(
                slot.work->originGenerationId());
        }
    }
}

bool DashboardOverloadResponderSet::
terminalGenerationNotificationDeliveredLocked(
    const std::uint64_t generationId) const noexcept
{
    for (std::size_t index{}; index < terminalGenerationCount_; ++index) {
        if (terminalGenerationIds_[index] == generationId) {
            return terminalGenerationNotificationLifecycles_[index] ==
                TerminalGenerationNotificationLifecycle::Delivered;
        }
    }
    return true;
}

void DashboardOverloadResponderSet::
notifyPendingTerminalGenerationLatches() noexcept
{
    for (;;) {
        std::optional<std::size_t> pendingIndex;
        std::uint64_t generationId{};
        std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
        bool failFastRequired{};
        {
            const std::scoped_lock lock{mutex_};
            for (std::size_t index{}; index < terminalGenerationCount_;
                 ++index) {
                if (terminalGenerationPendingNotificationLifecycles_[index] ==
                    TerminalGenerationPendingNotificationLifecycle::Pending) {
                    pendingIndex.emplace(index);
                    generationId = terminalGenerationIds_[index];
                    break;
                }
            }
            if (pendingIndex.has_value()) {
                observer = drainObserver_.lock();
                if (observer == nullptr) {
                    failFastRequired =
                        recordMissingManagedSourceDrainObserverLocked();
                } else {
                    terminalGenerationPendingNotificationLifecycles_[
                        *pendingIndex] =
                        TerminalGenerationPendingNotificationLifecycle::
                            Delivering;
                }
            }
        }
        if (!pendingIndex.has_value()) {
            return;
        }
        if (observer == nullptr) {
            if (failFastRequired) {
                failFast_->failFast();
            }
            return;
        }
        observer->overloadGenerationTerminalPending(generationId);
        {
            const std::scoped_lock lock{mutex_};
            terminalGenerationPendingNotificationLifecycles_[
                *pendingIndex] =
                TerminalGenerationPendingNotificationLifecycle::Delivered;
        }
    }
}

void DashboardOverloadResponderSet::notifyPendingTerminalOwner() noexcept
{
    std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
    bool missingManagedSourceObserver{};
    {
        const std::scoped_lock lock{mutex_};
        if (terminalOwnerNotificationLifecycle_ !=
            TerminalOwnerNotificationLifecycle::Pending) {
            return;
        }
        observer = drainObserver_.lock();
        if (observer == nullptr) {
            missingManagedSourceObserver =
                recordMissingManagedSourceDrainObserverLocked();
        } else {
            terminalOwnerNotificationLifecycle_ =
                TerminalOwnerNotificationLifecycle::Delivering;
        }
    }
    if (missingManagedSourceObserver) {
        failFast_->failFast();
        return;
    }
    if (observer == nullptr) {
        return;
    }
    observer->overloadOwnerBecameTerminal();
    {
        const std::scoped_lock lock{mutex_};
        terminalOwnerNotificationLifecycle_ =
            TerminalOwnerNotificationLifecycle::Delivered;
    }
}

void DashboardOverloadResponderSet::notifyPendingTerminalGenerations()
    noexcept
{
    notifyPendingTerminalGenerationLatches();
    notifyPendingTerminalOwner();
    for (;;) {
        std::optional<std::size_t> pendingIndex;
        std::uint64_t generationId{};
        std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
        bool failFastRequired{};
        {
            const std::scoped_lock lock{mutex_};
            for (std::size_t index{}; index < terminalGenerationCount_;
                 ++index) {
                if (terminalGenerationNotificationLifecycles_[index] ==
                        TerminalGenerationNotificationLifecycle::Pending &&
                    terminalGenerationPendingNotificationLifecycles_[index] ==
                        TerminalGenerationPendingNotificationLifecycle::
                            Delivered) {
                    pendingIndex.emplace(index);
                    generationId = terminalGenerationIds_[index];
                    break;
                }
            }
            if (pendingIndex.has_value()) {
                observer = drainObserver_.lock();
                if (observer == nullptr) {
                    failFastRequired =
                        recordMissingManagedSourceDrainObserverLocked();
                } else {
                    terminalGenerationNotificationLifecycles_[
                        *pendingIndex] =
                        TerminalGenerationNotificationLifecycle::Delivering;
                }
            }
        }

        if (!pendingIndex.has_value()) {
            notifyProcessDrainObserverIfReady();
            return;
        }
        if (observer == nullptr) {
            if (failFastRequired) {
                failFast_->failFast();
            }
            return;
        }
        observer->overloadGenerationBecameTerminal(generationId);
        // The terminal observer has now pinned and failed closed the exact
        // generation. Re-check ownership only after that causal edge; a
        // concurrent token return suppressed its earlier ordinary drain edge.
        notifyGenerationMayHaveDrained(generationId);
        {
            const std::scoped_lock lock{mutex_};
            terminalGenerationNotificationLifecycles_[*pendingIndex] =
                TerminalGenerationNotificationLifecycle::Delivered;
        }
    }
}

void DashboardOverloadResponderSet::beginTerminalShutdown(
    const std::optional<std::uint64_t> generationId,
    std::optional<Domain::Error> failure) noexcept
{
    {
        const std::scoped_lock lock{mutex_};
        if (failure.has_value()) {
            retainFailureLocked(std::move(*failure));
        }
        if (generationId.has_value()) {
            rememberTerminalGenerationLocked(*generationId);
        }
        shutdownRequested_ = true;
        terminalShutdownRequested_ = true;
        rememberTerminalOwnerLocked();
        rememberAllLiveTerminalGenerationsLocked();
        requestAllCancellationsLocked();
    }
    notifyPendingTerminalGenerationLatches();
    refreshDeadline();
}

void DashboardOverloadResponderSet::retainFailureLocked(
    Domain::Error error) noexcept
{
    if (!firstLifecycleFailure_.has_value()) {
        firstLifecycleFailureSnapshot_.emplace(classifyFailure(error));
        firstLifecycleFailure_.emplace(std::move(error));
    }
}

void DashboardOverloadResponderSet::retainFailure(
    Domain::Error error) noexcept
{
    const std::scoped_lock lock{mutex_};
    retainFailureLocked(std::move(error));
}

bool DashboardOverloadResponderSet::failDeadlineScheduleLocked(
    Domain::Error error) noexcept
{
    if (currentDeadline_.has_value()) {
        static_cast<void>(deadlineScheduler_->cancel(
            deadlineRegistrationId_, currentDeadline_->armSequence));
        currentDeadline_.reset();
    }

    bool liveOwnership{};
    {
        const std::scoped_lock lock{mutex_};
        retainFailureLocked(std::move(error));
        shutdownRequested_ = true;
        terminalShutdownRequested_ = true;
        rememberTerminalOwnerLocked();
        rememberAllLiveTerminalGenerationsLocked();
        requestAllCancellationsLocked();
        for (auto& slot : slots_) {
            if (slot.work.has_value()) {
                slot.work->closeNativeSocket();
                liveOwnership = true;
            }
        }
        if (liveOwnership) {
            incrementSaturated(failFastCount_);
        }
    }
    return liveOwnership;
}

void DashboardOverloadResponderSet::refreshDeadline() noexcept
{
    bool failFastRequired{};
    try {
        {
            const std::scoped_lock deadlineLock{deadlineMutex_};
            std::optional<Domain::MonotonicTimePoint> earliest;
            {
                const std::scoped_lock lock{mutex_};
                for (const auto& slot : slots_) {
                    if (!slot.work.has_value()) {
                        continue;
                    }
                    const auto candidate = slot.cancellationRequested
                        ? slot.cancellationDeadlineAt
                        : slot.deadlineAt;
                    if (!earliest.has_value() || candidate < *earliest) {
                        earliest = candidate;
                    }
                }
            }

            if (!earliest.has_value()) {
                if (currentDeadline_.has_value()) {
                    static_cast<void>(deadlineScheduler_->cancel(
                        deadlineRegistrationId_,
                        currentDeadline_->armSequence));
                    currentDeadline_.reset();
                }
                return;
            }

            if (currentDeadline_.has_value() &&
                currentDeadline_->kind ==
                    WindowsDashboardDeadlineKind::OverloadResponse &&
                currentDeadline_->deadline == *earliest) {
                return;
            }

            auto scheduled = deadlineScheduler_->schedule(
                WindowsDashboardDeadlineRequest{
                    deadlineRegistrationId_,
                    WindowsDashboardDeadlineKind::OverloadResponse,
                    *earliest});
            if (!scheduled) {
                failFastRequired = failDeadlineScheduleLocked(
                    std::move(scheduled).error());
            } else {
                currentDeadline_.emplace(std::move(scheduled).value());
            }
        }
    } catch (...) {
        {
            const std::scoped_lock deadlineLock{deadlineMutex_};
            failFastRequired = failDeadlineScheduleLocked(overloadError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard overload response deadline could not be refreshed safely."));
        }
    }
    notifyPendingTerminalGenerationLatches();
    if (failFastRequired) {
        failFast_->failFast();
    }
}

void DashboardOverloadResponderSet::dispatchDeadline(
    const WindowsDashboardDeadline deadline) noexcept
{
    try {
        bool failFastRequired{};
        {
            const std::scoped_lock deadlineLock{deadlineMutex_};
            if (!currentDeadline_.has_value() ||
                deadline != *currentDeadline_ ||
                deadline.registrationId != deadlineRegistrationId_ ||
                deadline.kind !=
                    WindowsDashboardDeadlineKind::OverloadResponse) {
                const std::scoped_lock lock{mutex_};
                incrementSaturated(staleDeadlineCount_);
                return;
            }
            currentDeadline_.reset();

            const std::scoped_lock lock{mutex_};
            for (auto& slot : slots_) {
                if (!slot.work.has_value()) {
                    continue;
                }
                if (slot.cancellationRequested) {
                    if (slot.cancellationDeadlineAt <= deadline.deadline) {
                        retainFailureLocked(integrityError(
                            "A dashboard overload native operation exceeded its cancellation-reap deadline."));
                        shutdownRequested_ = true;
                        terminalShutdownRequested_ = true;
                        rememberTerminalOwnerLocked();
                        failFastRequired = true;
                    }
                    continue;
                }
                if (slot.deadlineAt <= deadline.deadline) {
                    if (!requestCancellationLocked(slot, false)) {
                        shutdownRequested_ = true;
                    }
                    incrementSaturated(expiredCount_);
                }
            }
            if (shutdownRequested_) {
                rememberAllLiveTerminalGenerationsLocked();
                requestAllCancellationsLocked();
            }
            if (failFastRequired) {
                for (auto& slot : slots_) {
                    if (slot.work.has_value()) {
                        slot.work->closeNativeSocket();
                    }
                }
                incrementSaturated(failFastCount_);
            }
        }
        notifyPendingTerminalGenerationLatches();
        notifyPendingTerminalGenerations();
        if (failFastRequired) {
            failFast_->failFast();
            return;
        }
        refreshDeadline();
    } catch (...) {
        bool failFastRequired{};
        {
            const std::scoped_lock deadlineLock{deadlineMutex_};
            failFastRequired = failDeadlineScheduleLocked(overloadError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard overload response deadline could not be dispatched safely."));
        }
        notifyPendingTerminalGenerationLatches();
        notifyPendingTerminalGenerations();
        if (failFastRequired) {
            failFast_->failFast();
        }
    }
}

Domain::Result<DashboardOverloadAdmissionDisposition>
DashboardOverloadResponderSet::admit(
    DashboardAdmissionOverloadWork work) noexcept
{
    const auto originGenerationId = work.originGenerationId();
    std::optional<std::size_t> reservedIndex;
    bool closedDuringShutdown{};
    bool closedDuringTerminalShutdown{};
    {
        const std::scoped_lock lock{mutex_};
        closedDuringShutdown = shutdownRequested_;
        closedDuringTerminalShutdown = terminalShutdownRequested_;
        if (!closedDuringShutdown) {
            reservedIndex = findEmptyLocked();
        }
        if (reservedIndex.has_value()) {
            auto& slot = slots_[*reservedIndex];
            slot.work.emplace(std::move(work));
            slot.sendOffset = 0U;
            slot.deadlineAt = overloadDeadlineAt(
                slot.work->admittedAt());
            slot.cancellationDeadlineAt = {};
            slot.socketShutdownRequested = false;
            slot.cancellationRequested = false;
            slot.closeOriginAdmissionRequested = false;
            slot.nativeCancellationFailure = false;
            slot.lifecycle = SlotLifecycle::Associating;
        } else {
            ++settlingWorkCount_;
        }
    }

    if (!reservedIndex.has_value()) {
        if (closedDuringTerminalShutdown) {
            beginTerminalShutdown(originGenerationId);
        }
        DetachedWork detached{
            std::optional<DashboardAdmissionOverloadWork>{
                std::in_place, std::move(work)},
            true};
        auto finishFailure = finishDetached(
            std::move(detached), closedDuringShutdown);
        {
            const std::scoped_lock lock{mutex_};
            incrementSaturated(abandonedCount_);
        }
        if (finishFailure.has_value()) {
            auto error = std::move(*finishFailure);
            beginTerminalShutdown(
                originGenerationId, copyForRetention(error));
            return Domain::Result<
                DashboardOverloadAdmissionDisposition>::failure(
                std::move(error));
        }
        return Domain::Result<
            DashboardOverloadAdmissionDisposition>::success(
            closedDuringShutdown
                ? DashboardOverloadAdmissionDisposition::
                      ClosedDuringShutdown
                : DashboardOverloadAdmissionDisposition::ConnectionClosed);
    }

    refreshDeadline();

    auto associated = [&]() noexcept -> Domain::Result<void> {
        const std::scoped_lock lock{mutex_};
        const auto& slot = slots_[*reservedIndex];
        if (slot.lifecycle != SlotLifecycle::Associating ||
            !slot.work.has_value() ||
            slot.work->borrowedNativeSocket() == INVALID_SOCKET) {
            return Domain::Result<void>::failure(integrityError(
                "A dashboard overload responder lost its exact accepted "
                "socket before IOCP association."));
        }
        return socketAssociator_->associateSocket(
            slot.work->borrowedNativeSocket(), completionKey_);
    }();
    if (!associated) {
        auto error = std::move(associated).error();
        DetachedWork detached;
        bool cancellationRequested{};
        bool closeOriginAdmission{true};
        {
            const std::scoped_lock lock{mutex_};
            cancellationRequested =
                slots_[*reservedIndex].cancellationRequested;
            closeOriginAdmission =
                slots_[*reservedIndex].closeOriginAdmissionRequested ||
                !cancellationRequested;
            detached = detachLocked(*reservedIndex);
            incrementSaturated(abandonedCount_);
        }
        notifyPendingTerminalGenerationLatches();
        refreshDeadline();
        if (!cancellationRequested) {
            beginTerminalShutdown(
                originGenerationId, copyForRetention(error));
        }
        auto finishFailure = finishDetached(
            std::move(detached), closeOriginAdmission);
        if (finishFailure.has_value()) {
            beginTerminalShutdown(
                originGenerationId,
                copyForRetention(*finishFailure));
        }
        if (cancellationRequested && !finishFailure.has_value()) {
            return Domain::Result<
                DashboardOverloadAdmissionDisposition>::success(
                DashboardOverloadAdmissionDisposition::ClosedDuringShutdown);
        }
        return Domain::Result<
            DashboardOverloadAdmissionDisposition>::failure(
            std::move(error));
    }

    DetachedWork detached;
    std::optional<Domain::Error> issueFailure;
    bool finishForShutdown{};
    bool closeOriginAdmission{};
    {
        const std::scoped_lock lock{mutex_};
        auto& slot = slots_[*reservedIndex];
        if (slot.lifecycle != SlotLifecycle::Associating ||
            !slot.work.has_value()) {
            issueFailure.emplace(integrityError(
                "A dashboard overload responder lost its associating "
                "ownership before send issue."));
            if (slot.work.has_value()) {
                detached = detachLocked(*reservedIndex);
            }
            incrementSaturated(abandonedCount_);
            finishForShutdown = true;
            closeOriginAdmission = true;
        } else if (slot.cancellationRequested) {
            finishForShutdown = shutdownRequested_ ||
                slot.closeOriginAdmissionRequested;
            closeOriginAdmission =
                slot.closeOriginAdmissionRequested;
            detached = detachLocked(*reservedIndex);
            incrementSaturated(abandonedCount_);
        } else {
            auto issued = issueSendLocked(*reservedIndex);
            if (issued) {
                return Domain::Result<
                    DashboardOverloadAdmissionDisposition>::success(
                    DashboardOverloadAdmissionDisposition::
                        ResponseStarted);
            }
            issueFailure.emplace(normalizeIssueFailure(
                std::move(issued).error()));
            detached = detachLocked(*reservedIndex);
            incrementSaturated(abandonedCount_);
        }
    }

    refreshDeadline();

    const bool terminalIssue = issueFailure.has_value() &&
        !isExpectedAbandonment(*issueFailure);
    if (terminalIssue) {
        beginTerminalShutdown(
            originGenerationId, copyForRetention(*issueFailure));
        finishForShutdown = true;
        closeOriginAdmission = true;
    }
    auto finishFailure = finishDetached(
        std::move(detached), closeOriginAdmission);
    if (finishFailure.has_value()) {
        auto error = std::move(*finishFailure);
        beginTerminalShutdown(
            originGenerationId, copyForRetention(error));
        return Domain::Result<
            DashboardOverloadAdmissionDisposition>::failure(
            std::move(error));
    }
    if (terminalIssue) {
        return Domain::Result<
            DashboardOverloadAdmissionDisposition>::failure(
            std::move(*issueFailure));
    }
    if (finishForShutdown) {
        return Domain::Result<
            DashboardOverloadAdmissionDisposition>::success(
            DashboardOverloadAdmissionDisposition::ClosedDuringShutdown);
    }
    return Domain::Result<
        DashboardOverloadAdmissionDisposition>::success(
        DashboardOverloadAdmissionDisposition::ConnectionClosed);
}

void DashboardOverloadResponderSet::respond(
    DashboardAdmissionOverloadWork work) noexcept
{
    auto admitted = admit(std::move(work));
    if (!admitted) {
        retainFailure(copyForRetention(admitted.error()));
    }
}

std::size_t DashboardOverloadResponderSet::cancelGeneration(
    const std::uint64_t generationId) noexcept
{
    if (generationId == 0U) {
        return 0U;
    }
    try {
        std::size_t cancelled{};
        {
            const std::scoped_lock lock{mutex_};
            bool nativeCancellationHealthy{true};
            for (auto& slot : slots_) {
                if (slot.work.has_value() &&
                    slot.work->originGenerationId() == generationId) {
                    nativeCancellationHealthy =
                        requestCancellationLocked(slot, true) &&
                        nativeCancellationHealthy;
                    ++cancelled;
                }
            }
            if (!nativeCancellationHealthy) {
                shutdownRequested_ = true;
                terminalShutdownRequested_ = true;
                rememberAllLiveTerminalGenerationsLocked();
                requestAllCancellationsLocked();
            }
        }
        notifyPendingTerminalGenerationLatches();
        refreshDeadline();
        return cancelled;
    } catch (...) {
        beginTerminalShutdown(
            generationId,
            overloadError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard overload generation could not be cancelled safely."));
        return 0U;
    }
}

void DashboardOverloadResponderSet::
drainTerminalGenerationNotifications() noexcept
{
    notifyPendingTerminalGenerationLatches();
    notifyPendingTerminalGenerations();
}

Domain::Result<DashboardOverloadReapDisposition>
DashboardOverloadResponderSet::reap(
    const DashboardIoCompletionKey completionKey,
    const DWORD transferredBytes,
    OVERLAPPED* const operation,
    const DWORD nativeError) noexcept
{
    std::optional<DetachedWork> detached;
    std::optional<Domain::Error> terminalFailure;
    std::optional<std::uint64_t> detachedGenerationId;
    auto disposition = DashboardOverloadReapDisposition::ResponseAbandoned;
    bool closeOriginAdmission{};

    {
        const std::scoped_lock lock{mutex_};
        const auto found = findOperationLocked(operation);
        if (!found.has_value()) {
            terminalFailure.emplace(integrityError(
                operation == nullptr
                    ? "A dashboard overload completion carried a null "
                      "OVERLAPPED pointer."
                    : "A dashboard overload completion did not match any "
                      "of the eight live OVERLAPPED values."));
        } else {
            auto& slot = slots_[*found];
            detachedGenerationId.emplace(
                slot.work->originGenerationId());
            const bool cancellationRequested =
                slot.cancellationRequested;
            closeOriginAdmission =
                slot.closeOriginAdmissionRequested;
            const std::size_t remaining =
                responseBytes_->size() - slot.sendOffset;

            // The exact packet has left the kernel queue. Clear its native
            // metadata before validating packet shape so every exact failure
            // remains destructible and cannot await a nonexistent second reap.
            resetOperationLocked(*found);

            std::optional<Domain::Error> operationFailure;
            if (!(completionKey == completionKey_)) {
                operationFailure.emplace(integrityError(
                    "A dashboard overload completion carried the wrong "
                    "fixed-owner key."));
            } else if (static_cast<std::size_t>(transferredBytes) >
                       remaining) {
                operationFailure.emplace(integrityError(
                    "A dashboard overload completion exceeded its active "
                    "immutable response suffix."));
            } else if (nativeError == ERROR_OPERATION_ABORTED &&
                       !cancellationRequested) {
                operationFailure.emplace(integrityError(
                    "A dashboard overload send was aborted without an exact owner cancellation request."));
            } else if (nativeError != ERROR_SUCCESS) {
                operationFailure.emplace(
                    completionNativeError(nativeError));
            } else if (transferredBytes == 0U) {
                operationFailure.emplace(overloadError(
                    Domain::ErrorCodes::TransportClosed,
                    "A dashboard overload send completed with zero bytes."));
            }

            if (operationFailure.has_value()) {
                if (!isExpectedCompletionAbandonment(
                        *operationFailure)) {
                    terminalFailure.emplace(
                        std::move(*operationFailure));
                    closeOriginAdmission = true;
                } else {
                    closeOriginAdmission =
                        slot.closeOriginAdmissionRequested;
                }
                detached.emplace(detachLocked(*found));
                incrementSaturated(abandonedCount_);
            } else {
                slot.sendOffset +=
                    static_cast<std::size_t>(transferredBytes);
                if (slot.sendOffset == responseBytes_->size()) {
                    detached.emplace(detachLocked(*found));
                    incrementSaturated(deliveredCount_);
                    disposition = DashboardOverloadReapDisposition::
                        ResponseDelivered;
                } else if (cancellationRequested) {
                    detached.emplace(detachLocked(*found));
                    incrementSaturated(abandonedCount_);
                    disposition = DashboardOverloadReapDisposition::
                        ResponseAbandoned;
                } else {
                    auto issued = issueSendLocked(*found);
                    if (issued) {
                        disposition = DashboardOverloadReapDisposition::
                            PartialSendReissued;
                    } else {
                        auto error = normalizeIssueFailure(
                            std::move(issued).error());
                        if (!isExpectedAbandonment(error)) {
                            terminalFailure.emplace(std::move(error));
                            closeOriginAdmission = true;
                        }
                        detached.emplace(detachLocked(*found));
                        incrementSaturated(abandonedCount_);
                        disposition = DashboardOverloadReapDisposition::
                            ResponseAbandoned;
                    }
                }
            }
        }
    }

    if (terminalFailure.has_value()) {
        beginTerminalShutdown(
            detachedGenerationId,
            copyForRetention(*terminalFailure));
        closeOriginAdmission = true;
    } else if (detached.has_value()) {
        refreshDeadline();
    }
    if (detached.has_value()) {
        auto finishFailure = finishDetached(
            std::move(*detached), closeOriginAdmission);
        if (finishFailure.has_value()) {
            auto error = std::move(*finishFailure);
            beginTerminalShutdown(
                detachedGenerationId, copyForRetention(error));
            return Domain::Result<
                DashboardOverloadReapDisposition>::failure(
                std::move(error));
        }
    }
    if (terminalFailure.has_value()) {
        return Domain::Result<
            DashboardOverloadReapDisposition>::failure(
            std::move(*terminalFailure));
    }
    return Domain::Result<DashboardOverloadReapDisposition>::success(
        disposition);
}

void DashboardOverloadResponderSet::consume(
    const DashboardIoCompletionPacket packet,
    const DWORD nativeError) noexcept
{
    auto consumed = reap(
        packet.completionKey,
        packet.transferredBytes,
        packet.operation,
        nativeError);
    if (!consumed) {
        retainFailure(copyForRetention(consumed.error()));
    }
    notifyPendingTerminalGenerations();
}

void DashboardOverloadResponderSet::fatal(
    const DWORD nativeError) noexcept
{
    beginTerminalShutdown(
        std::nullopt, fatalIocpError(nativeError));
    notifyPendingTerminalGenerations();
}

void DashboardOverloadResponderSet::beginShutdown() noexcept
{
    bool missingManagedSourceObserver{};
    {
        const std::scoped_lock shutdownTransitionLock{
            shutdownTransitionMutex_};
        std::shared_ptr<IDashboardAdmissionOverloadDrainObserver> observer;
        bool publishShutdown{};
        bool completeOrdinaryShutdownSequence{};
        {
            const std::scoped_lock lock{mutex_};
            shutdownTransitionInProgress_ = true;
            completeOrdinaryShutdownSequence =
                ordinaryShutdownSequenceCompleted_;
            publishShutdown = !shutdownRequested_;
            shutdownRequested_ = true;
            if (publishShutdown) {
                observer = drainObserver_.lock();
                if (observer == nullptr) {
                    missingManagedSourceObserver =
                        recordMissingManagedSourceDrainObserverLocked();
                }
                completeOrdinaryShutdownSequence =
                    observer != nullptr ||
                    !processDrainObserverEverBound_;
            }
        }
        if (observer != nullptr) {
            observer->overloadOwnerBeganShutdown();
        }
        {
            const std::scoped_lock lock{mutex_};
            requestAllCancellationsLocked();
        }
        notifyPendingTerminalGenerationLatches();
        refreshDeadline();
        {
            const std::scoped_lock lock{mutex_};
            if (completeOrdinaryShutdownSequence) {
                ordinaryShutdownSequenceCompleted_ = true;
            }
            shutdownTransitionInProgress_ = false;
        }
    }
    // Native cancellation can escalate an ordinary shutdown while its source
    // callback is in flight. Publish the process-owner terminal edge here;
    // generation terminal delivery remains on its explicit bounded drain.
    notifyPendingTerminalOwner();
    if (missingManagedSourceObserver) {
        failFast_->failFast();
    }
    notifyProcessDrainObserverIfReady();
}

DashboardOverloadResponderSnapshot
DashboardOverloadResponderSet::snapshotLocked(
    const bool deadlineArmed) const noexcept
{
    std::size_t associating{};
    std::size_t sending{};
    std::size_t cancellationRequested{};
    for (const auto& slot : slots_) {
        switch (slot.lifecycle) {
        case SlotLifecycle::Empty:
            break;
        case SlotLifecycle::Associating:
            ++associating;
            break;
        case SlotLifecycle::Sending:
            ++sending;
            break;
        case SlotLifecycle::CancellationRequested:
            ++cancellationRequested;
            break;
        }
    }
    return DashboardOverloadResponderSnapshot{
        associating,
        sending,
        cancellationRequested,
        settlingWorkCount_,
        Capacity,
        deliveredCount_,
        abandonedCount_,
        deadlineArmed,
        expiredCount_,
        staleDeadlineCount_,
        failFastCount_,
        shutdownRequested_,
        ordinaryShutdownSequenceCompleted_,
        terminalOwnerNotificationLifecycle_ !=
            TerminalOwnerNotificationLifecycle::None,
        terminalOwnerNotificationLifecycle_ ==
            TerminalOwnerNotificationLifecycle::Delivered,
        managedSourceDrainObserverFailureReported_,
        processDrainObserverPublicationFailed_,
        shutdownTransitionInProgress_,
        terminalGenerationPublicationsCompletedLocked(),
        firstLifecycleFailureSnapshot_};
}

DashboardOverloadResponderSnapshot
DashboardOverloadResponderSet::snapshot() const noexcept
{
    try {
        const std::scoped_lock deadlineLock{deadlineMutex_};
        const std::scoped_lock lock{mutex_};
        return snapshotLocked(currentDeadline_.has_value());
    } catch (...) {
        return DashboardOverloadResponderSnapshot{
            Capacity,
            0U,
            0U,
            0U,
            Capacity,
            0U,
            0U,
            false,
            0U,
            0U,
            0U,
            true,
            true,
            false,
            false,
            true,
            true,
            true,
            false,
            DashboardOverloadResponderFailure{
                DashboardOverloadResponderFailureKind::InternalFailure,
                false}};
    }
}

std::optional<Domain::Error>
DashboardOverloadResponderSet::fullLifecycleFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstLifecycleFailure_;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
