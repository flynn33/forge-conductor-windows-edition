#pragma once

#include "ForgeConductor/Contracts/INativeSessionHostServices.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace ForgeConductor::SessionHost {

// Bounded admission queue between handoff bootstrap and the application-owned
// continuation executor. Accepted provider/handoff bindings remain remembered
// after dequeue so a replay cannot execute the same continuation twice.
class BoundedLogicalContinuationQueue final
    : public Contracts::INativeLogicalContinuationScheduler {
public:
    static constexpr std::size_t DefaultCapacity = 128U;

    explicit BoundedLogicalContinuationQueue(
        std::size_t capacity = DefaultCapacity);
    ~BoundedLogicalContinuationQueue() noexcept override;

    BoundedLogicalContinuationQueue(
        const BoundedLogicalContinuationQueue&) = delete;
    BoundedLogicalContinuationQueue& operator=(
        const BoundedLogicalContinuationQueue&) = delete;
    BoundedLogicalContinuationQueue(
        BoundedLogicalContinuationQueue&&) = delete;
    BoundedLogicalContinuationQueue& operator=(
        BoundedLogicalContinuationQueue&&) = delete;

    [[nodiscard]] Domain::Result<void> schedule(
        const Domain::NativeLogicalContinuation& continuation,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<
        std::optional<Domain::NativeLogicalContinuation>> takeNext(
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] std::size_t pendingCount() const noexcept;

    void cancel(
        const Domain::ProviderSessionId& sessionId) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::SessionHost
