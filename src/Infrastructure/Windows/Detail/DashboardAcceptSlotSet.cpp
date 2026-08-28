#include "DashboardAcceptSlotSet.h"

#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class DashboardAcceptSlotSetIdentity final {};

namespace {

[[nodiscard]] Domain::Error invalidOwnerError() noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "A dashboard accept-slot set requires a live listener and native API dependencies.");
}

[[nodiscard]] Domain::Error invalidCompletionError(
    std::string message) noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] DashboardAcceptLifecycleFailure classifyLifecycleFailure(
    const Domain::Error& error) noexcept
{
    DashboardAcceptLifecycleFailureKind kind{
        DashboardAcceptLifecycleFailureKind::Other};
    if (error.code == Domain::ErrorCodes::Cancelled) {
        kind = DashboardAcceptLifecycleFailureKind::Cancelled;
    } else if (error.code == Domain::ErrorCodes::TransportClosed) {
        kind = DashboardAcceptLifecycleFailureKind::TransportClosed;
    } else if (error.code == Domain::ErrorCodes::Unauthorized) {
        kind = DashboardAcceptLifecycleFailureKind::Unauthorized;
    } else if (error.code == Domain::ErrorCodes::LimitExceeded) {
        kind = DashboardAcceptLifecycleFailureKind::LimitExceeded;
    } else if (error.code == Domain::ErrorCodes::InternalFailure) {
        kind = DashboardAcceptLifecycleFailureKind::InternalFailure;
    }
    return DashboardAcceptLifecycleFailure{kind, error.retryable};
}

} // namespace

DashboardAcceptResumeToken::DashboardAcceptResumeToken(
    std::shared_ptr<const DashboardAcceptSlotSetIdentity> identity,
    const std::size_t slotIndex,
    const std::uint64_t sequence) noexcept
    : identity_{std::move(identity)},
      slotIndex_{slotIndex},
      sequence_{sequence},
      valid_{true}
{
}

DashboardAcceptResumeToken::DashboardAcceptResumeToken(
    DashboardAcceptResumeToken&& other) noexcept
    : identity_{std::move(other.identity_)},
      slotIndex_{other.slotIndex_},
      sequence_{other.sequence_},
      valid_{other.valid_}
{
    other.invalidate();
}

DashboardAcceptResumeToken& DashboardAcceptResumeToken::operator=(
    DashboardAcceptResumeToken&& other) noexcept
{
    if (this != std::addressof(other)) {
        if (valid_) {
            std::terminate();
        }
        identity_ = std::move(other.identity_);
        slotIndex_ = other.slotIndex_;
        sequence_ = other.sequence_;
        valid_ = other.valid_;
        other.invalidate();
    }
    return *this;
}

DashboardAcceptResumeToken::~DashboardAcceptResumeToken() noexcept
{
    if (valid_) {
        std::terminate();
    }
}

void DashboardAcceptResumeToken::invalidate() noexcept
{
    identity_.reset();
    slotIndex_ = 0U;
    sequence_ = 0U;
    valid_ = false;
}

DashboardAcceptReapResult::DashboardAcceptReapResult(
    const DashboardAcceptReapDisposition disposition,
    std::optional<DashboardAcceptedConnection> acceptedConnection,
    std::optional<DashboardAcceptResumeToken> resumeToken,
    std::optional<Domain::Error> acceptFailure,
    std::optional<Domain::Error> reissueFailure,
    std::optional<DashboardAcceptLifecycleFailure>
        cancellationFailure) noexcept
    : disposition_{disposition},
      acceptedConnection_{std::move(acceptedConnection)},
      resumeToken_{std::move(resumeToken)},
      acceptFailure_{std::move(acceptFailure)},
      reissueFailure_{std::move(reissueFailure)},
      cancellationFailure_{std::move(cancellationFailure)}
{
}

std::optional<DashboardAcceptedConnection>
DashboardAcceptReapResult::takeAcceptedConnection() noexcept
{
    auto connection = std::move(acceptedConnection_);
    acceptedConnection_.reset();
    return connection;
}

