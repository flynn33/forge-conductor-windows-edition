#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"

#include "Detail/DashboardBoundedMonotonicSequence.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr auto MinimumClockRecheckInterval = std::chrono::milliseconds{50};
constexpr auto MaximumClockRecheckInterval = std::chrono::milliseconds{250};

[[nodiscard]] Domain::Error schedulerError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] bool entryLess(
    const WindowsDashboardDeadline& left,
    const WindowsDashboardDeadline& right) noexcept
{
    if (left.deadline != right.deadline) {
        return left.deadline < right.deadline;
    }
    return left.registrationId < right.registrationId;
}

[[nodiscard]] constexpr bool isDefinedDeadlineKind(
    const WindowsDashboardDeadlineKind kind) noexcept
{
    switch (kind) {
    case WindowsDashboardDeadlineKind::HeaderIngress:
    case WindowsDashboardDeadlineKind::HandlerExecution:
    case WindowsDashboardDeadlineKind::SocketLifetime:
    case WindowsDashboardDeadlineKind::ServerSentEventsLifetime:
    case WindowsDashboardDeadlineKind::ServerSentEventsDelivery:
    case WindowsDashboardDeadlineKind::OverloadResponse:
    case WindowsDashboardDeadlineKind::ListenerRetirement:
    case WindowsDashboardDeadlineKind::ShutdownDrain:
        return true;
    default:
        return false;
    }
}

} // namespace

