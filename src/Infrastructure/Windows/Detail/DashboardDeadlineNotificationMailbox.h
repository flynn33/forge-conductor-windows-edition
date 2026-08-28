#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct DashboardDeadlineNotificationHandle final {
    std::size_t slotIndex{};
    std::uint64_t generation{};
    std::uint64_t registrationId{};

    bool operator==(const DashboardDeadlineNotificationHandle&) const = default;
};

enum class DashboardDeadlinePublishDisposition : std::uint8_t {
    NotificationRequired,
    Coalesced,
};

struct DashboardDeadlinePublishResult final {
    DashboardDeadlinePublishDisposition disposition{
        DashboardDeadlinePublishDisposition::NotificationRequired};
    DashboardDeadlineNotificationHandle handle;

    bool operator==(const DashboardDeadlinePublishResult&) const = default;
};

class DashboardDeadlineNotificationSnapshot final {
public:
    [[nodiscard]] std::size_t registeredCount() const noexcept
    {
        return registeredCount_;
    }

    [[nodiscard]] std::size_t retiredAwaitingReapCount() const noexcept
    {
        return retiredAwaitingReapCount_;
    }

    [[nodiscard]] std::size_t pendingNotificationCount() const noexcept
    {
        return pendingNotificationCount_;
    }

    [[nodiscard]] std::size_t maximumOwnerCount() const noexcept
    {
        return maximumOwnerCount_;
    }

    [[nodiscard]] bool isShutdown() const noexcept { return shutdown_; }

private:
    friend class DashboardDeadlineNotificationMailbox;

    DashboardDeadlineNotificationSnapshot(
        std::size_t registeredCount,
        std::size_t retiredAwaitingReapCount,
        std::size_t pendingNotificationCount,
        std::size_t maximumOwnerCount,
        bool shutdown) noexcept;

    std::size_t registeredCount_{};
    std::size_t retiredAwaitingReapCount_{};
    std::size_t pendingNotificationCount_{};
    std::size_t maximumOwnerCount_{};
    bool shutdown_{};
};

// Fixed-capacity bridge between the deadline worker and the shared IOCP. One
// stable slot belongs to each live connection or listener owner. Registration
// identifiers come from a process-lifetime nonreusing allocator, but concurrent
// owners may arrive out of numeric order. Slot generations independently reject
// stale IOCP completions after fixed-slot reuse.
// Publishing to
// an empty slot requests one synthetic IOCP notification; publishing again
// before that notification is reaped replaces the slot's immutable deadline
// without posting another completion. Retirement retains a tombstone only
// while its already-posted notification still needs to be reaped.
class DashboardDeadlineNotificationMailbox final {
public:
    static constexpr std::size_t HardMaximumOwnerCount =
        WindowsDashboardDeadlineScheduler::HardMaximumScheduledCount;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardDeadlineNotificationMailbox>>
    create(
        std::size_t maximumOwnerCount = HardMaximumOwnerCount) noexcept;

    ~DashboardDeadlineNotificationMailbox() noexcept;

    DashboardDeadlineNotificationMailbox(
        const DashboardDeadlineNotificationMailbox&) = delete;
    DashboardDeadlineNotificationMailbox& operator=(
        const DashboardDeadlineNotificationMailbox&) = delete;
    DashboardDeadlineNotificationMailbox(
        DashboardDeadlineNotificationMailbox&&) = delete;
    DashboardDeadlineNotificationMailbox& operator=(
        DashboardDeadlineNotificationMailbox&&) = delete;

    [[nodiscard]] Domain::Result<DashboardDeadlineNotificationHandle>
    registerOwner(std::uint64_t registrationId) noexcept;

    [[nodiscard]] Domain::Result<DashboardDeadlinePublishResult> publish(
        WindowsDashboardDeadline deadline) noexcept;

    // Reaps exactly one posted notification for the immutable handle. A null
    // value means the handle was stale or its owner retired before delivery.
    [[nodiscard]] std::optional<WindowsDashboardDeadline> take(
        const DashboardDeadlineNotificationHandle& handle) noexcept;

    // Stops publication for this exact generation. If a notification is
    // pending, the slot remains as a bounded tombstone until take() reaps it.
    [[nodiscard]] bool retire(
        const DashboardDeadlineNotificationHandle& handle) noexcept;

    [[nodiscard]] DashboardDeadlineNotificationSnapshot snapshot()
        const noexcept;

    // Rejects registration and publication, discards unpublished values, and
    // retains only tombstones whose synthetic IOCP notification must be reaped.
    void shutdown() noexcept;

private:
    explicit DashboardDeadlineNotificationMailbox(
        std::size_t maximumOwnerCount);

    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
