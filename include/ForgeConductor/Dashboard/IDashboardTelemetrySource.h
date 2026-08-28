#pragma once

#include "ForgeConductor/Dashboard/DashboardApplicationModels.h"
#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Dashboard/DashboardStreamQueryDecoder.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <memory>

namespace ForgeConductor::Dashboard {

// Application-owned telemetry boundary shared by every dashboard route. The
// source observes the existing collector; a handler must never start another
// collector or install a competing consumer directly.
class IDashboardTelemetrySource {
public:
    virtual ~IDashboardTelemetrySource() noexcept = default;

    [[nodiscard]] virtual Domain::Result<DashboardTelemetryHealth> health(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<DashboardTelemetryObservation> latest(
        const Domain::OperationContext& context) noexcept = 0;

    // A successful subscription is non-null, reports rate.deliveryHz(), and is
    // seeded with the latest immutable compact/full frame pair before return.
    // The returned subscription remains the caller's sole lifetime owner and
    // retains a capacity-one latest-value mailbox.
    [[nodiscard]] virtual Domain::Result<
        std::unique_ptr<IDashboardSseSubscription>>
    subscribe(
        const DashboardStreamRateSelection& rate,
        const Domain::OperationContext& context) noexcept = 0;

    // Idempotently rejects new observations/subscriptions and releases this
    // adapter's publication ownership. Injected collector ownership remains
    // with the composition root.
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Dashboard
