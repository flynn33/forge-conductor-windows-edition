#include "Infrastructure/Windows/Detail/DashboardConnectionEventBridge.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error eventError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard connection event failed and its diagnostic could not be retained.",
            retryable);
    }
}

[[nodiscard]] Domain::Error integrityError(std::string message) noexcept
{
    return eventError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] DashboardConnectionEventFailure classifyFailure(
    const Domain::Error& error) noexcept
{
    DashboardConnectionEventFailureKind kind{
        DashboardConnectionEventFailureKind::Other};
    if (error.code == Domain::ErrorCodes::InvalidRequest) {
        kind = DashboardConnectionEventFailureKind::InvalidRequest;
    } else if (error.code == Domain::ErrorCodes::Conflict) {
        kind = DashboardConnectionEventFailureKind::Conflict;
    } else if (error.code == Domain::ErrorCodes::LimitExceeded) {
        kind = DashboardConnectionEventFailureKind::LimitExceeded;
    } else if (error.code == Domain::ErrorCodes::TransportClosed) {
        kind = DashboardConnectionEventFailureKind::TransportClosed;
    } else if (error.code == Domain::ErrorCodes::IntegrityFailure) {
        kind = DashboardConnectionEventFailureKind::IntegrityFailure;
    } else if (error.code == Domain::ErrorCodes::InternalFailure) {
        kind = DashboardConnectionEventFailureKind::InternalFailure;
    }
    return DashboardConnectionEventFailure{kind, error.retryable};
}

void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

} // namespace

DashboardConnectionEventReapResult::DashboardConnectionEventReapResult(
    const DashboardConnectionEventReapDisposition disposition,
    std::optional<DashboardHandlerCompletion> handlerCompletion,
    const bool sseReady) noexcept
    : disposition_{disposition},
      handlerCompletion_{std::move(handlerCompletion)},
      sseReady_{sseReady}
{
}

std::optional<DashboardHandlerCompletion>
DashboardConnectionEventReapResult::takeHandlerCompletion() noexcept
{
    auto result = std::move(handlerCompletion_);
    handlerCompletion_.reset();
    return result;
}

DashboardConnectionEventSnapshot::DashboardConnectionEventSnapshot(
    const std::uint64_t ownerRegistrationId,
    const std::size_t postedOperationCount,
    const std::size_t handlerPayloadCount,
    const bool sseReadyLatched,
    const bool tombstoneAwaitingReap,
    const std::uint64_t successfulPostCount,
    const std::uint64_t coalescedSignalCount,
    const std::uint64_t deliveredPayloadCount,
    const std::uint64_t drainedTombstoneCount,
    const bool shutdown,
    const bool fatal,
    std::optional<DashboardConnectionEventFailure> failure) noexcept
    : ownerRegistrationId_{ownerRegistrationId},
      postedOperationCount_{postedOperationCount},
      handlerPayloadCount_{handlerPayloadCount},
      sseReadyLatched_{sseReadyLatched},
      tombstoneAwaitingReap_{tombstoneAwaitingReap},
      successfulPostCount_{successfulPostCount},
      coalescedSignalCount_{coalescedSignalCount},
      deliveredPayloadCount_{deliveredPayloadCount},
      drainedTombstoneCount_{drainedTombstoneCount},
      shutdown_{shutdown},
      fatal_{fatal},
      failure_{std::move(failure)}
{
}

DashboardConnectionEventBridge::DashboardConnectionEventBridge(
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t ownerRegistrationId,
    std::weak_ptr<IDashboardConnectionEventFatalSink> fatalSink) noexcept
    : kernel_{std::addressof(kernel)},
      completionKey_{completionKey},
      ownerRegistrationId_{ownerRegistrationId},
      fatalSink_{std::move(fatalSink)}
{
}

DashboardConnectionEventBridge::~DashboardConnectionEventBridge() noexcept
{
    shutdown();
    const std::scoped_lock lock{mutex_};
    if (posted_) {
        // The kernel still owns this object's OVERLAPPED address. Releasing
        // it would turn a bounded tombstone into a use-after-free.
        std::terminate();
    }
}

