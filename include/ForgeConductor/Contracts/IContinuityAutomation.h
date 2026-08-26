#pragma once

#include "ForgeConductor/Domain/ContinuityAutomationModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>

namespace ForgeConductor::Contracts {

class IContinuityAutomation {
public:
    virtual ~IContinuityAutomation() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityAutomationOutcome>
    observe(
        const Domain::ContinuityAutomationObservation& observation,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    [[nodiscard]] virtual std::size_t trackedProjectCount() const noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
