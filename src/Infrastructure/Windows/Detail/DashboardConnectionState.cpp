#include "DashboardConnectionState.h"

#include "DashboardHandlerOperations.h"

#include "ForgeConductor/Dashboard/DashboardHttpParserSession.h"
#include "ForgeConductor/Dashboard/DashboardSseBroadcaster.h"
#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error connectionStateError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection state failed and its diagnostic could not be retained.",
            retryable);
    }
}

[[nodiscard]] Domain::Error invalidConnectionStateError(
    std::string message) noexcept
{
    return connectionStateError(
        Domain::ErrorCodes::InvalidRequest, std::move(message));
}

[[nodiscard]] Domain::Error integrityConnectionStateError(
    std::string message) noexcept
{
    return connectionStateError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] Domain::Error deadlineConnectionStateError(
    std::string message) noexcept
{
    return connectionStateError(
        Domain::ErrorCodes::DeadlineExceeded, std::move(message));
}

} // namespace

DashboardConnectionStateSnapshot::DashboardConnectionStateSnapshot(
    const DashboardConnectionLifecycleState state,
    const std::uint64_t registrationId,
    const std::uint64_t generationId,
    const DashboardIoCompletionKey completionKey,
    const bool socketOperationOutstanding,
    const bool eventOperationOutstanding,
    const bool deadlineArmed,
    const bool shutdownRequested,
    const bool hasFailure) noexcept
    : state_{state},
      registrationId_{registrationId},
      generationId_{generationId},
      completionKey_{completionKey},
      socketOperationOutstanding_{socketOperationOutstanding},
      eventOperationOutstanding_{eventOperationOutstanding},
      deadlineArmed_{deadlineArmed},
      shutdownRequested_{shutdownRequested},
      hasFailure_{hasFailure}
{
}

class DashboardConnectionState::Impl final {
public:
    Impl(
        const std::uint64_t generationId,
        const DashboardConnectionRuntimeIdentity identity,
        const Domain::MonotonicTimePoint admittedAt,
        DashboardAdmissionController::Lease admissionLease,
        std::unique_ptr<IDashboardConnectionIo> socket,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        DashboardConnectionResponseCatalog& responseCatalog) noexcept
        : generationId_{generationId},
          identity_{identity},
          admittedAt_{admittedAt},
          admissionLease_{std::move(admissionLease)},
          socket_{std::move(socket)},
          deadlineScheduler_{std::addressof(deadlineScheduler)},
          handlerExecutor_{std::addressof(handlerExecutor)},
          runtimeServices_{std::addressof(runtimeServices)},
          application_{std::move(application)},
          responseCatalog_{std::addressof(responseCatalog)}
    {
    }

    ~Impl() noexcept
    {
        const auto current = state_.load(std::memory_order_acquire);
        if (current != DashboardConnectionLifecycleState::Created &&
            current != DashboardConnectionLifecycleState::Drained) {
            std::terminate();
        }
    }

