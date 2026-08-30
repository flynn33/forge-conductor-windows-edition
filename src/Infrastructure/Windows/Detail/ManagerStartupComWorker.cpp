#include "ManagerStartupComWorker.h"

#include "OperationContextGuard.h"

#include <Windows.h>
#include <objbase.h>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::string_view OperationAction =
    "execute a Manager startup Task Scheduler operation";
constexpr auto CancellationPulseInterval = std::chrono::milliseconds{50};

enum class CancellationReason {
    Caller,
    Deadline,
    Shutdown
};

[[nodiscard]] Domain::Error workerError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error cancellationError(
    const CancellationReason reason)
{
    if (reason == CancellationReason::Deadline) {
        return workerError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The Manager startup Task Scheduler operation exceeded its deadline.");
    }
    if (reason == CancellationReason::Shutdown) {
        return workerError(
            Domain::ErrorCodes::Cancelled,
            "The Manager startup Task Scheduler operation was cancelled during shutdown.");
    }
    return workerError(
        Domain::ErrorCodes::Cancelled,
        "The Manager startup Task Scheduler operation was cancelled.");
}

[[nodiscard]] Domain::Error startupError(
    const HRESULT nativeResult)
{
    return workerError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "The Manager startup COM worker could not initialize its MTA boundary "
        "(HRESULT " +
            std::to_string(static_cast<unsigned long>(nativeResult)) + ").");
}

} // namespace

class ManagerStartupComWorker::Impl final {
public:
    explicit Impl(
        std::shared_ptr<IManagerStartupComHandler> handler,
        std::shared_ptr<IManagerStartupComWorkerAdmissionGate>
            admissionGate) noexcept
        : pendingHandler_{std::move(handler)},
          admissionGate_{std::move(admissionGate)}
    {
    }

