#include "DashboardListenerCompletionKeyLease.h"

#include "ForgeConductor/Domain/Error.h"

#include <array>
#include <atomic>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::size_t ListenerSlotCount = 2U;
constexpr std::size_t InvalidListenerSlot = ListenerSlotCount;

[[nodiscard]] Domain::Error listenerLeaseError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error listenerLeaseConflict()
{
    return listenerLeaseError(
        Domain::ErrorCodes::Conflict,
        "Both fixed dashboard listener completion keys are leased.",
        true);
}

[[nodiscard]] Domain::Error listenerLeaseInternalFailure()
{
    return listenerLeaseError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard listener completion-key lease failed safely.");
}

} // namespace

class DashboardListenerCompletionKeyLeaseState final {
public:
    explicit DashboardListenerCompletionKeyLeaseState(
        const DashboardFixedIocpKeyAuthority& authority) noexcept
        : keys_{authority.listenerSlotA(), authority.listenerSlotB()}
    {
    }

    [[nodiscard]] Domain::Result<DashboardListenerCompletionKeyLease>
    tryAcquire(
        const std::shared_ptr<DashboardListenerCompletionKeyLeaseState>& self)
    {
        using LeaseResult =
            Domain::Result<DashboardListenerCompletionKeyLease>;
        auto observed = heldSlots_.load(std::memory_order_acquire);
        for (;;) {
            std::size_t slot{};
            if ((observed & slotBit(0U)) == 0U) {
                slot = 0U;
            } else if ((observed & slotBit(1U)) == 0U) {
                slot = 1U;
            } else {
                return LeaseResult::failure(listenerLeaseConflict());
            }

            const auto desired = observed | slotBit(slot);
            if (heldSlots_.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return LeaseResult::success(
                    DashboardListenerCompletionKeyLease{self, slot});
            }
        }
    }

    [[nodiscard]] DashboardIoCompletionKey key(
        const std::size_t slot) const noexcept
    {
        return slot < keys_.size() ? keys_[slot]
                                   : DashboardIoCompletionKey{0U};
    }

    void release(const std::size_t slot) noexcept
    {
        if (slot >= keys_.size()) {
            std::terminate();
        }
        const auto bit = slotBit(slot);
        const auto previous =
            heldSlots_.fetch_and(~bit, std::memory_order_acq_rel);
        if ((previous & bit) == 0U) {
            std::terminate();
        }
    }

private:
    [[nodiscard]] static constexpr std::uint32_t slotBit(
        const std::size_t slot) noexcept
    {
        return std::uint32_t{1U} << slot;
    }

    const std::array<DashboardIoCompletionKey, ListenerSlotCount> keys_;
    std::atomic_uint32_t heldSlots_{};
};

DashboardListenerCompletionKeyLease::DashboardListenerCompletionKeyLease(
    std::shared_ptr<DashboardListenerCompletionKeyLeaseState> state,
    const std::size_t slot) noexcept
    : state_{std::move(state)}, slot_{slot}
{
}

DashboardListenerCompletionKeyLease::DashboardListenerCompletionKeyLease(
    DashboardListenerCompletionKeyLease&& other) noexcept
    : state_{std::move(other.state_)},
      slot_{std::exchange(other.slot_, InvalidListenerSlot)}
{
}

DashboardListenerCompletionKeyLease::~DashboardListenerCompletionKeyLease()
    noexcept
{
    if (state_ != nullptr) {
        state_->release(slot_);
    }
}

bool DashboardListenerCompletionKeyLease::ownsSlot() const noexcept
{
    return state_ != nullptr && slot_ < ListenerSlotCount;
}

DashboardIoCompletionKey
DashboardListenerCompletionKeyLease::completionKey() const noexcept
{
    return state_ != nullptr ? state_->key(slot_)
                             : DashboardIoCompletionKey{0U};
}

Domain::Result<std::unique_ptr<DashboardListenerCompletionKeyLeasePool>>
DashboardListenerCompletionKeyLeasePool::create(
    const DashboardFixedIocpKeyAuthority& authority) noexcept
{
    using CreationResult = Domain::Result<std::unique_ptr<
        DashboardListenerCompletionKeyLeasePool>>;
    try {
        auto state =
            std::make_shared<DashboardListenerCompletionKeyLeaseState>(
                authority);
        return CreationResult::success(
            std::unique_ptr<DashboardListenerCompletionKeyLeasePool>{
                new DashboardListenerCompletionKeyLeasePool{
                    std::move(state)}});
    } catch (...) {
        return CreationResult::failure(listenerLeaseInternalFailure());
    }
}

DashboardListenerCompletionKeyLeasePool::
    DashboardListenerCompletionKeyLeasePool(
        std::shared_ptr<DashboardListenerCompletionKeyLeaseState> state)
        noexcept
    : state_{std::move(state)}
{
}

Domain::Result<DashboardListenerCompletionKeyLease>
DashboardListenerCompletionKeyLeasePool::tryAcquire() const noexcept
{
    try {
        return state_->tryAcquire(state_);
    } catch (...) {
        return Domain::Result<DashboardListenerCompletionKeyLease>::failure(
            listenerLeaseInternalFailure());
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