    void setEventBridge(
        std::shared_ptr<DashboardConnectionEventBridge> bridge) noexcept
    {
        eventBridge_ = std::move(bridge);
    }

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<IDashboardConnectionDrainObserver> observer)
        noexcept;
    [[nodiscard]] Domain::Result<void> start() noexcept;
    void dispatchIocp(
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept;
    void dispatchDeadline(WindowsDashboardDeadline deadline) noexcept;
    void beginShutdown() noexcept;
    void eventFatal(
        DashboardConnectionEventFatalNotification notification) noexcept;

    [[nodiscard]] DashboardConnectionStateSnapshot snapshot() const noexcept;
    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;
    [[nodiscard]] bool isDrained() const noexcept
    {
        return state_.load(std::memory_order_acquire) ==
            DashboardConnectionLifecycleState::Drained;
    }

    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept
    {
        return identity_.completionKey;
    }
    [[nodiscard]] std::uint64_t registrationId() const noexcept
    {
        return identity_.registrationId;
    }
    [[nodiscard]] std::uint64_t generationId() const noexcept
    {
        return generationId_;
    }

private:
    class ExternalNotificationDrain final {
    public:
        // DashboardConnectionEventBridge::retainFatalFailureLocked emits at
        // most its first retained fatal notification. This guard converts
        // that edge into closure and publishes the one-shot registry drain
        // edge only after the public state lock has been released.
        explicit ExternalNotificationDrain(Impl& owner) noexcept
            : owner_{&owner}
        {
        }
        ExternalNotificationDrain(const ExternalNotificationDrain&) = delete;
        ExternalNotificationDrain& operator=(
            const ExternalNotificationDrain&) = delete;
        ~ExternalNotificationDrain() noexcept
        {
            owner_->drainExternalNotifications();
        }

    private:
        Impl* owner_{};
    };

    [[nodiscard]] bool armDeadlineLocked(
        WindowsDashboardDeadlineKind kind,
        Domain::MonotonicTimePoint deadline) noexcept;
    void cancelDeadlineLocked() noexcept;
    [[nodiscard]] bool issueReceiveLocked() noexcept;
    [[nodiscard]] bool issueCurrentSendLocked() noexcept;
    [[nodiscard]] std::span<const std::byte> currentSendBytesLocked()
        const noexcept;
    void handleSocketCompletionLocked(
        DashboardConnectionSocketReapResult completion) noexcept;
    void handleReceiveLocked() noexcept;
    void submitPrepareLocked() noexcept;
    void handleEventCompletionLocked(
        DashboardConnectionEventReapResult completion) noexcept;
    void handleHandlerCompletionLocked(
        DashboardHandlerCompletion completion) noexcept;
    void handlePreparedExchangeLocked(
        Dashboard::DashboardPreparedExchange exchange) noexcept;
    void startFallbackLocked(
        DashboardConnectionResponseCatalog::ImmutableBytes bytes,
        Domain::Error error) noexcept;
    void handleSendCompletionLocked(DWORD transferredBytes) noexcept;
    void finishCompleteSendLocked() noexcept;
    void finishSseBootstrapSegmentLocked() noexcept;
    void finishSseFrameLocked() noexcept;
    void observeSseReadyLocked() noexcept;
    void maybeStartSseFrameLocked() noexcept;
    void armSseWaitLocked() noexcept;
    void closeLocked(std::optional<Domain::Error> failure) noexcept;
    void maybeDrainLocked() noexcept;
    void retainFailureLocked(Domain::Error error) noexcept;
    void consumePendingFatalLocked() noexcept;
    void drainExternalNotifications() noexcept;

    const std::uint64_t generationId_{};
    const DashboardConnectionRuntimeIdentity identity_{};
    const Domain::MonotonicTimePoint admittedAt_{};
    DashboardAdmissionController::Lease admissionLease_;
    std::unique_ptr<IDashboardConnectionIo> socket_;
    WindowsDashboardDeadlineScheduler* deadlineScheduler_{};
    WindowsDashboardHandlerExecutor* handlerExecutor_{};
    DashboardConnectionRuntimeServices* runtimeServices_{};
    std::shared_ptr<Dashboard::IDashboardConnectionApplication>
        application_;
    DashboardConnectionResponseCatalog* responseCatalog_{};
    std::shared_ptr<DashboardConnectionEventBridge> eventBridge_;
    std::weak_ptr<IDashboardConnectionDrainObserver> drainObserver_;

    Dashboard::DashboardHttpParserSession parser_;
    std::optional<Dashboard::DashboardPreparedExchange> preparedExchange_;
    DashboardConnectionResponseCatalog::ImmutableBytes fallbackBytes_;
    Dashboard::DashboardSseFramePair::ImmutableBytes sseFrameBytes_;
    Dashboard::DashboardSseDeliveryCursor sseCursor_;
    std::optional<WindowsDashboardHandlerExecutor::Reservation>
        postDeliveryReservation_;
    std::optional<WindowsDashboardDeadline> currentDeadline_;
    std::optional<Domain::Error> firstFailure_;
    std::stop_source prepareStop_;
    std::stop_source postDeliveryStop_;
    Domain::MonotonicTimePoint socketLifetimeDeadline_{};
    Domain::MonotonicTimePoint sseLifetimeDeadline_{};
    Domain::MonotonicTimePoint nextSseDelivery_{};
    Dashboard::DashboardPostDeliveryAction postDeliveryAction_{
        Dashboard::DashboardPostDeliveryAction::None};
    std::size_t sendOffset_{};
    std::uint8_t sseBootstrapSegment_{};
    bool sseReadyPending_{};
    bool shutdownRequested_{};
    bool drainObserverBound_{};
    bool drainNotificationSent_{};
    std::atomic_bool eventFatalPending_{};
    std::atomic_bool drainNotificationPending_{};
    std::atomic<DashboardConnectionLifecycleState> state_{
        DashboardConnectionLifecycleState::Created};
    mutable std::mutex mutex_;
};