    ~Impl() noexcept { shutdown(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept
    {
        try {
            auto handler = std::move(pendingHandler_);
            worker_ = std::jthread{
                [this, handler = std::move(handler)]() mutable noexcept {
                    workerMain(std::move(handler));
                }};

            std::unique_lock lock{stateMutex_};
            const bool started = stateChanged_.wait_for(
                lock,
                ManagerStartupComWorker::WorkerStartupTimeout,
                [this]() noexcept { return startupComplete_; });
            if (!started) {
                closed_ = true;
                lock.unlock();
                stateChanged_.notify_all();
                lock.lock();
                const bool stopped = stateChanged_.wait_for(
                    lock,
                    ManagerStartupComWorker::ShutdownDrainTimeout,
                    [this]() noexcept { return workerStopped_; });
                lock.unlock();
                if (!stopped) {
                    std::terminate();
                }
                if (worker_.joinable()) {
                    worker_.join();
                }
                return Domain::Result<void>::failure(workerError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The Manager startup COM worker did not initialize within its bounded startup window."));
            }

            const HRESULT nativeResult = startupResult_;
            lock.unlock();
            if (FAILED(nativeResult)) {
                if (worker_.joinable()) {
                    worker_.join();
                }
                return Domain::Result<void>::failure(
                    startupError(nativeResult));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            shutdown();
            return Domain::Result<void>::failure(workerError(
                Domain::ErrorCodes::InternalFailure,
                "The Manager startup COM worker could not be started safely."));
        }
    }

    [[nodiscard]] ManagerStartupComResult execute(
        ManagerStartupComRequest request) noexcept
    {
        bool admitted{};
        try {
            auto work = std::make_shared<WorkItem>(std::move(request));
            std::stop_callback cancellationRelay{
                work->callerCancellation,
                [this, work]() noexcept {
                    static_cast<void>(
                        work->operationCancellation.request_stop());
                    cancelWork(work, CancellationReason::Caller);
                }};
            {
                const std::lock_guard lock{stateMutex_};
                if (closed_) {
                    return ManagerStartupComResult::failure(workerError(
                        Domain::ErrorCodes::TransportClosed,
                        "The Manager startup COM worker is closed to new operations."));
                }
                if (hasOperationLocked(work->request.context.operationId)) {
                    return ManagerStartupComResult::failure(workerError(
                        Domain::ErrorCodes::Conflict,
                        "The Manager startup COM worker already owns this operation identifier."));
                }
                auto valid = validateOperationContext(
                    work->request.context,
                    std::chrono::steady_clock::now(),
                    OperationAction);
                if (!valid) {
                    return ManagerStartupComResult::failure(
                        std::move(valid).error());
                }
                if (active_ == nullptr) {
                    active_ = work;
                } else if (queued_ == nullptr) {
                    queued_ = work;
                } else {
                    return ManagerStartupComResult::failure(workerError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The Manager startup COM worker already owns one active and one queued operation.",
                        true));
                }
                admitted = true;
                if (admissionGate_ != nullptr) {
                    admissionGate_->afterAdmissionBeforeDispatch(
                        work->request.context.operationId);
                }
            }
            stateChanged_.notify_all();

            std::unique_lock lock{stateMutex_};
            while (!work->completion.has_value()) {
                if (work->cancellation.has_value()) {
                    if (!work->cancellationDrainDeadline.has_value()) {
                        std::terminate();
                    }
                    const auto drainDeadline =
                        work->cancellationDrainDeadline.value();
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= drainDeadline) {
                        std::terminate();
                    }
                    const auto pulseAt =
                        now + CancellationPulseInterval < drainDeadline
                            ? now + CancellationPulseInterval
                            : drainDeadline;
                    static_cast<void>(stateChanged_.wait_until(lock, pulseAt));
                    if (!work->completion.has_value()) {
                        lock.unlock();
                        pulseActiveCancellation(work);
                        lock.lock();
                    }
                    continue;
                }

                const auto operationDeadline =
                    work->request.context.deadline;
                if (stateChanged_.wait_until(lock, operationDeadline) ==
                        std::cv_status::timeout &&
                    !work->completion.has_value()) {
                    const auto cancellation = cancelWorkLocked(
                        work, CancellationReason::Deadline);
                    lock.unlock();
                    deliverCancellation(work, cancellation);
                    lock.lock();
                }
            }

            auto result = std::move(work->completion.value());
            lock.unlock();
            return result;
        } catch (...) {
            if (admitted) {
                std::terminate();
            }
            return ManagerStartupComResult::failure(workerError(
                Domain::ErrorCodes::InternalFailure,
                "The Manager startup COM operation could not be admitted safely."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::shared_ptr<WorkItem> work;
            {
                const std::lock_guard lock{stateMutex_};
                if (active_ != nullptr &&
                    active_->request.context.operationId == operationId) {
                    work = active_;
                } else if (
                    queued_ != nullptr &&
                    queued_->request.context.operationId == operationId) {
                    work = queued_;
                }
            }
            if (work != nullptr) {
                cancelWork(work, CancellationReason::Caller);
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] ManagerStartupComWorkerSnapshot snapshot() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return ManagerStartupComWorkerSnapshot{
                !closed_,
                startupComplete_ && SUCCEEDED(startupResult_) &&
                    !workerStopped_,
                active_ == nullptr ? 0U : 1U,
                queued_ == nullptr ? 0U : 1U};
        } catch (...) {
            return ManagerStartupComWorkerSnapshot{
                false, false, 0U, 0U};
        }
    }

    void shutdown() noexcept
    {
        try {
            const std::lock_guard shutdownLock{shutdownMutex_};

            bool relayNativeCancellation{};
            std::shared_ptr<WorkItem> activeWork;
            std::shared_ptr<WorkItem> queuedWork;
            {
                const std::lock_guard lock{stateMutex_};
                if (workerThreadId_ != 0U &&
                    workerThreadId_ == GetCurrentThreadId()) {
                    std::terminate();
                }

                closed_ = true;
                if (queued_ != nullptr) {
                    queuedWork = queued_;
                    completeWithoutExecutionLocked(
                        queued_, CancellationReason::Shutdown);
                    queued_.reset();
                }
                if (active_ != nullptr) {
                    activeWork = active_;
                    if (activeExecutionStarted_) {
                        relayNativeCancellation =
                            markActiveCancellationLocked(
                                active_, CancellationReason::Shutdown);
                    } else {
                        completeWithoutExecutionLocked(
                            active_, CancellationReason::Shutdown);
                        active_.reset();
                    }
                }
            }
            stateChanged_.notify_all();

            if (queuedWork != nullptr) {
                static_cast<void>(
                    queuedWork->operationCancellation.request_stop());
            }
            if (activeWork != nullptr) {
                static_cast<void>(
                    activeWork->operationCancellation.request_stop());
            }
            if (relayNativeCancellation) {
                pulseActiveCancellation(activeWork);
            }

            if (!worker_.joinable()) {
                return;
            }

            {
                std::unique_lock lock{stateMutex_};
                const auto drainDeadline =
                    std::chrono::steady_clock::now() +
                    ManagerStartupComWorker::ShutdownDrainTimeout;
                while (!workerStopped_) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= drainDeadline) {
                        std::terminate();
                    }
                    const auto pulseAt =
                        now + CancellationPulseInterval < drainDeadline
                            ? now + CancellationPulseInterval
                            : drainDeadline;
                    static_cast<void>(stateChanged_.wait_until(lock, pulseAt));
                    if (!workerStopped_ && activeWork != nullptr) {
                        lock.unlock();
                        pulseActiveCancellation(activeWork);
                        lock.lock();
                    }
                }
            }
            worker_.join();
        } catch (...) {
            std::terminate();
        }
    }

private:
    struct WorkItem final {
        explicit WorkItem(ManagerStartupComRequest value)
            : callerCancellation{value.context.cancellation},
              request{std::move(value)}
        {
            request.context.cancellation =
                operationCancellation.get_token();
        }

        std::stop_token callerCancellation;
        std::stop_source operationCancellation;
        ManagerStartupComRequest request;
        std::optional<ManagerStartupComResult> completion;
        std::optional<Domain::Error> cancellation;
        std::optional<Domain::MonotonicTimePoint>
            cancellationDrainDeadline;
    };

    struct CancellationActions final {
        bool requestOperationCancellation{};
        bool relayNativeCancellation{};
    };

    [[nodiscard]] bool hasOperationLocked(
        const Domain::OperationId& operationId) const noexcept
    {
        return (active_ != nullptr &&
                active_->request.context.operationId == operationId) ||
            (queued_ != nullptr &&
             queued_->request.context.operationId == operationId);
    }

    void completeWithoutExecutionLocked(
        const std::shared_ptr<WorkItem>& work,
        const CancellationReason reason)
    {
        if (!work->completion.has_value()) {
            work->completion.emplace(
                ManagerStartupComResult::failure(
                    cancellationError(reason)));
        }
    }

    [[nodiscard]] bool markActiveCancellationLocked(
        const std::shared_ptr<WorkItem>& work,
        const CancellationReason reason)
    {
        if (work->completion.has_value() ||
            work->cancellation.has_value()) {
            return false;
        }
        work->cancellation.emplace(cancellationError(reason));
        work->cancellationDrainDeadline.emplace(
            std::chrono::steady_clock::now() +
            ManagerStartupComWorker::CancellationDrainTimeout);
        return true;
    }

    [[nodiscard]] CancellationActions cancelWorkLocked(
        const std::shared_ptr<WorkItem>& work,
        const CancellationReason reason)
    {
        CancellationActions actions;
        if (work->completion.has_value()) {
            return actions;
        }
        if (queued_ == work) {
            actions.requestOperationCancellation = true;
            completeWithoutExecutionLocked(work, reason);
            queued_.reset();
        } else if (active_ == work) {
            actions.requestOperationCancellation = true;
            if (activeExecutionStarted_) {
                actions.relayNativeCancellation =
                    markActiveCancellationLocked(work, reason);
            } else {
                completeWithoutExecutionLocked(work, reason);
                active_.reset();
                promoteQueuedLocked();
            }
        }
        return actions;
    }

    void deliverCancellation(
        const std::shared_ptr<WorkItem>& work,
        const CancellationActions actions) noexcept
    {
        try {
            if (!actions.requestOperationCancellation) {
                return;
            }
            stateChanged_.notify_all();
            static_cast<void>(
                work->operationCancellation.request_stop());
            if (actions.relayNativeCancellation) {
                pulseActiveCancellation(work);
            }
        } catch (...) {
            std::terminate();
        }
    }

    void cancelWork(
        const std::shared_ptr<WorkItem>& work,
        const CancellationReason reason) noexcept
    {
        try {
            CancellationActions actions;
            {
                const std::lock_guard lock{stateMutex_};
                actions = cancelWorkLocked(work, reason);
            }
            deliverCancellation(work, actions);
        } catch (...) {
            std::terminate();
        }
    }

    void promoteQueuedLocked() noexcept
    {
        if (active_ != nullptr || closed_) {
            return;
        }
        active_ = std::move(queued_);
        activeExecutionStarted_ = false;
    }

    void pulseActiveCancellation(
        const std::shared_ptr<WorkItem>& work) noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            if (active_ == work && activeExecutionStarted_ &&
                work->cancellation.has_value() && workerThreadId_ != 0U) {
                static_cast<void>(CoCancelCall(workerThreadId_, 0U));
            }
        } catch (...) {
            std::terminate();
        }
    }

