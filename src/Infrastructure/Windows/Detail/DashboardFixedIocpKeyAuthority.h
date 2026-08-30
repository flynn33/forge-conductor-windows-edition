#pragma once

#include "DashboardIoCompletionPort.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <cstddef>
#include <span>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Immutable process authority for the four completion keys whose routing
// roles are fixed independently of dynamically admitted connections.
class DashboardFixedIocpKeyAuthority final {
public:
    static constexpr std::size_t FixedKeyCount = 4U;

    [[nodiscard]] static Domain::Result<DashboardFixedIocpKeyAuthority>
    create() noexcept;

    // Injectable validation seam. Production composition uses create().
    [[nodiscard]] static Domain::Result<DashboardFixedIocpKeyAuthority>
    create(
        DashboardIoCompletionKey deadline,
        DashboardIoCompletionKey overload,
        DashboardIoCompletionKey listenerSlotA,
        DashboardIoCompletionKey listenerSlotB) noexcept;

    [[nodiscard]] DashboardIoCompletionKey deadline() const noexcept;
    [[nodiscard]] DashboardIoCompletionKey overload() const noexcept;
    [[nodiscard]] DashboardIoCompletionKey listenerSlotA() const noexcept;
    [[nodiscard]] DashboardIoCompletionKey listenerSlotB() const noexcept;

    [[nodiscard]] std::span<
        const DashboardIoCompletionKey,
        FixedKeyCount>
    completionKeys() const noexcept;

    [[nodiscard]] bool contains(
        DashboardIoCompletionKey key) const noexcept;

private:
    explicit DashboardFixedIocpKeyAuthority(
        std::array<DashboardIoCompletionKey, FixedKeyCount> keys) noexcept;

    const std::array<DashboardIoCompletionKey, FixedKeyCount> keys_;
};

static_assert(DashboardFixedIocpKeyAuthority::FixedKeyCount == 4U);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