Domain::Result<void> DashboardConnectionState::Impl::bindDrainObserver(
    std::weak_ptr<IDashboardConnectionDrainObserver> observer) noexcept
{
    try {
        if (observer.expired()) {
            return Domain::Result<void>::failure(
                invalidConnectionStateError(
                    "A dashboard connection requires a live registry drain observer."));
        }
        {
            const std::scoped_lock lock{mutex_};
            if (drainObserverBound_) {
                return Domain::Result<void>::failure(
                    invalidConnectionStateError(
                        "A dashboard connection registry drain observer is one-shot."));
            }
            drainObserver_ = std::move(observer);
            drainObserverBound_ = true;
            if (state_.load(std::memory_order_relaxed) ==
                    DashboardConnectionLifecycleState::Drained &&
                !drainNotificationSent_) {
                drainNotificationSent_ = true;
                drainNotificationPending_.store(
                    true, std::memory_order_release);
            }
        }
        drainExternalNotifications();
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(connectionStateError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection could not bind its registry drain observer."));
    }
}

Domain::Result<void> DashboardConnectionState::Impl::start() noexcept
{
    ExternalNotificationDrain drain{*this};
    const std::scoped_lock lock{mutex_};
    if (state_.load(std::memory_order_relaxed) !=
            DashboardConnectionLifecycleState::Created ||
        eventBridge_ == nullptr) {
        return Domain::Result<void>::failure(invalidConnectionStateError(
            "A dashboard connection can be started exactly once after its event bridge is installed."));
    }

    const auto now = runtimeServices_->monotonicNow();
    const auto headerIngressDeadline = admittedAt_ +
        DashboardConnectionState::HeaderIngressLifetime;
    socketLifetimeDeadline_ = admittedAt_ +
        DashboardConnectionState::SocketLifetime;
    if (admittedAt_ > now) {
        closeLocked(integrityConnectionStateError(
            "The dashboard connection admission timestamp was ahead of its monotonic start observation."));
        return Domain::Result<void>::failure(
            integrityConnectionStateError(
                "The dashboard connection admission timestamp was ahead of its monotonic start observation."));
    }
    if (now >= headerIngressDeadline || now >= socketLifetimeDeadline_) {
        closeLocked(deadlineConnectionStateError(
            "The dashboard connection header-ingress lifetime expired before transport start."));
        return Domain::Result<void>::failure(
            deadlineConnectionStateError(
                "The dashboard connection header-ingress lifetime expired before transport start."));
    }
    state_.store(
        DashboardConnectionLifecycleState::Receiving,
        std::memory_order_release);
    if (!armDeadlineLocked(
            WindowsDashboardDeadlineKind::HeaderIngress,
            (std::min)(headerIngressDeadline, socketLifetimeDeadline_)) ||
        !issueReceiveLocked()) {
        closeLocked(std::nullopt);
        return Domain::Result<void>::failure(connectionStateError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection could not start its bounded ingress."));
    }
    return Domain::Result<void>::success();
}

bool DashboardConnectionState::Impl::armDeadlineLocked(
    const WindowsDashboardDeadlineKind kind,
    const Domain::MonotonicTimePoint deadline) noexcept
{
    auto scheduled = deadlineScheduler_->schedule(
        WindowsDashboardDeadlineRequest{
            identity_.registrationId, kind, deadline});
    if (!scheduled) {
        retainFailureLocked(std::move(scheduled).error());
        return false;
    }
    currentDeadline_.emplace(std::move(scheduled).value());
    return true;
}

void DashboardConnectionState::Impl::cancelDeadlineLocked() noexcept
{
    if (!currentDeadline_.has_value()) {
        return;
    }
    static_cast<void>(deadlineScheduler_->cancel(
        currentDeadline_->registrationId,
        currentDeadline_->armSequence));
    currentDeadline_.reset();
}

bool DashboardConnectionState::Impl::issueReceiveLocked() noexcept
{
    auto issued = socket_->issueReceive();
    if (!issued) {
        retainFailureLocked(std::move(issued).error());
        return false;
    }
    return true;
}

void DashboardConnectionState::Impl::dispatchIocp(
    const DWORD transferredBytes,
    OVERLAPPED* const operation,
    const DWORD nativeError) noexcept
{
    ExternalNotificationDrain drain{*this};
    if (operation == socket_->borrowedOperation()) {
        auto reaped = socket_->reap(
            identity_.completionKey,
            transferredBytes,
            operation,
            nativeError);
        const std::scoped_lock lock{mutex_};
        if (!reaped) {
            closeLocked(std::move(reaped).error());
        } else {
            handleSocketCompletionLocked(std::move(reaped).value());
        }
        consumePendingFatalLocked();
        maybeDrainLocked();
        return;
    }

    auto reaped = eventBridge_->reap(
        identity_.completionKey,
        transferredBytes,
        operation,
        nativeError);
    const std::scoped_lock lock{mutex_};
    if (!reaped) {
        closeLocked(std::move(reaped).error());
    } else {
        handleEventCompletionLocked(std::move(reaped).value());
    }
    consumePendingFatalLocked();
    maybeDrainLocked();
}

void DashboardConnectionState::Impl::handleSocketCompletionLocked(
    DashboardConnectionSocketReapResult completion) noexcept
{
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionLifecycleState::Closing ||
        current == DashboardConnectionLifecycleState::Drained) {
        maybeDrainLocked();
        return;
    }
    if (completion.operationKind() ==
        DashboardConnectionSocketOperationKind::Receive) {
        if (current != DashboardConnectionLifecycleState::Receiving) {
            closeLocked(integrityConnectionStateError(
                "A dashboard receive completed outside the ingress state."));
            return;
        }
        handleReceiveLocked();
        return;
    }

    if (current != DashboardConnectionLifecycleState::SendingComplete &&
        current != DashboardConnectionLifecycleState::SendingSseBootstrap &&
        current != DashboardConnectionLifecycleState::SendingSseFrame) {
        closeLocked(integrityConnectionStateError(
            "A dashboard send completed outside a delivery state."));
        return;
    }
    handleSendCompletionLocked(completion.transferredBytes());
}

void DashboardConnectionState::Impl::handleReceiveLocked() noexcept
{
    const auto received = socket_->receivedBytes();
    if (received.empty()) {
        closeLocked(integrityConnectionStateError(
            "A successful dashboard receive did not retain its bytes."));
        return;
    }

    const auto parsed = parser_.append(received);
    if (parsed.state() ==
            Dashboard::DashboardHttpParserSessionState::ReceivingHeader ||
        parsed.state() ==
            Dashboard::DashboardHttpParserSessionState::ReceivingBody) {
        if (parsed.state() ==
                Dashboard::DashboardHttpParserSessionState::ReceivingBody &&
            currentDeadline_.has_value() &&
            currentDeadline_->kind ==
                WindowsDashboardDeadlineKind::HeaderIngress) {
            if (!armDeadlineLocked(
                    WindowsDashboardDeadlineKind::SocketLifetime,
                    socketLifetimeDeadline_)) {
                closeLocked(std::nullopt);
                return;
            }
        }
        if (!issueReceiveLocked()) {
            closeLocked(std::nullopt);
        }
        return;
    }

    if (parsed.state() ==
            Dashboard::DashboardHttpParserSessionState::Complete ||
        parsed.state() ==
            Dashboard::DashboardHttpParserSessionState::Rejected) {
        submitPrepareLocked();
        return;
    }

    closeLocked(integrityConnectionStateError(
        "The dashboard HTTP parser entered an unusable terminal state."));
}

void DashboardConnectionState::Impl::submitPrepareLocked() noexcept
{
    const auto now = runtimeServices_->monotonicNow();
    const auto prepareDeadline = std::min(
        now + DashboardConnectionRuntimeServices::MaximumOperationLifetime,
        socketLifetimeDeadline_);
    auto context = runtimeServices_->createOperationContext(
        prepareDeadline, prepareStop_.get_token());
    if (!context) {
        startFallbackLocked(
            responseCatalog_->genericServiceUnavailable(),
            std::move(context).error());
        return;
    }

    Domain::Result<std::unique_ptr<DashboardPrepareHandlerOperation>>
        operation = [&]() noexcept {
            if (parser_.state() ==
                Dashboard::DashboardHttpParserSessionState::Complete) {
                auto request = parser_.takeRequest();
                if (!request) {
                    return Domain::Result<std::unique_ptr<
                        DashboardPrepareHandlerOperation>>::failure(
                        integrityConnectionStateError(
                            "A complete dashboard request could not leave its parser owner."));
                }
                return DashboardPrepareHandlerOperation::createRequest(
                    application_,
                    std::move(request).value(),
                    runtimeServices_->operationalServiceActive());
            }
            const auto* rejection = parser_.rejection();
            if (rejection == nullptr) {
                return Domain::Result<std::unique_ptr<
                    DashboardPrepareHandlerOperation>>::failure(
                    integrityConnectionStateError(
                        "A rejected dashboard request did not retain its canonical rejection."));
            }
            return DashboardPrepareHandlerOperation::createRejection(
                *rejection);
        }();
    if (!operation) {
        startFallbackLocked(
            responseCatalog_->internalFailure(),
            std::move(operation).error());
        return;
    }

    if (!armDeadlineLocked(
            WindowsDashboardDeadlineKind::HandlerExecution,
            prepareDeadline)) {
        closeLocked(std::nullopt);
        return;
    }
    state_.store(
        DashboardConnectionLifecycleState::Preparing,
        std::memory_order_release);
    std::unique_ptr<IDashboardHandlerOperation> baseOperation{
        std::move(operation).value()};
    auto submitted = handlerExecutor_->trySubmit(
        std::move(baseOperation),
        std::move(context).value(),
        eventBridge_);
    if (!submitted) {
        startFallbackLocked(
            responseCatalog_->genericServiceUnavailable(),
            std::move(submitted).error());
    }
}

void DashboardConnectionState::Impl::handleEventCompletionLocked(
    DashboardConnectionEventReapResult completion) noexcept
{
    if (completion.disposition() ==
        DashboardConnectionEventReapDisposition::
            RetiredNotificationDrained) {
        maybeDrainLocked();
        return;
    }

    auto handler = completion.takeHandlerCompletion();
    if (handler.has_value()) {
        handleHandlerCompletionLocked(std::move(*handler));
    }
    if (completion.sseReady()) {
        observeSseReadyLocked();
    }
}

void DashboardConnectionState::Impl::handleHandlerCompletionLocked(
    DashboardHandlerCompletion completion) noexcept
{
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionLifecycleState::Closing ||
        current == DashboardConnectionLifecycleState::Drained) {
        return;
    }

    if (current == DashboardConnectionLifecycleState::Preparing) {
        if (completion.kind() !=
                DashboardHandlerCompletionKind::PreparedExchange ||
            completion.preparedResult() == nullptr) {
            closeLocked(integrityConnectionStateError(
                "The dashboard prepare state received the wrong handler completion kind."));
            return;
        }
        cancelDeadlineLocked();
        auto* result = completion.preparedResult();
        if (!*result) {
            startFallbackLocked(
                responseCatalog_->internalFailure(),
                std::move(*result).error());
            return;
        }
        handlePreparedExchangeLocked(std::move(*result).value());
        return;
    }

    if (current == DashboardConnectionLifecycleState::AwaitingPostDelivery) {
        if (completion.kind() !=
                DashboardHandlerCompletionKind::PostDelivery ||
            completion.postDeliveryResult() == nullptr) {
            closeLocked(integrityConnectionStateError(
                "The dashboard post-delivery state received the wrong handler completion kind."));
            return;
        }
        cancelDeadlineLocked();
        auto* result = completion.postDeliveryResult();
        if (!*result) {
            closeLocked(std::move(*result).error());
        } else {
            closeLocked(std::nullopt);
        }
        return;
    }

    closeLocked(integrityConnectionStateError(
        "A dashboard handler completion arrived outside a handler-owned state."));
}