    void completeActiveLocked(
        const std::shared_ptr<WorkItem>& work,
        ManagerStartupComResult result)
    {
        if (active_ != work || !activeExecutionStarted_) {
            std::terminate();
        }
        if (work->cancellation.has_value()) {
            work->completion.emplace(
                ManagerStartupComResult::failure(
                    std::move(work->cancellation.value())));
        } else {
            work->completion.emplace(std::move(result));
        }
        active_.reset();
        activeExecutionStarted_ = false;
        promoteQueuedLocked();
    }

    void workerMain(
        std::shared_ptr<IManagerStartupComHandler> handler) noexcept
    {
        const HRESULT initialized = CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(initialized)) {
            handler.reset();
            publishStartupFailure(initialized);
            return;
        }

        const HRESULT cancellationEnabled =
            CoEnableCallCancellation(nullptr);
        if (FAILED(cancellationEnabled)) {
            handler.reset();
            CoUninitialize();
            publishStartupFailure(cancellationEnabled);
            return;
        }

        {
            const std::lock_guard lock{stateMutex_};
            workerThreadId_ = GetCurrentThreadId();
            startupResult_ = S_OK;
            startupComplete_ = true;
        }
        stateChanged_.notify_all();

        for (;;) {
            std::shared_ptr<WorkItem> work;
            {
                std::unique_lock lock{stateMutex_};
                stateChanged_.wait(lock, [this]() noexcept {
                    return active_ != nullptr || closed_;
                });
                if (active_ == nullptr) {
                    if (!closed_) {
                        std::terminate();
                    }
                    break;
                }

                work = active_;
                auto valid =
                    work->callerCancellation.stop_requested()
                    ? Domain::Result<void>::failure(
                          cancellationError(CancellationReason::Caller))
                    : validateOperationContext(
                          work->request.context,
                          std::chrono::steady_clock::now(),
                          OperationAction);
                if (!valid) {
                    work->completion.emplace(
                        ManagerStartupComResult::failure(
                            std::move(valid).error()));
                    active_.reset();
                    activeExecutionStarted_ = false;
                    promoteQueuedLocked();
                    lock.unlock();
                    stateChanged_.notify_all();
                    continue;
                }
                activeExecutionStarted_ = true;
            }

            auto result = handler->handle(work->request);
            {
                const std::lock_guard lock{stateMutex_};
                completeActiveLocked(work, std::move(result));
            }
            work.reset();
            stateChanged_.notify_all();
        }