std::optional<DashboardAcceptResumeToken>
DashboardAcceptReapResult::takeResumeToken() noexcept
{
    auto token = std::move(resumeToken_);
    resumeToken_.reset();
    return token;
}

DashboardAcceptSlotSetSnapshot::DashboardAcceptSlotSetSnapshot(
    const std::size_t issuedCount,
    const std::size_t idleCount,
    const std::size_t pausedCount,
    const std::size_t awaitingReturnCount,
    const std::size_t cancellationRequestedCount,
    const std::size_t drainedCount,
    const bool startAttempted,
    const bool listenerAssociated,
    const bool admissionOpen,
    std::optional<DashboardAcceptLifecycleFailure>
        lifecycleFailure) noexcept
    : issuedCount_{issuedCount},
      idleCount_{idleCount},
      pausedCount_{pausedCount},
      awaitingReturnCount_{awaitingReturnCount},
      cancellationRequestedCount_{cancellationRequestedCount},
      drainedCount_{drainedCount},
      startAttempted_{startAttempted},
      listenerAssociated_{listenerAssociated},
      admissionOpen_{admissionOpen},
      lifecycleFailure_{std::move(lifecycleFailure)}
{
}

bool DashboardAcceptSlotSetSnapshot::fullyDrained() const noexcept
{
    return drainedCount_ == DashboardAcceptSlotSet::SlotCount;
}

DashboardAcceptSlotSet::DashboardAcceptSlotSet(
    DashboardWinsockRuntime& runtime,
    DashboardListeningSocket listener,
    std::unique_ptr<DashboardWinsockExtensions> extensions,
    SlotOwners slots,
    std::shared_ptr<const DashboardAcceptSlotSetIdentity> identity,
    const DashboardIoCompletionKey completionKey) noexcept
    : runtime_{std::addressof(runtime)},
      listener_{std::move(listener)},
      extensions_{std::move(extensions)},
      slots_{std::move(slots)},
      identity_{std::move(identity)},
      completionKey_{completionKey}
{
    lifecycles_.fill(SlotLifecycle::Idle);
}

DashboardAcceptSlotSet::~DashboardAcceptSlotSet() noexcept
{
    const std::scoped_lock lock{mutex_};
    for (std::size_t index{}; index < SlotCount; ++index) {
        if (lifecycles_[index] == SlotLifecycle::Issued ||
            lifecycles_[index] == SlotLifecycle::Paused ||
            lifecycles_[index] == SlotLifecycle::AwaitingReturnClosed ||
            lifecycles_[index] == SlotLifecycle::CancellationRequested ||
            slots_[index]->state() != DashboardAcceptSlotState::Idle) {
            std::terminate();
        }
    }
}

Domain::Result<std::unique_ptr<DashboardAcceptSlotSet>>
DashboardAcceptSlotSet::create(
    DashboardWinsockRuntime& runtime,
    DashboardListeningSocket listener,
    const DashboardIoCompletionKey completionKey) noexcept
{
    try {
        return create(
            runtime,
            std::move(listener),
            completionKey,
            std::make_shared<DashboardWinsockExtensionSystemApi>(),
            std::make_shared<DashboardAcceptSlotSystemApi>());
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardAcceptSlotSet>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate accept-slot set dependencies."));
    }
}

