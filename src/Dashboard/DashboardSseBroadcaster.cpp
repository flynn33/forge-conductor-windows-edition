#include "ForgeConductor/Dashboard/DashboardSseBroadcaster.h"

#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace ForgeConductor::Dashboard {

namespace Detail {

class DashboardSseBroadcasterState;

class DashboardSseSubscriptionState final {
public:
    explicit DashboardSseSubscriptionState(
        std::weak_ptr<DashboardSseBroadcasterState> owner,
        DashboardSseFramePair::ImmutableFrame initialFrame,
        const double deliveryHz) noexcept
        : owner_{std::move(owner)},
          pendingFrame_{std::move(initialFrame)},
          deliveryHz_{deliveryHz}
    {
    }

    [[nodiscard]] bool bindSlot(const std::size_t slot)
    {
        std::lock_guard lock{mutex_};
        if (closed_) {
            return false;
        }
        slot_ = slot;
        return true;
    }

    void attachReadySink(std::weak_ptr<IDashboardSseReadySink> sink)
    {
        std::shared_ptr<IDashboardSseReadySink> ready;
        {
            std::lock_guard lock{mutex_};
            if (closed_) {
                return;
            }
            readySink_ = std::move(sink);
            ready = readySink_.lock();
            if (pendingFrame_ != nullptr && ready != nullptr &&
                (!pendingSignalIssued_ ||
                 !hasSameOwner(lastSignaledSink_, ready))) {
                pendingSignalIssued_ = true;
                lastSignaledSink_ = ready;
            } else {
                ready.reset();
            }
        }
        if (ready != nullptr) {
            ready->signal();
        }
    }

    void publish(DashboardSseFramePair::ImmutableFrame frame)
    {
        std::shared_ptr<IDashboardSseReadySink> sink;
        {
            std::lock_guard lock{mutex_};
            if (closed_) {
                return;
            }
            const bool wasEmpty = pendingFrame_ == nullptr;
            pendingFrame_ = std::move(frame);
            if (wasEmpty) {
                pendingSignalIssued_ = false;
                lastSignaledSink_.reset();
                sink = readySink_.lock();
                if (sink != nullptr) {
                    pendingSignalIssued_ = true;
                    lastSignaledSink_ = sink;
                }
            }
        }

        // Never call transport-owned code while holding subscription state.
        // The captured shared sink makes an already selected signal safe when
        // close or broadcaster shutdown wins concurrently.
        if (sink != nullptr) {
            sink->signal();
        }
    }

    [[nodiscard]] DashboardSseFramePair::ImmutableFrame takeLatest()
    {
        std::lock_guard lock{mutex_};
        if (closed_ || pendingFrame_ == nullptr) {
            return {};
        }
        pendingSignalIssued_ = false;
        lastSignaledSink_.reset();
        return std::exchange(pendingFrame_, {});
    }

    [[nodiscard]] std::size_t pendingCount() const
    {
        std::lock_guard lock{mutex_};
        return pendingFrame_ == nullptr ? 0U : 1U;
    }

    [[nodiscard]] bool isClosed() const
    {
        std::lock_guard lock{mutex_};
        return closed_;
    }

    [[nodiscard]] double deliveryHz() const noexcept { return deliveryHz_; }

    void close();

private:
    [[nodiscard]] static bool hasSameOwner(
        const std::weak_ptr<IDashboardSseReadySink>& left,
        const std::shared_ptr<IDashboardSseReadySink>& right) noexcept
    {
        return !left.owner_before(right) && !right.owner_before(left);
    }

    static constexpr std::size_t UnboundSlot =
        std::numeric_limits<std::size_t>::max();

    mutable std::mutex mutex_;
    std::weak_ptr<DashboardSseBroadcasterState> owner_;
    std::weak_ptr<IDashboardSseReadySink> readySink_;
    std::weak_ptr<IDashboardSseReadySink> lastSignaledSink_;
    DashboardSseFramePair::ImmutableFrame pendingFrame_;
    const double deliveryHz_{};
    std::size_t slot_{UnboundSlot};
    bool pendingSignalIssued_{};
    bool closed_{};
};

class DashboardSseBroadcasterState final {
public:
    enum class RegistrationStatus : std::uint8_t {
        Accepted,
        CapacityReached,
        Closed,
    };