        handler.reset();
        {
            const std::lock_guard lock{stateMutex_};
            if (active_ != nullptr || queued_ != nullptr ||
                activeExecutionStarted_) {
                std::terminate();
            }
        }

        // All handler and work ownership is already drained. A cleanup failure
        // cannot be repaired here, but it also cannot justify crashing the
        // process after the bounded lifetime invariant has been satisfied.
        static_cast<void>(CoDisableCallCancellation(nullptr));
        CoUninitialize();

        {
            const std::lock_guard lock{stateMutex_};
            workerThreadId_ = 0U;
            workerStopped_ = true;
        }
        stateChanged_.notify_all();
    }

    void publishStartupFailure(const HRESULT nativeResult) noexcept
    {
        try {
            {
                const std::lock_guard lock{stateMutex_};
                startupResult_ = nativeResult;
                startupComplete_ = true;
                workerStopped_ = true;
            }
            stateChanged_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    std::shared_ptr<IManagerStartupComHandler> pendingHandler_;
    std::shared_ptr<IManagerStartupComWorkerAdmissionGate>
        admissionGate_;
    mutable std::mutex stateMutex_;
    std::mutex shutdownMutex_;
    std::condition_variable stateChanged_;
    std::jthread worker_;
    std::shared_ptr<WorkItem> active_;
    std::shared_ptr<WorkItem> queued_;
    HRESULT startupResult_{E_UNEXPECTED};
    DWORD workerThreadId_{};
    bool startupComplete_{};
    bool workerStopped_{};
    bool activeExecutionStarted_{};
    bool closed_{};
};

Domain::Result<std::unique_ptr<ManagerStartupComWorker>>
ManagerStartupComWorker::create(
    std::shared_ptr<IManagerStartupComHandler> handler) noexcept
{
    return createForTesting(std::move(handler), nullptr);
}

Domain::Result<std::unique_ptr<ManagerStartupComWorker>>
ManagerStartupComWorker::createForTesting(
    std::shared_ptr<IManagerStartupComHandler> handler,
    std::shared_ptr<IManagerStartupComWorkerAdmissionGate>
        admissionGate) noexcept
{
    try {
        if (handler == nullptr) {
            return Domain::Result<
                std::unique_ptr<ManagerStartupComWorker>>::failure(
                workerError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Manager startup COM worker requires a handler."));
        }

        auto implementation = std::make_unique<Impl>(
            std::move(handler), std::move(admissionGate));
        auto started = implementation->start();
        if (!started) {
            return Domain::Result<
                std::unique_ptr<ManagerStartupComWorker>>::failure(
                std::move(started).error());
        }

        auto owner = std::unique_ptr<ManagerStartupComWorker>{
            new ManagerStartupComWorker{std::move(implementation)}};
        return Domain::Result<
            std::unique_ptr<ManagerStartupComWorker>>::success(
            std::move(owner));
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<ManagerStartupComWorker>>::failure(
            workerError(
                Domain::ErrorCodes::InternalFailure,
                "The Manager startup COM worker could not be created."));
    }
}

ManagerStartupComWorker::ManagerStartupComWorker(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

ManagerStartupComWorker::~ManagerStartupComWorker() noexcept
{
    shutdown();
}

ManagerStartupComResult ManagerStartupComWorker::execute(
    ManagerStartupComRequest request) noexcept
{
    return implementation_->execute(std::move(request));
}

void ManagerStartupComWorker::cancel(
    const Domain::OperationId& operationId) noexcept
{
    implementation_->cancel(operationId);
}

ManagerStartupComWorkerSnapshot ManagerStartupComWorker::snapshot()
    const noexcept
{
    return implementation_->snapshot();
}

void ManagerStartupComWorker::shutdown() noexcept
{
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
