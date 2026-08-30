#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

[[nodiscard]] bool isDefinedKind(
    const DashboardHandlerCompletionKind kind) noexcept
{
    return kind == DashboardHandlerCompletionKind::PreparedExchange ||
           kind == DashboardHandlerCompletionKind::PostDelivery;
}

[[nodiscard]] Domain::Error executorError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] DashboardHandlerCompletion failureCompletion(
    const DashboardHandlerCompletionKind kind,
    Domain::Error error)
{
    if (kind == DashboardHandlerCompletionKind::PreparedExchange) {
        return DashboardHandlerCompletion::prepared(
            DashboardHandlerCompletion::PreparedResult::failure(
                std::move(error)));
    }
    return DashboardHandlerCompletion::postDelivery(
        DashboardHandlerCompletion::PostDeliveryResult::failure(
            std::move(error)));
}

[[nodiscard]] Domain::Error cancellationError()
{
    return executorError(
        Domain::ErrorCodes::Cancelled,
        "The dashboard handler task was cancelled.");
}

[[nodiscard]] Domain::Error deadlineError()
{
    return executorError(
        Domain::ErrorCodes::DeadlineExceeded,
        "The dashboard handler task exceeded its deadline.");
}

[[nodiscard]] Domain::Error exceptionError()
{
    return executorError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard handler task failed at its execution boundary.");
}

[[nodiscard]] Domain::Error mismatchedCompletionError()
{
    return executorError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard handler task returned the wrong completion kind.");
}

[[nodiscard]] Domain::Error invalidReservationError()
{
    return executorError(
        Domain::ErrorCodes::Conflict,
        "The dashboard post-delivery reservation no longer owns capacity.");
}

class StopRelay final {
public:
    explicit StopRelay(std::stop_source source) noexcept
        : source_{std::move(source)}
    {
    }

    void operator()() noexcept { static_cast<void>(source_.request_stop()); }

private:
    std::stop_source source_;
};

class OwnedHandlerTask final {
public:
    OwnedHandlerTask(
        std::unique_ptr<IDashboardHandlerOperation> operation,
        Domain::OperationContext context,
        std::weak_ptr<IDashboardHandlerCompletionSink> completionSink)
        : kind_{operation->completionKind()},
          operation_{std::move(operation)},
          cancellationSource_{},
          externalCancellation_{
              context.cancellation,
              StopRelay{cancellationSource_}},
          context_{
              std::move(context.operationId),
              context.deadline,
              cancellationSource_.get_token(),
              std::move(context.correlationId)},
          completionSink_{std::move(completionSink)}
    {
    }

    OwnedHandlerTask(const OwnedHandlerTask&) = delete;
    OwnedHandlerTask& operator=(const OwnedHandlerTask&) = delete;
    OwnedHandlerTask(OwnedHandlerTask&&) = delete;
    OwnedHandlerTask& operator=(OwnedHandlerTask&&) = delete;

    [[nodiscard]] DashboardHandlerCompletionKind kind() const noexcept
    {
        return kind_;
    }

    void requestCancellation() noexcept
    {
        static_cast<void>(cancellationSource_.request_stop());
    }

    void executeAndPost() noexcept
    {
        try {
            auto completion = execute();
            if (auto sink = completionSink_.lock(); sink != nullptr) {
                static_cast<void>(sink->tryPost(std::move(completion)));
            }
        } catch (...) {
            try {
                auto completion = failureCompletion(kind_, exceptionError());
                if (auto sink = completionSink_.lock(); sink != nullptr) {
                    static_cast<void>(sink->tryPost(std::move(completion)));
                }
            } catch (...) {
                // Allocation exhaustion can prevent even a typed error value
                // from being built. The noexcept worker boundary remains intact.
            }
        }
    }

private:
    [[nodiscard]] DashboardHandlerCompletion execute()
    {
        if (context_.isCancellationRequested()) {
            return failureCompletion(kind_, cancellationError());
        }
        if (context_.isExpired(std::chrono::steady_clock::now())) {
            return failureCompletion(kind_, deadlineError());
        }

        auto completion = operation_->execute(context_);
        if (completion.kind() != kind_) {
            return failureCompletion(kind_, mismatchedCompletionError());
        }
        return completion;
    }

    const DashboardHandlerCompletionKind kind_;
    std::unique_ptr<IDashboardHandlerOperation> operation_;
    std::stop_source cancellationSource_;
    std::stop_callback<StopRelay> externalCancellation_;
    Domain::OperationContext context_;
    std::weak_ptr<IDashboardHandlerCompletionSink> completionSink_;
};

} // namespace