    struct Registration final {
        RegistrationStatus status{RegistrationStatus::Closed};
        std::size_t slot{};
    };

    explicit DashboardSseBroadcasterState(
        const std::size_t maximumSubscriptions) noexcept
        : maximumSubscriptions_{maximumSubscriptions}
    {
    }

    [[nodiscard]] Registration registerSubscription(
        const std::shared_ptr<DashboardSseSubscriptionState>& subscription)
    {
        std::lock_guard lock{mutex_};
        if (shutdown_) {
            return {RegistrationStatus::Closed, 0U};
        }

        cleanupExpiredLocked();
        for (std::size_t index{}; index < maximumSubscriptions_; ++index) {
            if (!slots_[index].occupied) {
                slots_[index].subscription = subscription;
                slots_[index].occupied = true;
                ++liveSubscriptions_;
                return {RegistrationStatus::Accepted, index};
            }
        }
        return {RegistrationStatus::CapacityReached, 0U};
    }

    void unregisterSubscription(
        const std::size_t slot,
        const DashboardSseSubscriptionState* const expected)
    {
        std::lock_guard lock{mutex_};
        if (slot >= maximumSubscriptions_ || !slots_[slot].occupied) {
            return;
        }

        const auto current = slots_[slot].subscription.lock();
        if (current.get() != expected) {
            return;
        }
        clearSlotLocked(slot);
    }

    [[nodiscard]] bool publish(
        const DashboardSseFramePair::ImmutableFrame& frame)
    {
        std::array<std::shared_ptr<DashboardSseSubscriptionState>,
                   DashboardSseBroadcaster::HardMaximumSubscriptions>
            subscriptions;
        std::size_t subscriptionCount{};
        {
            std::lock_guard lock{mutex_};
            if (shutdown_) {
                return false;
            }

            for (std::size_t index{}; index < maximumSubscriptions_; ++index) {
                if (!slots_[index].occupied) {
                    continue;
                }
                auto subscription = slots_[index].subscription.lock();
                if (subscription == nullptr) {
                    clearSlotLocked(index);
                    continue;
                }
                subscriptions[subscriptionCount++] =
                    std::move(subscription);
            }
        }

        for (std::size_t index{}; index < subscriptionCount; ++index) {
            subscriptions[index]->publish(frame);
        }
        return true;
    }

    [[nodiscard]] std::size_t maximumSubscriptionCount() const noexcept
    {
        return maximumSubscriptions_;
    }

    [[nodiscard]] std::size_t liveSubscriptionCount()
    {
        std::lock_guard lock{mutex_};
        cleanupExpiredLocked();
        return liveSubscriptions_;
    }

    [[nodiscard]] bool isShutdown() const
    {
        std::lock_guard lock{mutex_};
        return shutdown_;
    }

    void shutdown()
    {
        std::array<std::shared_ptr<DashboardSseSubscriptionState>,
                   DashboardSseBroadcaster::HardMaximumSubscriptions>
            subscriptions;
        std::size_t subscriptionCount{};
        {
            std::lock_guard lock{mutex_};
            if (shutdown_) {
                return;
            }
            shutdown_ = true;
            for (std::size_t index{}; index < maximumSubscriptions_; ++index) {
                if (slots_[index].occupied) {
                    if (auto subscription =
                            slots_[index].subscription.lock();
                        subscription != nullptr) {
                        subscriptions[subscriptionCount++] =
                            std::move(subscription);
                    }
                    slots_[index].subscription.reset();
                    slots_[index].occupied = false;
                }
            }
            liveSubscriptions_ = 0U;
        }

        for (std::size_t index{}; index < subscriptionCount; ++index) {
            subscriptions[index]->close();
        }
    }

private:
    struct Slot final {
        std::weak_ptr<DashboardSseSubscriptionState> subscription;
        bool occupied{};
    };

    void clearSlotLocked(const std::size_t slot) noexcept
    {
        slots_[slot].subscription.reset();
        slots_[slot].occupied = false;
        if (liveSubscriptions_ > 0U) {
            --liveSubscriptions_;
        }
    }