class WindowsDashboardDeadlineScheduler::Impl final
    : public std::enable_shared_from_this<
          WindowsDashboardDeadlineScheduler::Impl> {
public:
    Impl(
        std::shared_ptr<Contracts::IClock> clock,
        std::weak_ptr<IWindowsDashboardDeadlineSink> sink,
        const std::size_t maximumScheduledCount)
        : clock_{std::move(clock)},
          sink_{std::move(sink)},
          maximumScheduledCount_{maximumScheduledCount}
    {
        entries_.reserve(maximumScheduledCount_);
    }

    ~Impl() noexcept { shutdown(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void start()
    {
        const auto owner = shared_from_this();
        std::jthread worker{
            [owner](std::stop_token) noexcept { owner->workerMain(); }};
        const std::lock_guard lifecycleLock{lifecycleMutex_};
        workerThreadId_ = worker.get_id();
        workerExited_ = false;
        worker_ = std::move(worker);
    }

    [[nodiscard]] Domain::Result<WindowsDashboardDeadline> schedule(
        WindowsDashboardDeadlineRequest request) noexcept
    {
        try {
            if (request.registrationId == 0U) {
                return Domain::Result<WindowsDashboardDeadline>::failure(
                    schedulerError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A dashboard deadline requires a nonzero owner identifier."));
            }
            if (!isDefinedDeadlineKind(request.kind)) {
                return Domain::Result<WindowsDashboardDeadline>::failure(
                    schedulerError(
                        Domain::ErrorCodes::InvalidRequest,
                        "A dashboard deadline requires a defined deadline "
                        "kind."));
            }

            WindowsDashboardDeadline scheduled;
            {
                const std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return Domain::Result<WindowsDashboardDeadline>::failure(
                        schedulerError(
                            Domain::ErrorCodes::TransportClosed,
                            "The dashboard deadline scheduler is shut down."));
                }
                const auto existing = findByRegistrationId(
                    request.registrationId);
                if (existing == entries_.end() &&
                    entries_.size() >= maximumScheduledCount_) {
                    return Domain::Result<WindowsDashboardDeadline>::failure(
                        schedulerError(
                            Domain::ErrorCodes::LimitExceeded,
                            "Dashboard deadline capacity is exhausted.",
                            true));
                }
                auto stagedArmSequences = armSequences_;
                const auto armSequence = stagedArmSequences.tryTake(
                    [](const std::uint64_t) noexcept { return false; });
                if (!armSequence.has_value()) {
                    return Domain::Result<WindowsDashboardDeadline>::failure(
                        schedulerError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The dashboard deadline arm sequence is "
                            "exhausted."));
                }
                if (existing != entries_.end()) {
                    entries_.erase(existing);
                }
                scheduled = WindowsDashboardDeadline{
                    request.registrationId,
                    *armSequence,
                    request.kind,
                    request.deadline};
                const auto insertion = std::lower_bound(
                    entries_.begin(), entries_.end(), scheduled, entryLess);
                entries_.insert(insertion, scheduled);
                armSequences_ = stagedArmSequences;
                ++revision_;
            }
            stateChanged_.notify_one();
            return Domain::Result<WindowsDashboardDeadline>::success(
                std::move(scheduled));
        } catch (...) {
            return Domain::Result<WindowsDashboardDeadline>::failure(
                schedulerError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard deadline could not be scheduled."));
        }
    }

    [[nodiscard]] bool cancel(
        const std::uint64_t registrationId,
        const std::uint64_t armSequence) noexcept
    {
        try {
            bool removed = false;
            {
                const std::lock_guard lock{mutex_};
                if (!shutdown_) {
                    const auto existing = findByRegistrationId(registrationId);
                    if (existing != entries_.end() &&
                        existing->armSequence == armSequence) {
                        entries_.erase(existing);
                        ++revision_;
                        removed = true;
                    }
                }
            }
            if (removed) {
                stateChanged_.notify_one();
            }
            return removed;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] WindowsDashboardDeadlineSnapshot snapshot() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return WindowsDashboardDeadlineSnapshot{
                entries_.size(), maximumScheduledCount_, shutdown_};
        } catch (...) {
            return WindowsDashboardDeadlineSnapshot{
                0U, maximumScheduledCount_, true};
        }
    }

    void shutdown() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                if (!shutdown_) {
                    shutdown_ = true;
                    entries_.clear();
                    ++revision_;
                }
            }
            stateChanged_.notify_all();

            std::jthread claimedWorker;
            {
                const std::unique_lock lifecycleLock{lifecycleMutex_};
                const bool calledFromWorker =
                    !workerExited_ &&
                    workerThreadId_ == std::this_thread::get_id();
                if (calledFromWorker) {
                    if (worker_.joinable()) {
                        worker_.request_stop();
                        // The running lambda retains shared implementation
                        // ownership until it returns. Only its own handle is
                        // detached to avoid self-join.
                        worker_.detach();
                    }
                    return;
                }
                if (worker_.joinable()) {
                    worker_.request_stop();
                    claimedWorker = std::move(worker_);
                } else if (workerExited_) {
                    return;
                }
            }
            if (claimedWorker.joinable()) {
                claimedWorker.join();
            }
            {
                std::unique_lock lifecycleLock{lifecycleMutex_};
                lifecycleChanged_.wait(
                    lifecycleLock, [this] { return workerExited_; });
            }
        } catch (...) {
            // A noexcept platform shutdown boundary retains ownership. The
            // jthread destructor gets a final opportunity to join.
        }
    }