Domain::Result<std::shared_ptr<DashboardConnectionEventBridge>>
DashboardConnectionEventBridge::create(
    DashboardIocpWorkerKernel& kernel,
    const DashboardIoCompletionKey completionKey,
    const std::uint64_t ownerRegistrationId,
    std::weak_ptr<IDashboardConnectionEventFatalSink> fatalSink) noexcept
{
    using BridgeResult =
        Domain::Result<std::shared_ptr<DashboardConnectionEventBridge>>;
    if (completionKey.value() ==
        DashboardIocpWorkerKernel::ShutdownKeyValue) {
        return BridgeResult::failure(eventError(
            Domain::ErrorCodes::InvalidRequest,
            "A dashboard connection event bridge cannot use the reserved shutdown key."));
    }
    if (ownerRegistrationId == 0U) {
        return BridgeResult::failure(eventError(
            Domain::ErrorCodes::InvalidRequest,
            "A dashboard connection event bridge requires a nonzero owner registration identifier."));
    }

    try {
        return BridgeResult::success(
            std::shared_ptr<DashboardConnectionEventBridge>{
                new DashboardConnectionEventBridge{
                    kernel,
                    completionKey,
                    ownerRegistrationId,
                    std::move(fatalSink)}});
    } catch (...) {
        return BridgeResult::failure(eventError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection event bridge could not be allocated."));
    }
}

bool DashboardConnectionEventBridge::tryPost(
    DashboardHandlerCompletion completion) noexcept
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    std::optional<DashboardConnectionEventFatalNotification> notification;
    std::optional<DashboardHandlerCompletion> discarded;
    bool accepted{};
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                return false;
            }
            if (handlerCompletion_.has_value()) {
                notification = retainFatalFailureLocked(integrityError(
                    "A dashboard connection event bridge received a second handler completion before the first was reaped."),
                    discarded);
            } else {
                handlerCompletion_.emplace(std::move(completion));
                accepted = posted_
                    ? true
                    : postLocked(notification, discarded);
            }
        }
        discarded.reset();
        notifyFatal(std::move(notification));
        return accepted;
    } catch (...) {
        try {
            {
                const std::scoped_lock lock{mutex_};
                notification = retainFatalFailureLocked(eventError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard handler completion could not enter its bounded IOCP bridge safely."),
                    discarded);
            }
            discarded.reset();
            notifyFatal(std::move(notification));
        } catch (...) {
        }
        return false;
    }
}

void DashboardConnectionEventBridge::signal() noexcept
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    std::optional<DashboardConnectionEventFatalNotification> notification;
    std::optional<DashboardHandlerCompletion> discarded;
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (shutdown_) {
                return;
            }
            if (sseReadyLatched_) {
                incrementSaturating(coalescedSignalCount_);
                return;
            }

            sseReadyLatched_ = true;
            if (posted_) {
                incrementSaturating(coalescedSignalCount_);
                return;
            }
            static_cast<void>(postLocked(notification, discarded));
        }
        discarded.reset();
        notifyFatal(std::move(notification));
    } catch (...) {
        try {
            {
                const std::scoped_lock lock{mutex_};
                notification = retainFatalFailureLocked(eventError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard SSE-ready signal could not enter its bounded IOCP bridge safely."),
                    discarded);
            }
            discarded.reset();
            notifyFatal(std::move(notification));
        } catch (...) {
        }
    }
}

bool DashboardConnectionEventBridge::postLocked(
    std::optional<DashboardConnectionEventFatalNotification>&
        notification,
    std::optional<DashboardHandlerCompletion>& discarded) noexcept
{
    operation_ = {};
    posted_ = true;
    tombstone_ = false;
    auto posted = kernel_->postAdmitted(
        0U, completionKey_, std::addressof(operation_));
    if (posted) {
        incrementSaturating(successfulPostCount_);
        return true;
    }

    posted_ = false;
    operation_ = {};
    if (handlerCompletion_.has_value()) {
        discarded.emplace(std::move(*handlerCompletion_));
        handlerCompletion_.reset();
    }
    sseReadyLatched_ = false;
    notification = retainFatalFailureLocked(
        std::move(posted).error(), discarded);
    return false;
}