    void cleanupExpiredLocked() noexcept
    {
        for (std::size_t index{}; index < maximumSubscriptions_; ++index) {
            if (slots_[index].occupied &&
                slots_[index].subscription.expired()) {
                clearSlotLocked(index);
            }
        }
    }

    mutable std::mutex mutex_;
    std::array<Slot, DashboardSseBroadcaster::HardMaximumSubscriptions> slots_;
    const std::size_t maximumSubscriptions_;
    std::size_t liveSubscriptions_{};
    bool shutdown_{};
};

void DashboardSseSubscriptionState::close()
{
    std::shared_ptr<DashboardSseBroadcasterState> owner;
    std::size_t registeredSlot{UnboundSlot};
    {
        std::lock_guard lock{mutex_};
        if (closed_) {
            return;
        }
        closed_ = true;
        pendingFrame_.reset();
        readySink_.reset();
        lastSignaledSink_.reset();
        pendingSignalIssued_ = false;
        owner = owner_.lock();
        owner_.reset();
        registeredSlot = std::exchange(slot_, UnboundSlot);
    }

    if (owner != nullptr && registeredSlot != UnboundSlot) {
        owner->unregisterSubscription(registeredSlot, this);
    }
}

} // namespace Detail

namespace {

[[nodiscard]] Domain::Error closedError()
{
    return Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "The dashboard SSE broadcaster is closed.");
}

[[nodiscard]] Domain::Error internalError(const char* const message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, message);
}

} // namespace

DashboardSseSubscription::DashboardSseSubscription(
    std::shared_ptr<Detail::DashboardSseSubscriptionState> state) noexcept
    : state_{std::move(state)}
{
}

DashboardSseSubscription::~DashboardSseSubscription() noexcept
{
    close();
}

void DashboardSseSubscription::attachReadySink(
    std::weak_ptr<IDashboardSseReadySink> sink) noexcept
{
    try {
        state_->attachReadySink(std::move(sink));
    } catch (...) {
        // Notification attachment has no recoverable output boundary.
    }
}

DashboardSseFramePair::ImmutableFrame DashboardSseSubscription::takeLatest()
    noexcept
{
    try {
        return state_->takeLatest();
    } catch (...) {
        return {};
    }
}

std::size_t DashboardSseSubscription::pendingCount() const noexcept
{
    try {
        return state_->pendingCount();
    } catch (...) {
        return 0U;
    }
}

double DashboardSseSubscription::deliveryHz() const noexcept
{
    return state_->deliveryHz();
}

void DashboardSseSubscription::close() noexcept
{
    try {
        state_->close();
    } catch (...) {
        // Destruction and shutdown may not propagate synchronization errors.
    }
}

bool DashboardSseSubscription::isClosed() const noexcept
{
    try {
        return state_->isClosed();
    } catch (...) {
        return true;
    }
}

DashboardSseBroadcaster::DashboardSseBroadcaster(
    std::shared_ptr<Detail::DashboardSseBroadcasterState> state) noexcept
    : state_{std::move(state)}
{
}

DashboardSseBroadcaster::DashboardSseBroadcaster(
    DashboardSseBroadcaster&& other) noexcept
    : state_{std::exchange(other.state_, {})}
{
}

DashboardSseBroadcaster& DashboardSseBroadcaster::operator=(
    DashboardSseBroadcaster&& other) noexcept
{
    if (this != &other) {
        shutdown();
        state_ = std::exchange(other.state_, {});
    }
    return *this;
}

DashboardSseBroadcaster::~DashboardSseBroadcaster() noexcept
{
    shutdown();
}

Domain::Result<DashboardSseBroadcaster> DashboardSseBroadcaster::create(
    const std::size_t maximumSubscriptions) noexcept
{
    try {
        if (maximumSubscriptions == 0U ||
            maximumSubscriptions > HardMaximumSubscriptions) {
            return Domain::Result<DashboardSseBroadcaster>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard SSE subscription capacity must be between 1 "
                    "and 32."));
        }
        return Domain::Result<DashboardSseBroadcaster>::success(
            DashboardSseBroadcaster{
                std::make_shared<Detail::DashboardSseBroadcasterState>(
                    maximumSubscriptions)});
    } catch (...) {
        return Domain::Result<DashboardSseBroadcaster>::failure(internalError(
            "The dashboard SSE broadcaster could not be created."));
    }
}