Domain::Result<std::unique_ptr<DashboardAcceptSlotSet>>
DashboardAcceptSlotSet::create(
    DashboardWinsockRuntime& runtime,
    DashboardListeningSocket listener,
    const DashboardIoCompletionKey completionKey,
    std::shared_ptr<IDashboardWinsockExtensionApi> extensionApi,
    std::shared_ptr<IDashboardAcceptSlotApi> slotApi) noexcept
{
    if (listener.borrowedNativeSocket() == INVALID_SOCKET ||
        extensionApi == nullptr || slotApi == nullptr ||
        completionKey.value() ==
            DashboardIocpWorkerKernel::ShutdownKeyValue) {
        return Domain::Result<
            std::unique_ptr<DashboardAcceptSlotSet>>::failure(
            invalidOwnerError());
    }

    auto extensions = DashboardWinsockExtensions::discover(
        listener.borrowedNativeSocket(), std::move(extensionApi));
    if (!extensions) {
        return Domain::Result<
            std::unique_ptr<DashboardAcceptSlotSet>>::failure(
            std::move(extensions).error());
    }

    SlotOwners slots{};
    for (auto& slot : slots) {
        auto created = DashboardAcceptSlot::create(slotApi);
        if (!created) {
            return Domain::Result<
                std::unique_ptr<DashboardAcceptSlotSet>>::failure(
                std::move(created).error());
        }
        slot = std::move(created).value();
    }

    try {
        auto identity =
            std::make_shared<const DashboardAcceptSlotSetIdentity>();
        return Domain::Result<
            std::unique_ptr<DashboardAcceptSlotSet>>::success(
            std::unique_ptr<DashboardAcceptSlotSet>{
                new DashboardAcceptSlotSet{
                    runtime,
                    std::move(listener),
                    std::move(extensions).value(),
                    std::move(slots),
                    std::move(identity),
                    completionKey}});
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardAcceptSlotSet>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its four accept slots."));
    }
}

Domain::Result<DashboardAcceptIssueDisposition>
DashboardAcceptSlotSet::issueLocked(const std::size_t index) noexcept
{
    if (index >= SlotCount ||
        lifecycles_[index] != SlotLifecycle::Idle ||
        slots_[index]->state() != DashboardAcceptSlotState::Idle) {
        return Domain::Result<DashboardAcceptIssueDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A dashboard accept slot was not idle when issue was requested."));
    }

    auto issued = slots_[index]->issue(*runtime_, *extensions_, listener_);
    if (issued) {
        lifecycles_[index] = SlotLifecycle::Issued;
    }
    return issued;
}

Domain::Result<void> DashboardAcceptSlotSet::start(
    DashboardIocpWorkerKernel& kernel) noexcept
{
    const std::scoped_lock lock{mutex_};
    if (startAttempted_ || terminalClosed_) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "A dashboard accept-slot set may be started exactly once and cannot restart after admission closes."));
    }
    startAttempted_ = true;

    auto associated = kernel.associateSocket(
        listener_.borrowedNativeSocket(), completionKey_);
    if (!associated) {
        auto error = std::move(associated).error();
        static_cast<void>(closeAdmissionAndRequestCancellationLocked());
        return Domain::Result<void>::failure(std::move(error));
    }

    listenerAssociated_ = true;
    admissionOpen_ = true;
    for (std::size_t index{}; index < SlotCount; ++index) {
        auto issued = issueLocked(index);
        if (!issued) {
            auto error = std::move(issued).error();
            static_cast<void>(closeAdmissionAndRequestCancellationLocked());
            return Domain::Result<void>::failure(std::move(error));
        }
    }
    return Domain::Result<void>::success();
}

std::optional<DashboardAcceptLifecycleFailure>
DashboardAcceptSlotSet::closeAdmissionAndRequestCancellationLocked() noexcept
{
    admissionOpen_ = false;
    terminalClosed_ = true;
    std::optional<DashboardAcceptLifecycleFailure> firstFailure;

    for (std::size_t index{}; index < SlotCount; ++index) {
        switch (lifecycles_[index]) {
        case SlotLifecycle::Idle:
            lifecycles_[index] = SlotLifecycle::Drained;
            break;
        case SlotLifecycle::Paused:
            lifecycles_[index] = SlotLifecycle::AwaitingReturnClosed;
            break;
        case SlotLifecycle::AwaitingReturnClosed:
            break;
        case SlotLifecycle::Issued: {
            auto cancellation = slots_[index]->requestCancellation();
            if (cancellation) {
                lifecycles_[index] = SlotLifecycle::CancellationRequested;
            } else {
                auto failure = retainLifecycleFailureLocked(
                    std::move(cancellation).error());
                if (!firstFailure.has_value()) {
                    firstFailure.emplace(failure);
                }
            }
            break;
        }
        case SlotLifecycle::CancellationRequested:
        case SlotLifecycle::Drained:
            break;
        }
    }

    return firstFailure;
}