DashboardHandlerCompletion::DashboardHandlerCompletion(
    PreparedResult result) noexcept
    : value_{std::in_place_index<0>, std::move(result)}
{
}

DashboardHandlerCompletion::DashboardHandlerCompletion(
    PostDeliveryResult result) noexcept
    : value_{std::in_place_index<1>, std::move(result)}
{
}

DashboardHandlerCompletion DashboardHandlerCompletion::prepared(
    PreparedResult result) noexcept
{
    return DashboardHandlerCompletion{std::move(result)};
}

DashboardHandlerCompletion DashboardHandlerCompletion::postDelivery(
    PostDeliveryResult result) noexcept
{
    return DashboardHandlerCompletion{std::move(result)};
}

DashboardHandlerCompletionKind DashboardHandlerCompletion::kind()
    const noexcept
{
    return value_.index() == 0U
        ? DashboardHandlerCompletionKind::PreparedExchange
        : DashboardHandlerCompletionKind::PostDelivery;
}

DashboardHandlerCompletion::PreparedResult*
DashboardHandlerCompletion::preparedResult() noexcept
{
    return std::get_if<0>(&value_);
}

const DashboardHandlerCompletion::PreparedResult*
DashboardHandlerCompletion::preparedResult() const noexcept
{
    return std::get_if<0>(&value_);
}

DashboardHandlerCompletion::PostDeliveryResult*
DashboardHandlerCompletion::postDeliveryResult() noexcept
{
    return std::get_if<1>(&value_);
}

const DashboardHandlerCompletion::PostDeliveryResult*
DashboardHandlerCompletion::postDeliveryResult() const noexcept
{
    return std::get_if<1>(&value_);
}

