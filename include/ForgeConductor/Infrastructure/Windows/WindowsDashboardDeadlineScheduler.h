#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {

enum class WindowsDashboardDeadlineKind : std::uint8_t {
    HeaderIngress,
    HandlerExecution,
    SocketLifetime,
    ServerSentEventsLifetime,
    ServerSentEventsDelivery,
    OverloadResponse,
    ListenerRetirement,
    ShutdownDrain,
};

struct WindowsDashboardDeadlineRequest final {
    std::uint64_t registrationId{};
    WindowsDashboardDeadlineKind kind{
        WindowsDashboardDeadlineKind::HeaderIngress};
    Domain::MonotonicTimePoint deadline;

    bool operator==(const WindowsDashboardDeadlineRequest&) const = default;
};

struct WindowsDashboardDeadline final {
    std::uint64_t registrationId{};
    std::uint64_t armSequence{};
    WindowsDashboardDeadlineKind kind{
        WindowsDashboardDeadlineKind::HeaderIngress};
    Domain::MonotonicTimePoint deadline;

    bool operator==(const WindowsDashboardDeadline&) const = default;
};

// Implemented by the IOCP runtime. signal() must remain nonblocking and posts
// one synthetic completion carrying the immutable deadline value.
class IWindowsDashboardDeadlineSink {
public:
    virtual ~IWindowsDashboardDeadlineSink() noexcept = default;
    virtual void signal(WindowsDashboardDeadline deadline) noexcept = 0;
};

enum class WindowsDashboardDeadlineSchedulerFailureKind : std::uint8_t {
    DeadlineSinkUnavailable,
    WorkerFailure,
};

struct WindowsDashboardDeadlineSchedulerFailure final {
    WindowsDashboardDeadlineSchedulerFailureKind kind{
        WindowsDashboardDeadlineSchedulerFailureKind::WorkerFailure};

    bool operator==(
        const WindowsDashboardDeadlineSchedulerFailure&) const = default;
};

// Optional for standalone schedulers and one-shot once managed. A managed
// observer is notified outside scheduler locks when its owner thread can no
// longer deliver exact deadlines. Losing a previously bound observer at that
// boundary is process-fatal rather than silently discarding the health edge.
class IWindowsDashboardDeadlineSchedulerFailureObserver {
public:
    virtual ~IWindowsDashboardDeadlineSchedulerFailureObserver() noexcept =
        default;
    virtual void dashboardDeadlineSchedulerFailed(
        WindowsDashboardDeadlineSchedulerFailure failure) noexcept = 0;
};

class WindowsDashboardDeadlineSnapshot final {
public:
    [[nodiscard]] std::size_t scheduledCount() const noexcept
    {
        return scheduledCount_;
    }

    [[nodiscard]] std::size_t maximumScheduledCount() const noexcept
    {
        return maximumScheduledCount_;
    }

    [[nodiscard]] bool isShutdown() const noexcept { return shutdown_; }

    [[nodiscard]] const WindowsDashboardDeadlineSchedulerFailure* failure()
        const noexcept
    {
        return failure_.has_value() ? std::addressof(*failure_) : nullptr;
    }

private:
    friend class WindowsDashboardDeadlineScheduler;

    WindowsDashboardDeadlineSnapshot(
        const std::size_t scheduledCount,
        const std::size_t maximumScheduledCount,
        const bool shutdown,
        std::optional<WindowsDashboardDeadlineSchedulerFailure> failure)
        noexcept
        : scheduledCount_{scheduledCount},
          maximumScheduledCount_{maximumScheduledCount},
          shutdown_{shutdown},
          failure_{std::move(failure)}
    {
    }

    std::size_t scheduledCount_{};
    std::size_t maximumScheduledCount_{};
    bool shutdown_{};
    std::optional<WindowsDashboardDeadlineSchedulerFailure> failure_;
};

// Process-owned deadline owner for dashboard connections and listener
// generations. Registration is indexed by a nonzero stable owner identifier;
// every arm for that owner carries a new nonzero monotonically increasing
// sequence generated scheduler-wide. An update replaces the existing entry in
// place, so superseded deadlines never accumulate. A completion consumer
// accepts an event only when both identifier and sequence still match its
// current arm. The hard ceiling covers 40 connections, two listener
// generations, one fixed overload-response pool, and one process-shutdown
// drain deadline.
class WindowsDashboardDeadlineScheduler final {
public:
    static constexpr std::size_t HardMaximumScheduledCount = 44U;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<WindowsDashboardDeadlineScheduler>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::weak_ptr<IWindowsDashboardDeadlineSink> sink,
        std::size_t maximumScheduledCount =
            HardMaximumScheduledCount) noexcept;

    ~WindowsDashboardDeadlineScheduler() noexcept;

    WindowsDashboardDeadlineScheduler(
        const WindowsDashboardDeadlineScheduler&) = delete;
    WindowsDashboardDeadlineScheduler& operator=(
        const WindowsDashboardDeadlineScheduler&) = delete;
    WindowsDashboardDeadlineScheduler(
        WindowsDashboardDeadlineScheduler&&) = delete;
    WindowsDashboardDeadlineScheduler& operator=(
        WindowsDashboardDeadlineScheduler&&) = delete;

    [[nodiscard]] Domain::Result<WindowsDashboardDeadline> schedule(
        WindowsDashboardDeadlineRequest request) noexcept;

    [[nodiscard]] Domain::Result<void> bindFailureObserver(
        std::weak_ptr<
            IWindowsDashboardDeadlineSchedulerFailureObserver> observer)
        noexcept;

    // Returns true only when the exact live arm was removed. Token-aware
    // cancellation prevents a delayed cancel from removing a successor arm.
    [[nodiscard]] bool cancel(
        std::uint64_t registrationId,
        std::uint64_t armSequence) noexcept;

    [[nodiscard]] WindowsDashboardDeadlineSnapshot snapshot() const noexcept;

    // Idempotently rejects future registrations, discards all pending entries,
    // wakes the single owner thread, and joins it when called off that thread.
    void shutdown() noexcept;

private:
    class Impl;

    explicit WindowsDashboardDeadlineScheduler(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
