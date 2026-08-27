#pragma once

#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace ForgeConductor::Dashboard {

namespace Detail {
class DashboardSseBroadcasterState;
class DashboardSseSubscriptionState;
} // namespace Detail

// Per-connection delivered-frame cadence. Null takes do not advance it;
// successful deliveries 10, 20, and so on select the full representation.
class DashboardSseDeliveryCursor final {
public:
    [[nodiscard]] DashboardSseFramePair::ImmutableBytes select(
        const DashboardSseFramePair::ImmutableFrame& frame) noexcept
    {
        if (frame == nullptr) {
            return {};
        }
        const bool useFullRepresentation = deliveryCadence_ == 9U;
        deliveryCadence_ = useFullRepresentation
            ? static_cast<std::uint8_t>(0U)
            : static_cast<std::uint8_t>(deliveryCadence_ + 1U);
        return useFullRepresentation ? frame->fullBytes()
                                     : frame->compactBytes();
    }

private:
    std::uint8_t deliveryCadence_{};
};

// Concrete latest-value subscription returned to application composition and
// then owned through IDashboardSseSubscription by the prepared exchange.
class DashboardSseSubscription final : public IDashboardSseSubscription {
public:
    DashboardSseSubscription(const DashboardSseSubscription&) = delete;
    DashboardSseSubscription& operator=(const DashboardSseSubscription&) =
        delete;
    DashboardSseSubscription(DashboardSseSubscription&&) = delete;
    DashboardSseSubscription& operator=(DashboardSseSubscription&&) = delete;
    ~DashboardSseSubscription() noexcept override;

    void attachReadySink(
        std::weak_ptr<IDashboardSseReadySink> sink) noexcept override;

    [[nodiscard]] DashboardSseFramePair::ImmutableFrame takeLatest()
        noexcept override;

    [[nodiscard]] std::size_t pendingCount() const noexcept override;

    [[nodiscard]] double deliveryHz() const noexcept override;

    void close() noexcept override;

    [[nodiscard]] bool isClosed() const noexcept;

private:
    friend class DashboardSseBroadcaster;

    explicit DashboardSseSubscription(
        std::shared_ptr<Detail::DashboardSseSubscriptionState> state) noexcept;

    const std::shared_ptr<Detail::DashboardSseSubscriptionState> state_;
};

// Synchronous, platform-neutral latest-value fan-out. Publication stores at
// most one shared immutable frame per subscription and performs no allocation
// proportional to publication history. Ready sinks must keep signal bounded:
// signaling is synchronous so the broadcaster never owns a callback queue.
class DashboardSseBroadcaster final {
public:
    static constexpr std::size_t HardMaximumSubscriptions = 32U;

    DashboardSseBroadcaster(const DashboardSseBroadcaster&) = delete;
    DashboardSseBroadcaster& operator=(const DashboardSseBroadcaster&) = delete;
    DashboardSseBroadcaster(DashboardSseBroadcaster&& other) noexcept;
    DashboardSseBroadcaster& operator=(DashboardSseBroadcaster&& other) noexcept;
    ~DashboardSseBroadcaster() noexcept;

    [[nodiscard]] static Domain::Result<DashboardSseBroadcaster> create(
        std::size_t maximumSubscriptions = HardMaximumSubscriptions) noexcept;

    [[nodiscard]] Domain::Result<std::unique_ptr<DashboardSseSubscription>>
    subscribe(
        DashboardSseFramePair::ImmutableFrame initialFrame,
        double deliveryHz) noexcept;

    [[nodiscard]] Domain::Result<void> publish(
        DashboardSseFramePair::ImmutableFrame frame) noexcept;

    [[nodiscard]] std::size_t maximumSubscriptionCount() const noexcept;
    [[nodiscard]] std::size_t liveSubscriptionCount() const noexcept;
    [[nodiscard]] bool isShutdown() const noexcept;

    // Bounded synchronous state transition only: no callback, worker, join,
    // condition wait, or drain is owned here. A signal already selected by an
    // in-flight publisher may finish after shutdown using its captured shared
    // sink lifetime; all subscription state is already closed and detached.
    void shutdown() noexcept;

private:
    explicit DashboardSseBroadcaster(
        std::shared_ptr<Detail::DashboardSseBroadcasterState> state) noexcept;

    std::shared_ptr<Detail::DashboardSseBroadcasterState> state_;
};

} // namespace ForgeConductor::Dashboard
