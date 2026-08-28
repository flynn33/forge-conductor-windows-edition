#pragma once

#include "ForgeConductor/Dashboard/DashboardApplicationModels.h"
#include "ForgeConductor/Dashboard/DashboardSessionCloseRequest.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ForgeConductor::Dashboard {

// Read/write facade for the released operational dashboard surface. The
// concrete implementation composes existing application services and owns no
// transport, socket, platform handle, or JSON behavior.
class IDashboardOperationalService {
public:
    virtual ~IDashboardOperationalService() noexcept = default;

    // At most MaximumAgentSpecs, MaximumOpenSessions,
    // MaximumPresenceRecords, MaximumStatusAuditEvents, and MaximumToolNames
    // are accepted in the corresponding DashboardStatusData collections.
    [[nodiscard]] virtual Domain::Result<DashboardStatusData> status(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::DoctorReport> doctor(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::AgentSpec>> agents(
        const Domain::OperationContext& context) noexcept = 0;

    // open is capped at MaximumOpenSessions and recent at
    // MaximumRecentSessions. Both projections are observed in one service call
    // so the handler does not reconstruct a racy listing.
    [[nodiscard]] virtual Domain::Result<DashboardSessionListing> sessions(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::AuditEvent>> audit(
        const Domain::OperationContext& context) noexcept = 0;

    // Returns at most MaximumDiagnosticLines. Each line and their aggregate
    // text are bounded by DashboardApplicationLimits before crossing this
    // interface.
    [[nodiscard]] virtual Domain::Result<std::vector<std::string>>
    diagnosticLines(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::size_t> pruneSessions(
        const Domain::OperationContext& context) noexcept = 0;

    // This dashboard-administrative close is intentionally distinct from the
    // client-owned agent completion flow. The validated request supplies the
    // exact session and bounded summary to the operational implementation.
    [[nodiscard]] virtual Domain::Result<Domain::AgentSession> closeSession(
        const DashboardSessionCloseRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    // Idempotently rejects new facade calls. Injected repositories and
    // services remain owned by the composition root.
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Dashboard