class WindowsDashboardHandlerExecutor::Impl final
    : public std::enable_shared_from_this<
          WindowsDashboardHandlerExecutor::Impl> {
public:
    Impl() = default;

    ~Impl() noexcept { shutdownForDestruction(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept
    {
        try {
            for (std::size_t index{};
                 index < WindowsDashboardHandlerExecutor::WorkerCount;
                 ++index) {
                const auto owner = shared_from_this();
                workers_[index] = std::thread{
                    [owner, index]() noexcept { owner->workerMain(index); }};
                {
                    const std::lock_guard lock{stateMutex_};
                    ++startedWorkerCount_;
                }
            }
            return Domain::Result<void>::success();
        } catch (...) {
            shutdown();
            return Domain::Result<void>::failure(executorError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard handler workers could not be started."));
        }
    }

    [[nodiscard]] Domain::Result<void> trySubmit(
        std::unique_ptr<IDashboardHandlerOperation> operation,
        Domain::OperationContext context,
        std::weak_ptr<IDashboardHandlerCompletionSink> completionSink) noexcept
    {
        try {
            if (operation == nullptr || completionSink.expired()) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A dashboard handler task requires an operation and a "
                    "live completion sink."));
            }
            const auto kind = operation->completionKind();
            if (!isDefinedKind(kind)) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard handler task kind is undefined."));
            }
            if (context.isCancellationRequested()) {
                return Domain::Result<void>::failure(cancellationError());
            }
            if (context.isExpired(std::chrono::steady_clock::now())) {
                return Domain::Result<void>::failure(deadlineError());
            }

            auto task = std::make_unique<OwnedHandlerTask>(
                std::move(operation),
                std::move(context),
                std::move(completionSink));

            {
                const std::lock_guard lock{stateMutex_};
                if (stopping_) {
                    return Domain::Result<void>::failure(executorError(
                        Domain::ErrorCodes::TransportClosed,
                        "The dashboard handler executor is shutting down."));
                }
                if (task->kind() != kind) {
                    return Domain::Result<void>::failure(executorError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The dashboard handler task changed kind during "
                        "admission."));
                }
                if (pending_.size() + reservationCount_ >=
                    WindowsDashboardHandlerExecutor::QueueCapacity) {
                    return Domain::Result<void>::failure(executorError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The dashboard handler queue and reservations reached "
                        "their configured capacity.",
                        true));
                }
                pending_.push_back(std::move(task));
            }
            workAvailable_.notify_one();
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(executorError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard handler task could not be admitted safely."));
        }
    }

    [[nodiscard]] Domain::Result<
        WindowsDashboardHandlerExecutor::Reservation>
    tryReservePostDelivery() noexcept
    {
        try {
            WindowsDashboardHandlerExecutor::Reservation reservation;
            reservation.implementation_ = shared_from_this();
            {
                const std::lock_guard lock{stateMutex_};
                if (stopping_) {
                    return Domain::Result<
                        WindowsDashboardHandlerExecutor::Reservation>::failure(
                        executorError(
                            Domain::ErrorCodes::TransportClosed,
                            "The dashboard handler executor is shutting "
                            "down."));
                }
                if (pending_.size() + reservationCount_ >=
                    WindowsDashboardHandlerExecutor::QueueCapacity) {
                    return Domain::Result<
                        WindowsDashboardHandlerExecutor::Reservation>::failure(
                        executorError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The dashboard handler queue and reservations "
                            "reached their configured capacity.",
                            true));
                }
                ++reservationCount_;
                reservation.ownsCapacity_ = true;
            }
            return Domain::Result<
                WindowsDashboardHandlerExecutor::Reservation>::success(
                std::move(reservation));
        } catch (...) {
            return Domain::Result<
                WindowsDashboardHandlerExecutor::Reservation>::failure(
                executorError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard post-delivery capacity could not be "
                    "reserved safely."));
        }
    }

    [[nodiscard]] Domain::Result<void> trySubmitReservedPostDelivery(
        WindowsDashboardHandlerExecutor::Reservation& reservation,
        std::unique_ptr<IDashboardHandlerOperation> operation,
        Domain::OperationContext context,
        std::weak_ptr<IDashboardHandlerCompletionSink> completionSink) noexcept
    {
        try {
            if (!reservation.ownsCapacity_ ||
                reservation.implementation_.get() != this) {
                return Domain::Result<void>::failure(
                    invalidReservationError());
            }
            if (operation == nullptr || completionSink.expired()) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A reserved dashboard post-delivery task requires an "
                    "operation and a live completion sink."));
            }
            const auto kind = operation->completionKind();
            if (kind != DashboardHandlerCompletionKind::PostDelivery) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A dashboard post-delivery reservation accepts only a "
                    "PostDelivery operation."));
            }
            if (context.isCancellationRequested()) {
                return Domain::Result<void>::failure(cancellationError());
            }
            if (context.isExpired(std::chrono::steady_clock::now())) {
                return Domain::Result<void>::failure(deadlineError());
            }

            auto task = std::make_unique<OwnedHandlerTask>(
                std::move(operation),
                std::move(context),
                std::move(completionSink));

            {
                const std::lock_guard lock{stateMutex_};
                if (!reservation.ownsCapacity_ ||
                    reservation.implementation_.get() != this) {
                    return Domain::Result<void>::failure(
                        invalidReservationError());
                }
                if (stopping_) {
                    return Domain::Result<void>::failure(executorError(
                        Domain::ErrorCodes::TransportClosed,
                        "The dashboard handler executor is shutting down."));
                }
                if (task->kind() != kind) {
                    return Domain::Result<void>::failure(executorError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The reserved dashboard handler task changed kind "
                        "during admission."));
                }
                if (reservationCount_ == 0U ||
                    pending_.size() + reservationCount_ >
                        WindowsDashboardHandlerExecutor::QueueCapacity) {
                    return Domain::Result<void>::failure(executorError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The dashboard post-delivery reservation accounting "
                        "was inconsistent."));
                }
                pending_.push_back(std::move(task));
                --reservationCount_;
                reservation.ownsCapacity_ = false;
            }
            workAvailable_.notify_one();
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(executorError(
                Domain::ErrorCodes::InternalFailure,
                "The reserved dashboard post-delivery task could not be "
                "admitted safely."));
        }
    }

    void releaseReservation() noexcept
    {
        DrainNotification notification;
        try {
            {
                const std::lock_guard lock{stateMutex_};
                if (reservationCount_ == 0U) {
                    std::terminate();
                }
                --reservationCount_;
                notification = takeShutdownDrainNotificationLocked();
            }
            notifyShutdownDrainObserver(std::move(notification));
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool reservationValid() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return !stopping_;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return pending_.size();
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t reservationCount() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return reservationCount_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t activeCount() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return activeTaskCount_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] bool isShuttingDown() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return stopping_;
        } catch (...) {
            return true;
        }
    }

    [[nodiscard]] bool fullyDrained() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return fullyDrainedLocked();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] Domain::Result<void> bindShutdownDrainObserver(
        std::weak_ptr<IDashboardHandlerExecutorDrainObserver> observer)
        noexcept
    {
        try {
            auto pinned = observer.lock();
            if (pinned == nullptr) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard handler executor requires a live process "
                    "shutdown-drain observer."));
            }

            const std::lock_guard lock{stateMutex_};
            if (stopping_) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::TransportClosed,
                    "The dashboard handler executor is closed to process "
                    "shutdown-drain observer binding."));
            }
            if (shutdownDrainObserverEverBound_) {
                return Domain::Result<void>::failure(executorError(
                    Domain::ErrorCodes::Conflict,
                    "The dashboard handler executor process shutdown-drain "
                    "observer is already bound."));
            }
            shutdownDrainObserver_ = std::move(pinned);
            shutdownDrainObserverEverBound_ = true;
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(executorError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard handler executor process shutdown-drain "
                "observer could not be bound safely."));
        }
    }

    void beginShutdown() noexcept
    {
        static_cast<void>(beginShutdownAndDetectProcessObservation());
    }

    void shutdown() noexcept
    {
        try {
            const auto currentWorker = currentWorkerIndex();
            if (currentWorker.has_value()) {
                const bool processObserved =
                    beginShutdownAndDetectProcessObservation();
                if (processObserved) {
                    // Process-observed shutdown has one external lifecycle
                    // owner. Never detach a handler-worker thread handle or
                    // publish its drain edge from this context.
                    return;
                }

                std::unique_lock shutdownLock{
                    shutdownMutex_, std::try_to_lock};
                if (!shutdownLock.owns_lock()) {
                    // An external owner is already joining this worker. Let
                    // the callback return so that join can complete.
                    beginShutdown();
                    return;
                }
                joinStartedWorkers(currentWorker);
                return;
            }

            DrainNotification notification;
            {
                const std::lock_guard shutdownLock{shutdownMutex_};
                beginShutdown();
                joinStartedWorkers(std::nullopt);
                {
                    const std::lock_guard stateLock{stateMutex_};
                    workersJoined_ = true;
                    notification = takeShutdownDrainNotificationLocked();
                }
            }
            notifyShutdownDrainObserver(std::move(notification));
        } catch (...) {
            std::terminate();
        }
    }