DashboardAcceptLifecycleFailure
DashboardAcceptSlotSet::retainLifecycleFailureLocked(
    Domain::Error error) noexcept
{
    const auto failure = classifyLifecycleFailure(error);
    if (!firstLifecycleFailure_.has_value()) {
        firstLifecycleFailure_.emplace(std::move(error));
    }
    if (!firstLifecycleFailureSnapshot_.has_value()) {
        firstLifecycleFailureSnapshot_.emplace(failure);
    }
    return failure;
}

std::optional<DashboardAcceptLifecycleFailure>
DashboardAcceptSlotSet::closeAdmissionAndRequestCancellation() noexcept
{
    const std::scoped_lock lock{mutex_};
    return closeAdmissionAndRequestCancellationLocked();
}

Domain::Result<DashboardAcceptResumeDisposition>
DashboardAcceptSlotSet::resume(
    DashboardAcceptResumeToken&& token) noexcept
{
    const std::scoped_lock lock{mutex_};
    if (!token.valid_) {
        return Domain::Result<DashboardAcceptResumeDisposition>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "A dashboard accept resume token is one-shot and was already returned."));
    }
    if (token.identity_.get() != identity_.get()) {
        return Domain::Result<DashboardAcceptResumeDisposition>::failure(
            invalidCompletionError(
                "A dashboard accept resume token belongs to another listener generation."));
    }
    if (token.slotIndex_ >= SlotCount ||
        token.sequence_ == 0U ||
        token.sequence_ != resumeSequences_[token.slotIndex_]) {
        token.invalidate();
        return Domain::Result<DashboardAcceptResumeDisposition>::failure(
            invalidCompletionError(
                "A dashboard accept resume token is stale or malformed."));
    }

    const std::size_t index = token.slotIndex_;
    if (!admissionOpen_) {
        if (lifecycles_[index] != SlotLifecycle::AwaitingReturnClosed) {
            token.invalidate();
            return Domain::Result<DashboardAcceptResumeDisposition>::failure(
                invalidCompletionError(
                    "A closed dashboard listener received a token for a slot that was not awaiting return."));
        }
        token.invalidate();
        lifecycles_[index] = SlotLifecycle::Drained;
        return Domain::Result<DashboardAcceptResumeDisposition>::success(
            DashboardAcceptResumeDisposition::ReturnedAfterClose);
    }

    if (lifecycles_[index] != SlotLifecycle::Paused) {
        token.invalidate();
        return Domain::Result<DashboardAcceptResumeDisposition>::failure(
            invalidCompletionError(
                "A dashboard accept resume token did not match a paused slot."));
    }

    token.invalidate();
    lifecycles_[index] = SlotLifecycle::Idle;
    auto issued = issueLocked(index);
    if (issued) {
        return Domain::Result<DashboardAcceptResumeDisposition>::success(
            DashboardAcceptResumeDisposition::Reissued);
    }

    auto error = std::move(issued).error();
    lifecycles_[index] = SlotLifecycle::Drained;
    static_cast<void>(closeAdmissionAndRequestCancellationLocked());
    return Domain::Result<DashboardAcceptResumeDisposition>::failure(
        std::move(error));
}