private:
    class WorkerExitGuard final {
    public:
        explicit WorkerExitGuard(Impl& owner) noexcept : owner_{owner} {}

        ~WorkerExitGuard() noexcept { owner_.markWorkerExited(); }

        WorkerExitGuard(const WorkerExitGuard&) = delete;
        WorkerExitGuard& operator=(const WorkerExitGuard&) = delete;

    private:
        Impl& owner_;
    };

    void markWorkerExited() noexcept
    {
        try {
            {
                const std::lock_guard lifecycleLock{lifecycleMutex_};
                workerExited_ = true;
            }
            lifecycleChanged_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    using EntryIterator = std::vector<WindowsDashboardDeadline>::iterator;

    [[nodiscard]] EntryIterator findByRegistrationId(
        const std::uint64_t registrationId) noexcept
    {
        return std::find_if(
            entries_.begin(),
            entries_.end(),
            [registrationId](const WindowsDashboardDeadline& entry) noexcept {
                return entry.registrationId == registrationId;
            });
    }

    void workerMain() noexcept
    {
        WorkerExitGuard exitGuard{*this};
        try {
            for (;;) {
                std::optional<WindowsDashboardDeadline> due;
                {
                    std::unique_lock lock{mutex_};
                    for (;;) {
                        if (shutdown_) {
                            return;
                        }
                        if (entries_.empty()) {
                            const auto revision = revision_;
                            stateChanged_.wait(lock, [this, revision] {
                                return shutdown_ || revision_ != revision;
                            });
                            continue;
                        }

                        const auto now = clock_->monotonicNow();
                        if (entries_.front().deadline <= now) {
                            due.emplace(std::move(entries_.front()));
                            entries_.erase(entries_.begin());
                            ++revision_;
                            break;
                        }

                        const auto remaining =
                            entries_.front().deadline - now;
                        const auto relativeWait = std::clamp(
                            remaining,
                            std::chrono::duration_cast<
                                Domain::MonotonicTimePoint::duration>(
                                MinimumClockRecheckInterval),
                            std::chrono::duration_cast<
                                Domain::MonotonicTimePoint::duration>(
                                MaximumClockRecheckInterval));
                        const auto revision = revision_;
                        static_cast<void>(stateChanged_.wait_for(
                            lock,
                            relativeWait,
                            [this, revision] {
                                return shutdown_ || revision_ != revision;
                            }));
                    }
                }

                if (due) {
                    if (const auto sink = sink_.lock()) {
                        sink->signal(std::move(*due));
                    }
                }
            }
        } catch (...) {
            // No exception may cross the native worker boundary. A failure in
            // this fixed owner closes registration and wakes future callers as
            // TransportClosed rather than leaving an apparently live timer.
            try {
                {
                    const std::lock_guard lock{mutex_};
                    shutdown_ = true;
                    entries_.clear();
                    ++revision_;
                }
                stateChanged_.notify_all();
            } catch (...) {
            }
        }
    }

    const std::shared_ptr<Contracts::IClock> clock_;
    const std::weak_ptr<IWindowsDashboardDeadlineSink> sink_;
    const std::size_t maximumScheduledCount_{};

    mutable std::mutex mutex_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::condition_variable stateChanged_;
    std::vector<WindowsDashboardDeadline> entries_;
    std::jthread worker_;
    std::thread::id workerThreadId_{};
    std::uint64_t revision_{};
    Detail::DashboardBoundedMonotonicSequence<std::uint64_t>
        armSequences_{1U};
    bool workerExited_{true};
    bool shutdown_{};
};

Domain::Result<std::unique_ptr<WindowsDashboardDeadlineScheduler>>
WindowsDashboardDeadlineScheduler::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::weak_ptr<IWindowsDashboardDeadlineSink> sink,
    const std::size_t maximumScheduledCount) noexcept
{
    using SchedulerResult = Domain::Result<
        std::unique_ptr<WindowsDashboardDeadlineScheduler>>;
    try {
        if (!clock || sink.expired() || maximumScheduledCount == 0U ||
            maximumScheduledCount > HardMaximumScheduledCount) {
            return SchedulerResult::failure(schedulerError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard deadline scheduler requires a clock, a live sink, and a bounded positive capacity."));
        }

        auto implementation = std::make_shared<Impl>(
            std::move(clock), std::move(sink), maximumScheduledCount);
        implementation->start();
        return SchedulerResult::success(
            std::unique_ptr<WindowsDashboardDeadlineScheduler>{
                new WindowsDashboardDeadlineScheduler{
                    std::move(implementation)}});
    } catch (...) {
        return SchedulerResult::failure(schedulerError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard deadline scheduler could not be created."));
    }
}

WindowsDashboardDeadlineScheduler::WindowsDashboardDeadlineScheduler(
    std::shared_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsDashboardDeadlineScheduler::~WindowsDashboardDeadlineScheduler() noexcept
{
    shutdown();
}

Domain::Result<WindowsDashboardDeadline>
WindowsDashboardDeadlineScheduler::schedule(
    WindowsDashboardDeadlineRequest request) noexcept
{
    return implementation_->schedule(std::move(request));
}

bool WindowsDashboardDeadlineScheduler::cancel(
    const std::uint64_t registrationId,
    const std::uint64_t armSequence) noexcept
{
    return implementation_->cancel(registrationId, armSequence);
}

WindowsDashboardDeadlineSnapshot
WindowsDashboardDeadlineScheduler::snapshot() const noexcept
{
    return implementation_->snapshot();
}

void WindowsDashboardDeadlineScheduler::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