void DashboardConnectionState::Impl::handlePreparedExchangeLocked(
    Dashboard::DashboardPreparedExchange exchange) noexcept
{
    fallbackBytes_.reset();
    sseFrameBytes_.reset();
    sendOffset_ = 0U;
    postDeliveryAction_ = Dashboard::DashboardPostDeliveryAction::None;

    if (exchange.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Complete) {
        auto* complete = exchange.completeExchange();
        if (complete == nullptr) {
            startFallbackLocked(
                responseCatalog_->internalFailure(),
                integrityConnectionStateError(
                    "A complete dashboard exchange did not expose its owned response."));
            return;
        }

        postDeliveryAction_ = complete->takePostDeliveryAction();
        if (postDeliveryAction_ !=
            Dashboard::DashboardPostDeliveryAction::None) {
            auto reserved = handlerExecutor_->tryReservePostDelivery();
            if (!reserved) {
                postDeliveryAction_ =
                    Dashboard::DashboardPostDeliveryAction::None;
                startFallbackLocked(
                    responseCatalog_->genericServiceUnavailable(),
                    std::move(reserved).error());
                return;
            }
            postDeliveryReservation_.emplace(
                std::move(reserved).value());
        }

        preparedExchange_.emplace(std::move(exchange));
        state_.store(
            DashboardConnectionLifecycleState::SendingComplete,
            std::memory_order_release);
        if (!armDeadlineLocked(
                WindowsDashboardDeadlineKind::SocketLifetime,
                socketLifetimeDeadline_) ||
            !issueCurrentSendLocked()) {
            closeLocked(std::nullopt);
        }
        return;
    }

    if (exchange.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::ServerSentEvents) {
        auto* sse = exchange.sseExchange();
        if (sse == nullptr || sse->subscription() == nullptr) {
            startFallbackLocked(
                responseCatalog_->streamUnavailable(),
                integrityConnectionStateError(
                    "An SSE dashboard exchange did not expose its subscription."));
            return;
        }

        auto convertedAdmission = admissionLease_.convertToSse();
        if (!convertedAdmission) {
            startFallbackLocked(
                responseCatalog_->streamUnavailable(),
                std::move(convertedAdmission).error());
            return;
        }

        preparedExchange_.emplace(std::move(exchange));
        sseBootstrapSegment_ = 0U;
        sseLifetimeDeadline_ = runtimeServices_->monotonicNow() +
            DashboardConnectionState::ServerSentEventsLifetime;
        nextSseDelivery_ = runtimeServices_->monotonicNow();
        state_.store(
            DashboardConnectionLifecycleState::SendingSseBootstrap,
            std::memory_order_release);
        if (!armDeadlineLocked(
                WindowsDashboardDeadlineKind::ServerSentEventsLifetime,
                sseLifetimeDeadline_)) {
            closeLocked(std::nullopt);
            return;
        }

        // attachReadySink may synchronously publish one bounded event. The
        // fatal callback uses a nonblocking latch when this mutex is held.
        preparedExchange_->sseExchange()->subscription()->attachReadySink(
            eventBridge_);
        consumePendingFatalLocked();
        if (state_.load(std::memory_order_relaxed) ==
                DashboardConnectionLifecycleState::SendingSseBootstrap &&
            !issueCurrentSendLocked()) {
            closeLocked(std::nullopt);
        }
        return;
    }

    startFallbackLocked(
        responseCatalog_->internalFailure(),
        integrityConnectionStateError(
            "The handler returned an empty dashboard exchange."));
}