Domain::Result<std::size_t> DashboardAcceptSlotSet::findIssuedSlotLocked(
    OVERLAPPED* const operation) const noexcept
{
    if (operation == nullptr) {
        return Domain::Result<std::size_t>::failure(
            invalidCompletionError(
                "A dashboard accept completion carried a null OVERLAPPED pointer."));
    }

    for (std::size_t index{}; index < SlotCount; ++index) {
        if (slots_[index]->borrowedOperation() == operation) {
            if (lifecycles_[index] != SlotLifecycle::Issued &&
                lifecycles_[index] !=
                    SlotLifecycle::CancellationRequested) {
                return Domain::Result<std::size_t>::failure(
                    invalidCompletionError(
                        "A dashboard accept completion targeted a slot without an issued operation."));
            }
            return Domain::Result<std::size_t>::success(index);
        }
    }

    return Domain::Result<std::size_t>::failure(
        invalidCompletionError(
            "A dashboard accept completion did not match any of the four owned OVERLAPPED values."));
}

DashboardAcceptReapResult DashboardAcceptSlotSet::finishFailureLocked(
    const std::size_t index,
    Domain::Error acceptFailure) noexcept
{
    lifecycles_[index] = SlotLifecycle::Drained;

    const bool perClientRetry =
        acceptFailure.retryable &&
        acceptFailure.code == Domain::ErrorCodes::TransportClosed;
    if (admissionOpen_ && perClientRetry) {
        lifecycles_[index] = SlotLifecycle::Idle;
        auto reissued = issueLocked(index);
        if (reissued) {
            return DashboardAcceptReapResult{
                DashboardAcceptReapDisposition::FailureReissued,
                std::nullopt,
                std::nullopt,
                std::optional<Domain::Error>{std::move(acceptFailure)},
                std::nullopt,
                std::nullopt};
        }

        auto reissueFailure = std::move(reissued).error();
        lifecycles_[index] = SlotLifecycle::Drained;
        auto cancellationFailure =
            closeAdmissionAndRequestCancellationLocked();
        return DashboardAcceptReapResult{
            DashboardAcceptReapDisposition::FailureDrained,
            std::nullopt,
            std::nullopt,
            std::optional<Domain::Error>{std::move(acceptFailure)},
            std::optional<Domain::Error>{std::move(reissueFailure)},
            std::move(cancellationFailure)};
    }

    auto cancellationFailure = admissionOpen_
        ? closeAdmissionAndRequestCancellationLocked()
        : std::optional<DashboardAcceptLifecycleFailure>{};
    return DashboardAcceptReapResult{
        DashboardAcceptReapDisposition::FailureDrained,
        std::nullopt,
        std::nullopt,
        std::optional<Domain::Error>{std::move(acceptFailure)},
        std::nullopt,
        std::move(cancellationFailure)};
}