Domain::Result<std::unique_ptr<DashboardSseSubscription>>
DashboardSseBroadcaster::subscribe(
    DashboardSseFramePair::ImmutableFrame initialFrame,
    const double deliveryHz) noexcept
{
    if (initialFrame == nullptr) {
        return Domain::Result<std::unique_ptr<DashboardSseSubscription>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A dashboard SSE subscription requires an initial frame."));
    }
    if (!std::isfinite(deliveryHz) ||
        deliveryHz < IDashboardSseSubscription::MinimumDeliveryHz ||
        deliveryHz > IDashboardSseSubscription::MaximumDeliveryHz) {
        return Domain::Result<std::unique_ptr<DashboardSseSubscription>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard SSE delivery frequency must be between 1 and 2 "
                "Hz."));
    }

    const auto owner = state_;
    if (owner == nullptr) {
        return Domain::Result<std::unique_ptr<DashboardSseSubscription>>::failure(
            closedError());
    }

    try {
        auto subscriptionState =
            std::make_shared<Detail::DashboardSseSubscriptionState>(
                owner, std::move(initialFrame), deliveryHz);
        std::unique_ptr<DashboardSseSubscription> subscription{
            new DashboardSseSubscription{subscriptionState}};
        const auto registration =
            owner->registerSubscription(subscriptionState);
        if (registration.status ==
            Detail::DashboardSseBroadcasterState::RegistrationStatus::Closed) {
            subscription->close();
            return Domain::Result<
                std::unique_ptr<DashboardSseSubscription>>::failure(
                closedError());
        }
        if (registration.status ==
            Detail::DashboardSseBroadcasterState::RegistrationStatus::
                CapacityReached) {
            subscription->close();
            return Domain::Result<
                std::unique_ptr<DashboardSseSubscription>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "Dashboard SSE subscription capacity is reached.",
                    true));
        }
        if (!subscriptionState->bindSlot(registration.slot)) {
            subscription->close();
            return Domain::Result<
                std::unique_ptr<DashboardSseSubscription>>::failure(
                closedError());
        }
        return Domain::Result<
            std::unique_ptr<DashboardSseSubscription>>::success(
            std::move(subscription));
    } catch (...) {
        return Domain::Result<std::unique_ptr<DashboardSseSubscription>>::failure(
            internalError(
                "The dashboard SSE subscription could not be created."));
    }
}

Domain::Result<void> DashboardSseBroadcaster::publish(
    DashboardSseFramePair::ImmutableFrame frame) noexcept
{
    if (frame == nullptr) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard SSE broadcaster cannot publish a null frame."));
    }

    const auto state = state_;
    if (state == nullptr) {
        return Domain::Result<void>::failure(closedError());
    }

    try {
        if (!state->publish(frame)) {
            return Domain::Result<void>::failure(closedError());
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "The dashboard SSE frame could not be published."));
    }
}

std::size_t DashboardSseBroadcaster::maximumSubscriptionCount() const noexcept
{
    const auto state = state_;
    return state == nullptr ? 0U : state->maximumSubscriptionCount();
}

std::size_t DashboardSseBroadcaster::liveSubscriptionCount() const noexcept
{
    const auto state = state_;
    if (state == nullptr) {
        return 0U;
    }
    try {
        return state->liveSubscriptionCount();
    } catch (...) {
        return 0U;
    }
}

bool DashboardSseBroadcaster::isShutdown() const noexcept
{
    const auto state = state_;
    if (state == nullptr) {
        return true;
    }
    try {
        return state->isShutdown();
    } catch (...) {
        return true;
    }
}

void DashboardSseBroadcaster::shutdown() noexcept
{
    const auto state = state_;
    if (state == nullptr) {
        return;
    }
    try {
        state->shutdown();
    } catch (...) {
        // The broadcaster owns no worker that can be abandoned. Destruction
        // remains non-throwing even if a synchronization primitive fails.
    }
}

} // namespace ForgeConductor::Dashboard