void DashboardConnectionState::Impl::startFallbackLocked(
    DashboardConnectionResponseCatalog::ImmutableBytes bytes,
    Domain::Error error) noexcept
{
    retainFailureLocked(std::move(error));
    postDeliveryAction_ = Dashboard::DashboardPostDeliveryAction::None;
    if (postDeliveryReservation_.has_value()) {
        postDeliveryReservation_->release();
        postDeliveryReservation_.reset();
    }
    preparedExchange_.reset();
    sseFrameBytes_.reset();
    fallbackBytes_ = std::move(bytes);
    sendOffset_ = 0U;
    cancelDeadlineLocked();
    if (fallbackBytes_ == nullptr || fallbackBytes_->empty() ||
        runtimeServices_->monotonicNow() >= socketLifetimeDeadline_) {
        closeLocked(std::nullopt);
        return;
    }
    state_.store(
        DashboardConnectionLifecycleState::SendingComplete,
        std::memory_order_release);
    if (!armDeadlineLocked(
            WindowsDashboardDeadlineKind::SocketLifetime,
            socketLifetimeDeadline_) ||
        !issueCurrentSendLocked()) {
        closeLocked(std::nullopt);
    }
}

std::span<const std::byte>
DashboardConnectionState::Impl::currentSendBytesLocked() const noexcept
{
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionLifecycleState::SendingComplete) {
        if (fallbackBytes_ != nullptr) {
            return {*fallbackBytes_};
        }
        if (preparedExchange_.has_value()) {
            const auto* complete = preparedExchange_->completeExchange();
            if (complete != nullptr) {
                return {complete->encodedResponse().bytes()};
            }
        }
        return {};
    }
    if (current ==
        DashboardConnectionLifecycleState::SendingSseBootstrap) {
        if (!preparedExchange_.has_value() ||
            preparedExchange_->sseExchange() == nullptr) {
            return {};
        }
        const auto* sse = preparedExchange_->sseExchange();
        return sseBootstrapSegment_ == 0U
            ? std::span<const std::byte>{sse->encodedHead().bytes()}
            : std::span<const std::byte>{sse->connectedCommentBytes()};
    }
    if (current == DashboardConnectionLifecycleState::SendingSseFrame &&
        sseFrameBytes_ != nullptr) {
        return {*sseFrameBytes_};
    }
    return {};
}

bool DashboardConnectionState::Impl::issueCurrentSendLocked() noexcept
{
    const auto bytes = currentSendBytesLocked();
    if (bytes.empty() || sendOffset_ >= bytes.size()) {
        retainFailureLocked(integrityConnectionStateError(
            "A dashboard send state did not own a nonempty remaining byte range."));
        return false;
    }
    auto issued = socket_->issueSend(bytes.subspan(sendOffset_));
    if (!issued) {
        retainFailureLocked(std::move(issued).error());
        return false;
    }
    return true;
}

void DashboardConnectionState::Impl::handleSendCompletionLocked(
    const DWORD transferredBytes) noexcept
{
    const auto bytes = currentSendBytesLocked();
    if (bytes.empty() || transferredBytes == 0U ||
        static_cast<std::size_t>(transferredBytes) >
            bytes.size() - sendOffset_) {
        closeLocked(integrityConnectionStateError(
            "A dashboard send completion exceeded its remaining owned bytes."));
        return;
    }
    sendOffset_ += static_cast<std::size_t>(transferredBytes);
    if (sendOffset_ < bytes.size()) {
        if (!issueCurrentSendLocked()) {
            closeLocked(std::nullopt);
        }
        return;
    }

    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionLifecycleState::SendingComplete) {
        finishCompleteSendLocked();
    } else if (
        current ==
        DashboardConnectionLifecycleState::SendingSseBootstrap) {
        finishSseBootstrapSegmentLocked();
    } else if (
        current == DashboardConnectionLifecycleState::SendingSseFrame) {
        finishSseFrameLocked();
    } else {
        closeLocked(integrityConnectionStateError(
            "A dashboard send completed after its delivery state changed."));
    }
}