Domain::Result<DashboardAcceptReapResult> DashboardAcceptSlotSet::reap(
    const DashboardIoCompletionKey completionKey,
    const DWORD transferredBytes,
    OVERLAPPED* const operation,
    const DWORD nativeError) noexcept
{
    const std::scoped_lock lock{mutex_};
    if (!(completionKey == completionKey_)) {
        return Domain::Result<DashboardAcceptReapResult>::failure(
            invalidCompletionError(
                "A dashboard accept completion carried the wrong listener-generation key."));
    }
    auto found = findIssuedSlotLocked(operation);
    if (!found) {
        return Domain::Result<DashboardAcceptReapResult>::failure(
            std::move(found).error());
    }
    const std::size_t index = std::move(found).value();

    if (transferredBytes != 0U) {
        // The packet has already been removed from the kernel queue. Consume
        // the exact owned operation before reporting corruption, otherwise no
        // later packet exists that could make this slot destructible.
        static_cast<void>(
            slots_[index]->reapFailed(operation, ERROR_INVALID_DATA));
        lifecycles_[index] = SlotLifecycle::Drained;
        static_cast<void>(closeAdmissionAndRequestCancellationLocked());
        return Domain::Result<DashboardAcceptReapResult>::failure(
            invalidCompletionError(
                "A zero-receive AcceptEx completion reported transferred bytes."));
    }

    if (nativeError != ERROR_SUCCESS) {
        auto failed = slots_[index]->reapFailed(operation, nativeError);
        if (failed) {
            lifecycles_[index] = SlotLifecycle::Drained;
            static_cast<void>(closeAdmissionAndRequestCancellationLocked());
            return Domain::Result<DashboardAcceptReapResult>::failure(
                invalidCompletionError(
                    "A failed AcceptEx packet did not produce a typed slot failure."));
        }

        auto result = finishFailureLocked(
            index, std::move(failed).error());
        return Domain::Result<DashboardAcceptReapResult>::success(
            std::move(result));
    }

    auto accepted = slots_[index]->reapSuccessful(*extensions_, operation);
    if (!accepted) {
        auto result = finishFailureLocked(
            index, std::move(accepted).error());
        return Domain::Result<DashboardAcceptReapResult>::success(
            std::move(result));
    }

    std::optional<DashboardAcceptedConnection> connection{
        std::in_place, std::move(accepted).value()};
    lifecycles_[index] = SlotLifecycle::Drained;
    if (!admissionOpen_) {
        return Domain::Result<DashboardAcceptReapResult>::success(
            DashboardAcceptReapResult{
                DashboardAcceptReapDisposition::AcceptedAndDrained,
                std::move(connection),
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt});
    }

    if (resumeSequences_[index] ==
        (std::numeric_limits<std::uint64_t>::max)()) {
        lifecycles_[index] = SlotLifecycle::Drained;
        auto cancellationFailure =
            closeAdmissionAndRequestCancellationLocked();
        auto sequenceFailure = invalidCompletionError(
            "A dashboard accept resume sequence was exhausted.");
        return Domain::Result<DashboardAcceptReapResult>::success(
            DashboardAcceptReapResult{
                DashboardAcceptReapDisposition::AcceptedAndDrained,
                std::move(connection),
                std::nullopt,
                std::nullopt,
                std::optional<Domain::Error>{std::move(sequenceFailure)},
                std::move(cancellationFailure)});
    }

    ++resumeSequences_[index];
    lifecycles_[index] = SlotLifecycle::Paused;
    std::optional<DashboardAcceptResumeToken> token{
        DashboardAcceptResumeToken{
            identity_, index, resumeSequences_[index]}};
    return Domain::Result<DashboardAcceptReapResult>::success(
        DashboardAcceptReapResult{
            DashboardAcceptReapDisposition::AcceptedAndPaused,
            std::move(connection),
            std::move(token),
            std::nullopt,
            std::nullopt,
            std::nullopt});
}

DashboardAcceptSlotSetSnapshot DashboardAcceptSlotSet::snapshotLocked()
    const noexcept
{
    std::size_t issued{};
    std::size_t idle{};
    std::size_t paused{};
    std::size_t awaitingReturn{};
    std::size_t cancellationRequested{};
    std::size_t drained{};

    for (const auto lifecycle : lifecycles_) {
        switch (lifecycle) {
        case SlotLifecycle::Idle:
            ++idle;
            break;
        case SlotLifecycle::Issued:
            ++issued;
            break;
        case SlotLifecycle::Paused:
            ++paused;
            break;
        case SlotLifecycle::AwaitingReturnClosed:
            ++awaitingReturn;
            break;
        case SlotLifecycle::CancellationRequested:
            ++cancellationRequested;
            break;
        case SlotLifecycle::Drained:
            ++drained;
            break;
        }
    }

    return DashboardAcceptSlotSetSnapshot{
        issued,
        idle,
        paused,
        awaitingReturn,
        cancellationRequested,
        drained,
        startAttempted_,
        listenerAssociated_,
        admissionOpen_,
        firstLifecycleFailureSnapshot_};
}

DashboardAcceptSlotSetSnapshot DashboardAcceptSlotSet::snapshot() const noexcept
{
    const std::scoped_lock lock{mutex_};
    return snapshotLocked();
}

std::optional<Domain::Error>
DashboardAcceptSlotSet::fullLifecycleFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstLifecycleFailure_;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
