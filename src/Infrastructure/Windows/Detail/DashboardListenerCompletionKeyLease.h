#pragma once

#include "DashboardFixedIocpKeyAuthority.h"

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class DashboardListenerCompletionKeyLeaseState;
class DashboardListenerCompletionKeyLeasePool;

// Move-only lifetime token for one of the two process-fixed listener keys.
// There is intentionally no explicit release operation: a key returns to the
// shared pool state only when its owning lease object is destroyed.
class DashboardListenerCompletionKeyLease final {
public:
    DashboardListenerCompletionKeyLease(
        const DashboardListenerCompletionKeyLease&) = delete;
    DashboardListenerCompletionKeyLease& operator=(
        const DashboardListenerCompletionKeyLease&) = delete;
    DashboardListenerCompletionKeyLease(
        DashboardListenerCompletionKeyLease&& other) noexcept;
    DashboardListenerCompletionKeyLease& operator=(
        DashboardListenerCompletionKeyLease&&) = delete;
    ~DashboardListenerCompletionKeyLease() noexcept;

    [[nodiscard]] bool ownsSlot() const noexcept;
    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept;

private:
    friend class DashboardListenerCompletionKeyLeaseState;
    friend class DashboardListenerCompletionKeyLeasePool;

    DashboardListenerCompletionKeyLease(
        std::shared_ptr<DashboardListenerCompletionKeyLeaseState> state,
        std::size_t slot) noexcept;

    std::shared_ptr<DashboardListenerCompletionKeyLeaseState> state_;
    std::size_t slot_{};
};

// Process-composition-owned authority for at most two simultaneous listener
// generations. Leases retain the shared pool state, so destroying the pool
// cannot make a live key reusable or strand its eventual release.
class DashboardListenerCompletionKeyLeasePool final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<
        DashboardListenerCompletionKeyLeasePool>>
    create(const DashboardFixedIocpKeyAuthority& authority) noexcept;

    DashboardListenerCompletionKeyLeasePool(
        const DashboardListenerCompletionKeyLeasePool&) = delete;
    DashboardListenerCompletionKeyLeasePool& operator=(
        const DashboardListenerCompletionKeyLeasePool&) = delete;
    DashboardListenerCompletionKeyLeasePool(
        DashboardListenerCompletionKeyLeasePool&&) = delete;
    DashboardListenerCompletionKeyLeasePool& operator=(
        DashboardListenerCompletionKeyLeasePool&&) = delete;
    ~DashboardListenerCompletionKeyLeasePool() noexcept = default;

    [[nodiscard]] Domain::Result<DashboardListenerCompletionKeyLease>
    tryAcquire() const noexcept;

private:
    explicit DashboardListenerCompletionKeyLeasePool(
        std::shared_ptr<DashboardListenerCompletionKeyLeaseState> state)
        noexcept;

    const std::shared_ptr<DashboardListenerCompletionKeyLeaseState> state_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