void DashboardConnectionState::Impl::finishCompleteSendLocked() noexcept
{
    sendOffset_ = 0U;
    fallbackBytes_.reset();
    if (postDeliveryAction_ ==
        Dashboard::DashboardPostDeliveryAction::None) {
        closeLocked(std::nullopt);
        return;
    }
    if (!postDeliveryReservation_.has_value() ||
        !postDeliveryReservation_->valid()) {
        closeLocked(integrityConnectionStateError(
            "A delivered dashboard action lost its reserved post-delivery capacity."));
        return;
    }

    auto operation = DashboardPostDeliveryHandlerOperation::create(
        application_, postDeliveryAction_);
    if (!operation) {
        closeLocked(std::move(operation).error());
        return;
    }
    const auto deadline = runtimeServices_->monotonicNow() +
        DashboardConnectionRuntimeServices::MaximumOperationLifetime;
    auto context = runtimeServices_->createOperationContext(
        deadline, postDeliveryStop_.get_token());
    if (!context) {
        closeLocked(std::move(context).error());
        return;
    }

    cancelDeadlineLocked();
    if (!armDeadlineLocked(
            WindowsDashboardDeadlineKind::HandlerExecution, deadline)) {
        closeLocked(std::nullopt);
        return;
    }
    static_cast<void>(socket_->shutdownBoth());
    state_.store(
        DashboardConnectionLifecycleState::AwaitingPostDelivery,
        std::memory_order_release);
    std::unique_ptr<IDashboardHandlerOperation> baseOperation{
        std::move(operation).value()};
    auto reservation = std::move(*postDeliveryReservation_);
    postDeliveryReservation_.reset();
    auto submitted = std::move(reservation).trySubmit(
        std::move(baseOperation),
        std::move(context).value(),
        eventBridge_);
    postDeliveryAction_ = Dashboard::DashboardPostDeliveryAction::None;
    if (!submitted) {
        closeLocked(std::move(submitted).error());
    }
}

void DashboardConnectionState::Impl::finishSseBootstrapSegmentLocked()
    noexcept
{
    sendOffset_ = 0U;
    if (sseBootstrapSegment_ == 0U) {
        sseBootstrapSegment_ = 1U;
        if (!issueCurrentSendLocked()) {
            closeLocked(std::nullopt);
        }
        return;
    }

    state_.store(
        DashboardConnectionLifecycleState::SseIdle,
        std::memory_order_release);
    if (preparedExchange_.has_value() &&
        preparedExchange_->sseExchange() != nullptr &&
        preparedExchange_->sseExchange()->subscription() != nullptr &&
        preparedExchange_->sseExchange()->subscription()->pendingCount() !=
            0U) {
        sseReadyPending_ = true;
    }
    maybeStartSseFrameLocked();
}

void DashboardConnectionState::Impl::finishSseFrameLocked() noexcept
{
    sendOffset_ = 0U;
    sseFrameBytes_.reset();
    const auto* subscription = preparedExchange_.has_value() &&
            preparedExchange_->sseExchange() != nullptr
        ? preparedExchange_->sseExchange()->subscription()
        : nullptr;
    if (subscription == nullptr) {
        closeLocked(integrityConnectionStateError(
            "A live dashboard stream lost its subscription after delivery."));
        return;
    }
    const auto interval = std::chrono::duration_cast<
        Domain::MonotonicTimePoint::duration>(
        std::chrono::duration<double>{1.0 / subscription->deliveryHz()});
    nextSseDelivery_ = runtimeServices_->monotonicNow() + interval;
    state_.store(
        DashboardConnectionLifecycleState::SseIdle,
        std::memory_order_release);
    if (subscription->pendingCount() != 0U) {
        sseReadyPending_ = true;
    }
    maybeStartSseFrameLocked();
}

void DashboardConnectionState::Impl::observeSseReadyLocked() noexcept
{
    const auto current = state_.load(std::memory_order_relaxed);
    if (current !=
            DashboardConnectionLifecycleState::SendingSseBootstrap &&
        current != DashboardConnectionLifecycleState::SseIdle &&
        current != DashboardConnectionLifecycleState::SendingSseFrame) {
        if (current != DashboardConnectionLifecycleState::Closing &&
            current != DashboardConnectionLifecycleState::Drained) {
            closeLocked(integrityConnectionStateError(
                "An SSE-ready event reached a non-stream dashboard connection."));
        }
        return;
    }
    sseReadyPending_ = true;
    if (current == DashboardConnectionLifecycleState::SseIdle) {
        maybeStartSseFrameLocked();
    }
}

void DashboardConnectionState::Impl::maybeStartSseFrameLocked() noexcept
{
    if (state_.load(std::memory_order_relaxed) !=
        DashboardConnectionLifecycleState::SseIdle) {
        return;
    }
    const auto now = runtimeServices_->monotonicNow();
    if (now >= sseLifetimeDeadline_) {
        closeLocked(deadlineConnectionStateError(
            "The dashboard SSE absolute lifetime expired."));
        return;
    }
    if (!sseReadyPending_) {
        armSseWaitLocked();
        return;
    }
    if (now < nextSseDelivery_) {
        armSseWaitLocked();
        return;
    }

    auto* subscription = preparedExchange_.has_value() &&
            preparedExchange_->sseExchange() != nullptr
        ? preparedExchange_->sseExchange()->subscription()
        : nullptr;
    if (subscription == nullptr) {
        closeLocked(integrityConnectionStateError(
            "A live dashboard stream did not retain its subscription."));
        return;
    }
    sseReadyPending_ = false;
    auto frame = subscription->takeLatest();
    if (frame == nullptr) {
        armSseWaitLocked();
        return;
    }
    sseFrameBytes_ = sseCursor_.select(frame);
    if (sseFrameBytes_ == nullptr || sseFrameBytes_->empty()) {
        closeLocked(integrityConnectionStateError(
            "A dashboard SSE frame did not expose its selected immutable bytes."));
        return;
    }
    sendOffset_ = 0U;
    state_.store(
        DashboardConnectionLifecycleState::SendingSseFrame,
        std::memory_order_release);
    if (!armDeadlineLocked(
            WindowsDashboardDeadlineKind::ServerSentEventsLifetime,
            sseLifetimeDeadline_) ||
        !issueCurrentSendLocked()) {
        closeLocked(std::nullopt);
    }
}

void DashboardConnectionState::Impl::armSseWaitLocked() noexcept
{
    const auto now = runtimeServices_->monotonicNow();
    const bool deliveryPending = sseReadyPending_ &&
        nextSseDelivery_ > now &&
        nextSseDelivery_ < sseLifetimeDeadline_;
    const auto kind = deliveryPending
        ? WindowsDashboardDeadlineKind::ServerSentEventsDelivery
        : WindowsDashboardDeadlineKind::ServerSentEventsLifetime;
    const auto deadline = deliveryPending
        ? nextSseDelivery_
        : sseLifetimeDeadline_;
    if (!armDeadlineLocked(kind, deadline)) {
        closeLocked(std::nullopt);
    }
}