private:
    struct DrainNotification final {
        std::shared_ptr<IDashboardHandlerExecutorDrainObserver> observer;
        bool missingObserver{};
    };

    struct WorkerThreadIdentity final {
        const Impl* owner{};
        std::size_t index{};
    };

    [[nodiscard]] bool fullyDrainedLocked() const noexcept
    {
        return stopping_ && pending_.empty() && activeTaskCount_ == 0U &&
               reservationCount_ == 0U && workersJoined_;
    }

    [[nodiscard]] DrainNotification takeShutdownDrainNotificationLocked()
        noexcept
    {
        DrainNotification notification;
        if (shutdownDrainNotificationSent_ ||
            !shutdownDrainObserverEverBound_ || !fullyDrainedLocked()) {
            return notification;
        }

        shutdownDrainNotificationSent_ = true;
        notification.observer = shutdownDrainObserver_.lock();
        notification.missingObserver = notification.observer == nullptr;
        return notification;
    }

    static void notifyShutdownDrainObserver(
        DrainNotification notification) noexcept
    {
        if (notification.observer != nullptr) {
            notification.observer->handlerExecutorMayHaveDrained();
        } else if (notification.missingObserver) {
            // Process composition is the strong lifetime owner. Losing that
            // owner before the exact edge is a structural violation.
            std::terminate();
        }
    }

    void shutdownForDestruction() noexcept
    {
        try {
            const auto currentWorker = currentWorkerIndex();
            if (!currentWorker.has_value()) {
                shutdown();
                return;
            }

            // Reaching implementation destruction on one of its own workers
            // proves that no external finalizer retained shutdown authority.
            // The current handle cannot be joined from this thread, so settle
            // every other literal handle and detach only this already-quiesced
            // one. Never claim or publish exact joined drainage here.
            static_cast<void>(beginShutdownAndDetectProcessObservation());
            const std::lock_guard shutdownLock{shutdownMutex_};
            joinStartedWorkers(currentWorker, true);
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool beginShutdownAndDetectProcessObservation() noexcept
    {
        try {
            bool processObserved{};
            {
                const std::lock_guard lock{stateMutex_};
                stopping_ = true;
                for (auto& task : pending_) {
                    task->requestCancellation();
                }
                for (auto* task : activeTasks_) {
                    if (task != nullptr) {
                        task->requestCancellation();
                    }
                }
                processObserved = shutdownDrainObserverEverBound_;
            }
            workAvailable_.notify_all();
            return processObserved;
        } catch (...) {
            std::terminate();
        }
    }

    void joinStartedWorkers(
        const std::optional<std::size_t> currentWorker,
        const bool currentWorkerAlreadyQuiesced = false) noexcept
    {
        try {
            {
                std::unique_lock lock{stateMutex_};
                const bool exited = workerStateChanged_.wait_for(
                    lock,
                    WindowsDashboardHandlerExecutor::ShutdownDrainTimeout,
                    [this,
                     currentWorker,
                     currentWorkerAlreadyQuiesced] {
                        const auto currentAllowance =
                            currentWorker.has_value() &&
                                !currentWorkerAlreadyQuiesced
                            ? 1U
                            : 0U;
                        return quiescedWorkerCount_ + currentAllowance ==
                               startedWorkerCount_;
                    });
                if (!exited) {
                    std::terminate();
                }
            }
            for (std::size_t index{}; index < workers_.size(); ++index) {
                auto& worker = workers_[index];
                if (!worker.joinable()) {
                    continue;
                }
                if (currentWorker.has_value() &&
                    index == currentWorker.value()) {
                    worker.detach();
                } else {
                    worker.join();
                }
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::optional<std::size_t> currentWorkerIndex()
        const noexcept
    {
        if (workerThreadIdentity_.owner == this &&
            workerThreadIdentity_.index < workers_.size()) {
            return workerThreadIdentity_.index;
        }
        try {
            const auto current = std::this_thread::get_id();
            const std::lock_guard lock{stateMutex_};
            for (std::size_t index{}; index < workerThreadIds_.size();
                 ++index) {
                if (workerThreadIds_[index] == current) {
                    return index;
                }
            }
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    void workerMain(const std::size_t workerIndex) noexcept
    {
        workerThreadIdentity_ = WorkerThreadIdentity{this, workerIndex};
        try {
            {
                const std::lock_guard lock{stateMutex_};
                workerThreadIds_[workerIndex] = std::this_thread::get_id();
            }

            for (;;) {
                std::unique_ptr<OwnedHandlerTask> task;
                {
                    std::unique_lock lock{stateMutex_};
                    workAvailable_.wait(lock, [this] {
                        return stopping_ || !pending_.empty();
                    });
                    if (pending_.empty()) {
                        if (stopping_) {
                            break;
                        }
                        continue;
                    }
                    task = std::move(pending_.front());
                    pending_.pop_front();
                    activeTasks_[workerIndex] = task.get();
                    ++activeTaskCount_;
                }

                task->executeAndPost();

                {
                    const std::lock_guard lock{stateMutex_};
                    activeTasks_[workerIndex] = nullptr;
                    if (activeTaskCount_ == 0U) {
                        std::terminate();
                    }
                    --activeTaskCount_;
                }
                workerStateChanged_.notify_all();
            }

            {
                const std::lock_guard lock{stateMutex_};
                workerThreadIds_[workerIndex] = std::thread::id{};
                ++quiescedWorkerCount_;
            }
            workerStateChanged_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    mutable std::mutex stateMutex_;
    std::mutex shutdownMutex_;
    std::condition_variable workAvailable_;
    std::condition_variable workerStateChanged_;
    std::deque<std::unique_ptr<OwnedHandlerTask>> pending_;
    std::array<
        OwnedHandlerTask*,
        WindowsDashboardHandlerExecutor::WorkerCount>
        activeTasks_{};
    std::array<
        std::thread,
        WindowsDashboardHandlerExecutor::WorkerCount>
        workers_{};
    std::array<
        std::thread::id,
        WindowsDashboardHandlerExecutor::WorkerCount>
        workerThreadIds_{};
    std::size_t startedWorkerCount_{};
    std::size_t quiescedWorkerCount_{};
    std::size_t activeTaskCount_{};
    std::size_t reservationCount_{};
    std::weak_ptr<IDashboardHandlerExecutorDrainObserver>
        shutdownDrainObserver_;
    bool stopping_{};
    bool workersJoined_{};
    bool shutdownDrainObserverEverBound_{};
    bool shutdownDrainNotificationSent_{};
    inline static thread_local WorkerThreadIdentity workerThreadIdentity_{};
};

WindowsDashboardHandlerExecutor::Reservation::~Reservation() noexcept
{
    release();
}

WindowsDashboardHandlerExecutor::Reservation::Reservation(
    Reservation&& other) noexcept
    : implementation_{std::move(other.implementation_)},
      ownsCapacity_{std::exchange(other.ownsCapacity_, false)}
{
}

WindowsDashboardHandlerExecutor::Reservation&
WindowsDashboardHandlerExecutor::Reservation::operator=(
    Reservation&& other) noexcept
{
    if (this != &other) {
        release();
        implementation_ = std::move(other.implementation_);
        ownsCapacity_ = std::exchange(other.ownsCapacity_, false);
    }
    return *this;
}

bool WindowsDashboardHandlerExecutor::Reservation::valid() const noexcept
{
    return ownsCapacity_ && implementation_ != nullptr &&
           implementation_->reservationValid();
}

Domain::Result<void>
WindowsDashboardHandlerExecutor::Reservation::trySubmit(
    std::unique_ptr<IDashboardHandlerOperation> operation,
    Domain::OperationContext context,
    std::weak_ptr<IDashboardHandlerCompletionSink> completionSink) && noexcept
{
    if (!ownsCapacity_ || implementation_ == nullptr) {
        return Domain::Result<void>::failure(invalidReservationError());
    }
    auto submitted = implementation_->trySubmitReservedPostDelivery(
        *this,
        std::move(operation),
        std::move(context),
        std::move(completionSink));
    if (submitted) {
        implementation_.reset();
    }
    return submitted;
}

void WindowsDashboardHandlerExecutor::Reservation::release() noexcept
{
    if (!ownsCapacity_ || implementation_ == nullptr) {
        ownsCapacity_ = false;
        implementation_.reset();
        return;
    }
    ownsCapacity_ = false;
    auto implementation = std::move(implementation_);
    implementation->releaseReservation();
}

Domain::Result<std::unique_ptr<WindowsDashboardHandlerExecutor>>
WindowsDashboardHandlerExecutor::create() noexcept
{
    try {
        auto implementation = std::make_shared<Impl>();
        auto started = implementation->start();
        if (!started) {
            return Domain::Result<
                std::unique_ptr<WindowsDashboardHandlerExecutor>>::failure(
                std::move(started).error());
        }
        return Domain::Result<
            std::unique_ptr<WindowsDashboardHandlerExecutor>>::success(
            std::unique_ptr<WindowsDashboardHandlerExecutor>{
                new WindowsDashboardHandlerExecutor{
                    std::move(implementation)}});
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<WindowsDashboardHandlerExecutor>>::failure(
            executorError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard handler executor could not be created."));
    }
}

WindowsDashboardHandlerExecutor::WindowsDashboardHandlerExecutor(
    std::shared_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsDashboardHandlerExecutor::~WindowsDashboardHandlerExecutor() noexcept
{
    shutdown();
}

Domain::Result<void> WindowsDashboardHandlerExecutor::trySubmit(
    std::unique_ptr<IDashboardHandlerOperation> operation,
    Domain::OperationContext context,
    std::weak_ptr<IDashboardHandlerCompletionSink> completionSink) noexcept
{
    return implementation_->trySubmit(
        std::move(operation),
        std::move(context),
        std::move(completionSink));
}

Domain::Result<WindowsDashboardHandlerExecutor::Reservation>
WindowsDashboardHandlerExecutor::tryReservePostDelivery() noexcept
{
    return implementation_->tryReservePostDelivery();
}

std::size_t WindowsDashboardHandlerExecutor::pendingCount() const noexcept
{
    return implementation_->pendingCount();
}

std::size_t WindowsDashboardHandlerExecutor::reservationCount() const noexcept
{
    return implementation_->reservationCount();
}

std::size_t WindowsDashboardHandlerExecutor::activeCount() const noexcept
{
    return implementation_->activeCount();
}

bool WindowsDashboardHandlerExecutor::isShuttingDown() const noexcept
{
    return implementation_->isShuttingDown();
}

bool WindowsDashboardHandlerExecutor::fullyDrained() const noexcept
{
    return implementation_->fullyDrained();
}

Domain::Result<void>
WindowsDashboardHandlerExecutor::bindShutdownDrainObserver(
    std::weak_ptr<IDashboardHandlerExecutorDrainObserver> observer) noexcept
{
    return implementation_->bindShutdownDrainObserver(std::move(observer));
}

void WindowsDashboardHandlerExecutor::beginShutdown() noexcept
{
    implementation_->beginShutdown();
}

void WindowsDashboardHandlerExecutor::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation != nullptr) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
