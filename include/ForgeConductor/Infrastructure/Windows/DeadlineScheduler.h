#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <atomic>

namespace ForgeConductor::Infrastructure::Windows {

// The current contract is an admission gate, not a sleeping/timer API. The
// composition root must keep the injected clock alive longer than this object.
class DeadlineScheduler final : public Contracts::IDeadlineScheduler {
public:
    explicit DeadlineScheduler(const Contracts::IClock& clock) noexcept;

    DeadlineScheduler(const DeadlineScheduler&) = delete;
    DeadlineScheduler& operator=(const DeadlineScheduler&) = delete;
    DeadlineScheduler(DeadlineScheduler&&) = delete;
    DeadlineScheduler& operator=(DeadlineScheduler&&) = delete;

    [[nodiscard]] Domain::Result<void> waitUntil(
        const Domain::OperationContext& context) noexcept override;
    void shutdown() noexcept override;

private:
    const Contracts::IClock& clock_;
    std::atomic_bool shutdown_{false};
};

} // namespace ForgeConductor::Infrastructure::Windows