Domain::Result<DashboardConnectionEventReapResult>
DashboardConnectionEventBridge::reap(
    const DashboardIoCompletionKey completionKey,
    const DWORD transferredBytes,
    OVERLAPPED* const operation,
    const DWORD nativeError) noexcept
{
    using ReapResult =
        Domain::Result<DashboardConnectionEventReapResult>;
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();

    // A dispatcher may ask the wrong connection owner first. Never mutate this
    // owner unless the dequeued packet carries its one exact stable address.
    if (operation != std::addressof(operation_)) {
        return ReapResult::failure(integrityError(
            "A dashboard connection event completion used a foreign operation address."));
    }

    std::optional<DashboardConnectionEventFatalNotification> notification;
    std::optional<DashboardConnectionEventReapResult> success;
    std::optional<Domain::Error> failure;
    std::optional<DashboardHandlerCompletion> discarded;
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!posted_) {
                auto error = integrityError(
                    "A dashboard connection event completion repeated an operation that was not posted.");
                notification = retainFatalFailureLocked(
                    Domain::Error{error}, discarded);
                failure.emplace(std::move(error));
            } else {
                const bool wasTombstone = tombstone_;

                // This exact packet has left the kernel queue. Vacate its
                // obligation before validating shape so corruption cannot
                // strand storage that will never receive another completion.
                posted_ = false;
                tombstone_ = false;
                operation_ = {};

                if (!(completionKey == completionKey_) ||
                    transferredBytes != 0U ||
                    nativeError != ERROR_SUCCESS) {
                    if (handlerCompletion_.has_value()) {
                        discarded.emplace(std::move(*handlerCompletion_));
                        handlerCompletion_.reset();
                    }
                    sseReadyLatched_ = false;
                    auto error = integrityError(
                        "A dashboard connection event completion violated its exact key, zero-byte, or success contract.");
                    notification =
                        retainFatalFailureLocked(
                            Domain::Error{error}, discarded);
                    failure.emplace(std::move(error));
                } else if (wasTombstone) {
                    if (handlerCompletion_.has_value() || sseReadyLatched_) {
                        if (handlerCompletion_.has_value()) {
                            discarded.emplace(
                                std::move(*handlerCompletion_));
                            handlerCompletion_.reset();
                        }
                        sseReadyLatched_ = false;
                        auto error = integrityError(
                            "A retired dashboard connection event retained a deliverable payload while being reaped.");
                        notification =
                            retainFatalFailureLocked(
                                Domain::Error{error}, discarded);
                        failure.emplace(std::move(error));
                    } else {
                        incrementSaturating(drainedTombstoneCount_);
                        success.emplace(DashboardConnectionEventReapResult{
                            DashboardConnectionEventReapDisposition::
                                RetiredNotificationDrained,
                            std::nullopt,
                            false});
                    }
                } else {
                    auto handler = std::move(handlerCompletion_);
                    handlerCompletion_.reset();
                    const bool ready = sseReadyLatched_;
                    sseReadyLatched_ = false;
                    incrementSaturating(deliveredPayloadCount_);
                    success.emplace(DashboardConnectionEventReapResult{
                        DashboardConnectionEventReapDisposition::
                            PayloadDelivered,
                        std::move(handler),
                        ready});
                }
            }
        }

        discarded.reset();
        notifyFatal(std::move(notification));
        if (failure.has_value()) {
            return ReapResult::failure(std::move(*failure));
        }
        return ReapResult::success(std::move(*success));
    } catch (...) {
        try {
            {
                const std::scoped_lock lock{mutex_};
                notification = retainFatalFailureLocked(eventError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard connection event completion could not be reaped safely."),
                    discarded);
            }
            discarded.reset();
            notifyFatal(std::move(notification));
        } catch (...) {
        }
        return ReapResult::failure(eventError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection event completion could not be reaped safely."));
    }
}

void DashboardConnectionEventBridge::transitionToShutdownLocked(
    std::optional<DashboardHandlerCompletion>& discarded) noexcept
{
    shutdown_ = true;
    if (handlerCompletion_.has_value()) {
        discarded.emplace(std::move(*handlerCompletion_));
        handlerCompletion_.reset();
    }
    sseReadyLatched_ = false;
    if (posted_) {
        tombstone_ = true;
    } else {
        tombstone_ = false;
        operation_ = {};
    }
}

std::optional<DashboardConnectionEventFatalNotification>
DashboardConnectionEventBridge::retainFatalFailureLocked(
    Domain::Error error,
    std::optional<DashboardHandlerCompletion>& discarded) noexcept
{
    transitionToShutdownLocked(discarded);
    fatal_ = true;
    if (firstFailureSnapshot_.has_value()) {
        return std::nullopt;
    }

    firstFailureSnapshot_.emplace(classifyFailure(error));
    firstFailure_.emplace(std::move(error));
    return DashboardConnectionEventFatalNotification{
        ownerRegistrationId_, *firstFailureSnapshot_};
}

void DashboardConnectionEventBridge::notifyFatal(
    std::optional<DashboardConnectionEventFatalNotification>
        notification) noexcept
{
    if (!notification.has_value()) {
        return;
    }
    if (auto sink = fatalSink_.lock(); sink != nullptr) {
        sink->fatal(*notification);
    }
}

DashboardConnectionEventSnapshot
DashboardConnectionEventBridge::snapshotLocked() const noexcept
{
    return DashboardConnectionEventSnapshot{
        ownerRegistrationId_,
        posted_ ? 1U : 0U,
        handlerCompletion_.has_value() ? 1U : 0U,
        sseReadyLatched_,
        tombstone_,
        successfulPostCount_,
        coalescedSignalCount_,
        deliveredPayloadCount_,
        drainedTombstoneCount_,
        shutdown_,
        fatal_,
        firstFailureSnapshot_};
}

DashboardConnectionEventSnapshot DashboardConnectionEventBridge::snapshot()
    const noexcept
{
    try {
        const std::scoped_lock lock{mutex_};
        return snapshotLocked();
    } catch (...) {
        return DashboardConnectionEventSnapshot{
            ownerRegistrationId_,
            0U,
            0U,
            false,
            false,
            0U,
            0U,
            0U,
            0U,
            true,
            true,
            DashboardConnectionEventFailure{
                DashboardConnectionEventFailureKind::InternalFailure,
                false}};
    }
}

std::optional<Domain::Error>
DashboardConnectionEventBridge::fullFailure() const
{
    const std::scoped_lock lock{mutex_};
    return firstFailure_;
}

void DashboardConnectionEventBridge::shutdown() noexcept
{
    std::optional<DashboardHandlerCompletion> discarded;
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (!shutdown_) {
                transitionToShutdownLocked(discarded);
            }
        }
        discarded.reset();
    } catch (...) {
        std::terminate();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
