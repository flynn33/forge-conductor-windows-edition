#pragma once

#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <memory>

namespace ForgeConductor::Application {

// Converts provider budget observations into bounded, synchronous continuity
// actions. Each project admits at most one observation at a time; no thread or
// timer is owned by this service.
class ContinuityAutomation final
    : public Contracts::IContinuityAutomation {
public:
    ContinuityAutomation(
        Contracts::IContinuityCoordinator& coordinator,
        Contracts::IClock& clock);
    ~ContinuityAutomation() noexcept override;

    ContinuityAutomation(const ContinuityAutomation&) = delete;
    ContinuityAutomation& operator=(const ContinuityAutomation&) = delete;
    ContinuityAutomation(ContinuityAutomation&&) = delete;
    ContinuityAutomation& operator=(ContinuityAutomation&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ContinuityAutomationOutcome> observe(
        const Domain::ContinuityAutomationObservation& observation,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    [[nodiscard]] std::size_t trackedProjectCount() const noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