void DashboardConnectionState::Impl::dispatchDeadline(
    const WindowsDashboardDeadline deadline) noexcept
{
    ExternalNotificationDrain drain{*this};
    const std::scoped_lock lock{mutex_};
    consumePendingFatalLocked();
    if (state_.load(std::memory_order_relaxed) ==
            DashboardConnectionLifecycleState::Closing ||
        state_.load(std::memory_order_relaxed) ==
            DashboardConnectionLifecycleState::Drained) {
        maybeDrainLocked();
        return;
    }
    if (!currentDeadline_.has_value() ||
        deadline.registrationId != identity_.registrationId ||
        deadline.armSequence != currentDeadline_->armSequence ||
        deadline.kind != currentDeadline_->kind) {
        return;
    }
    currentDeadline_.reset();

    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionLifecycleState::Closing ||
        current == DashboardConnectionLifecycleState::Drained) {
        maybeDrainLocked();
        return;
    }
    if (deadline.kind ==
            WindowsDashboardDeadlineKind::ServerSentEventsDelivery &&
        current == DashboardConnectionLifecycleState::SseIdle) {
        maybeStartSseFrameLocked();
        return;
    }
    if (deadline.kind == WindowsDashboardDeadlineKind::HeaderIngress) {
        closeLocked(deadlineConnectionStateError(
            "The dashboard request header exceeded its two-second ingress deadline."));
    } else if (
        deadline.kind == WindowsDashboardDeadlineKind::HandlerExecution) {
        prepareStop_.request_stop();
        postDeliveryStop_.request_stop();
        closeLocked(deadlineConnectionStateError(
            "A dashboard handler exceeded its bounded execution deadline."));
    } else if (
        deadline.kind ==
            WindowsDashboardDeadlineKind::ServerSentEventsLifetime) {
        closeLocked(deadlineConnectionStateError(
            "The dashboard SSE connection reached its one-hour absolute lifetime."));
    } else {
        closeLocked(deadlineConnectionStateError(
            "The dashboard connection reached its absolute socket lifetime."));
    }
}

void DashboardConnectionState::Impl::beginShutdown() noexcept
{
    ExternalNotificationDrain drain{*this};
    const std::scoped_lock lock{mutex_};
    closeLocked(std::nullopt);
}

void DashboardConnectionState::Impl::closeLocked(
    std::optional<Domain::Error> failure) noexcept
{
    if (failure.has_value()) {
        retainFailureLocked(std::move(*failure));
    }
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == DashboardConnectionLifecycleState::Drained) {
        return;
    }
    shutdownRequested_ = true;
    state_.store(
        DashboardConnectionLifecycleState::Closing,
        std::memory_order_release);
    prepareStop_.request_stop();
    postDeliveryStop_.request_stop();
    cancelDeadlineLocked();
    postDeliveryAction_ = Dashboard::DashboardPostDeliveryAction::None;
    if (postDeliveryReservation_.has_value()) {
        postDeliveryReservation_->release();
        postDeliveryReservation_.reset();
    }
    if (preparedExchange_.has_value() &&
        preparedExchange_->sseExchange() != nullptr) {
        preparedExchange_->sseExchange()->close();
    }
    static_cast<void>(socket_->shutdownBoth());
    if (socket_->state() != DashboardConnectionSocketState::Idle) {
        auto cancelled = socket_->requestCancellation();
        if (!cancelled) {
            retainFailureLocked(std::move(cancelled).error());
        }
    }
    if (eventBridge_ != nullptr) {
        eventBridge_->shutdown();
    }
    maybeDrainLocked();
}

void DashboardConnectionState::Impl::maybeDrainLocked() noexcept
{
    if (state_.load(std::memory_order_relaxed) !=
        DashboardConnectionLifecycleState::Closing) {
        return;
    }
    const bool socketDrained =
        socket_->state() == DashboardConnectionSocketState::Idle;
    const bool eventDrained = eventBridge_ == nullptr ||
        eventBridge_->snapshot().fullyDrained();
    if (!socketDrained || !eventDrained || currentDeadline_.has_value()) {
        return;
    }
    preparedExchange_.reset();
    fallbackBytes_.reset();
    sseFrameBytes_.reset();
    admissionLease_.release();
    state_.store(
        DashboardConnectionLifecycleState::Drained,
        std::memory_order_release);
    if (drainObserverBound_ && !drainNotificationSent_) {
        drainNotificationSent_ = true;
        drainNotificationPending_.store(true, std::memory_order_release);
    }
}

void DashboardConnectionState::Impl::retainFailureLocked(
    Domain::Error error) noexcept
{
    if (firstFailure_.has_value()) {
        return;
    }
    try {
        firstFailure_.emplace(std::move(error));
    } catch (...) {
        try {
            firstFailure_.emplace(connectionStateError(
                Domain::ErrorCodes::InternalFailure,
                "The first dashboard connection failure could not be retained."));
        } catch (...) {
        }
    }
}

void DashboardConnectionState::Impl::eventFatal(
    const DashboardConnectionEventFatalNotification) noexcept
{
    eventFatalPending_.store(true, std::memory_order_release);
    drainExternalNotifications();
}

void DashboardConnectionState::Impl::consumePendingFatalLocked() noexcept
{
    if (!eventFatalPending_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    closeLocked(integrityConnectionStateError(
        "The bounded dashboard connection event bridge failed fatally."));
}

void DashboardConnectionState::Impl::drainExternalNotifications() noexcept
{
    if (!eventFatalPending_.load(std::memory_order_acquire) &&
        !drainNotificationPending_.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        // Every public mutex owner installs ExternalNotificationDrain before
        // acquiring
        // the mutex. Its destructor runs after unlock and performs another
        // nonblocking attempt, so contention cannot strand this latch. The
        // production bridge emits at most one notification from
        // retainFatalFailureLocked, so no second bridge fatal can arrive after
        // consumePendingFatalLocked clears that edge while this drain owns the
        // mutex.
        return;
    }
    consumePendingFatalLocked();

    std::shared_ptr<IDashboardConnectionDrainObserver> observer;
    if (drainNotificationPending_.exchange(
            false, std::memory_order_acq_rel)) {
        observer = drainObserver_.lock();
    }
    lock.unlock();
    if (observer != nullptr) {
        observer->connectionMayHaveDrained(
            identity_.completionKey,
            identity_.registrationId,
            generationId_);
    }
}

DashboardConnectionStateSnapshot DashboardConnectionState::Impl::snapshot()
    const noexcept
{
    ExternalNotificationDrain drain{*const_cast<Impl*>(this)};
    try {
        const std::scoped_lock lock{mutex_};
        const auto event = eventBridge_ == nullptr
            ? std::size_t{0U}
            : eventBridge_->snapshot().postedOperationCount();
        return DashboardConnectionStateSnapshot{
            state_.load(std::memory_order_relaxed),
            identity_.registrationId,
            generationId_,
            identity_.completionKey,
            socket_->state() != DashboardConnectionSocketState::Idle,
            event != 0U,
            currentDeadline_.has_value(),
            shutdownRequested_,
            firstFailure_.has_value() ||
                eventFatalPending_.load(std::memory_order_relaxed)};
    } catch (...) {
        return DashboardConnectionStateSnapshot{
            state_.load(std::memory_order_acquire),
            identity_.registrationId,
            generationId_,
            identity_.completionKey,
            true,
            true,
            true,
            true,
            true};
    }
}

std::optional<Domain::Error> DashboardConnectionState::Impl::fullFailure()
    const
{
    ExternalNotificationDrain drain{*const_cast<Impl*>(this)};
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

Domain::Result<std::shared_ptr<DashboardConnectionState>>
DashboardConnectionState::create(
    const std::uint64_t generationId,
    const DashboardConnectionRuntimeIdentity identity,
    const Domain::MonotonicTimePoint admittedAt,
    DashboardAdmissionController::Lease admissionLease,
    std::unique_ptr<IDashboardConnectionIo> socket,
    DashboardIocpWorkerKernel& kernel,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    WindowsDashboardHandlerExecutor& handlerExecutor,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    DashboardConnectionResponseCatalog& responseCatalog) noexcept
{
    using CreateResult =
        Domain::Result<std::shared_ptr<DashboardConnectionState>>;
    if (generationId == 0U || identity.registrationId == 0U ||
        identity.completionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue ||
        !admissionLease.ownsAdmission() ||
        admissionLease.kind() != DashboardAdmissionKind::Short ||
        socket == nullptr ||
        !(socket->completionKey() == identity.completionKey) ||
        application == nullptr) {
        return CreateResult::failure(invalidConnectionStateError(
            "A dashboard connection state requires nonzero identities, matching socket ownership, and complete services."));
    }

    try {
        auto owner = std::shared_ptr<DashboardConnectionState>{
            new DashboardConnectionState{}};
        owner->implementation_ = std::make_unique<Impl>(
            generationId,
            identity,
            admittedAt,
            std::move(admissionLease),
            std::move(socket),
            deadlineScheduler,
            handlerExecutor,
            runtimeServices,
            std::move(application),
            responseCatalog);
        std::weak_ptr<IDashboardConnectionEventFatalSink> fatalSink{owner};
        auto bridge = DashboardConnectionEventBridge::create(
            kernel,
            identity.completionKey,
            identity.registrationId,
            std::move(fatalSink));
        if (!bridge) {
            return CreateResult::failure(std::move(bridge).error());
        }
        owner->implementation_->setEventBridge(
            std::move(bridge).value());
        return CreateResult::success(std::move(owner));
    } catch (...) {
        return CreateResult::failure(connectionStateError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection state could not be allocated."));
    }
}

DashboardConnectionState::~DashboardConnectionState() noexcept = default;

DashboardIoCompletionKey DashboardConnectionState::completionKey()
    const noexcept
{
    return implementation_->completionKey();
}

std::uint64_t DashboardConnectionState::registrationId() const noexcept
{
    return implementation_->registrationId();
}

std::uint64_t DashboardConnectionState::generationId() const noexcept
{
    return implementation_->generationId();
}

Domain::Result<void> DashboardConnectionState::bindDrainObserver(
    std::weak_ptr<IDashboardConnectionDrainObserver> observer) noexcept
{
    return implementation_->bindDrainObserver(std::move(observer));
}

Domain::Result<void> DashboardConnectionState::start() noexcept
{
    return implementation_->start();
}

void DashboardConnectionState::dispatchIocp(
    const DWORD transferredBytes,
    OVERLAPPED* const operation,
    const DWORD nativeError) noexcept
{
    implementation_->dispatchIocp(
        transferredBytes, operation, nativeError);
}

void DashboardConnectionState::dispatchDeadline(
    const WindowsDashboardDeadline deadline) noexcept
{
    implementation_->dispatchDeadline(deadline);
}

void DashboardConnectionState::beginShutdown() noexcept
{
    implementation_->beginShutdown();
}

bool DashboardConnectionState::isDrained() const noexcept
{
    return implementation_->isDrained();
}

DashboardConnectionStateSnapshot DashboardConnectionState::snapshot()
    const noexcept
{
    return implementation_->snapshot();
}

std::optional<Domain::Error> DashboardConnectionState::fullFailure() const
{
    return implementation_->fullFailure();
}

void DashboardConnectionState::fatal(
    const DashboardConnectionEventFatalNotification notification) noexcept
{
    implementation_->eventFatal(notification);
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
