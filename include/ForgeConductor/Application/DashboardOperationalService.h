#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include "ForgeConductor/Dashboard/DashboardApplicationModels.h"
#include "ForgeConductor/Dashboard/DashboardSessionCloseRequest.h"
#include "ForgeConductor/Dashboard/IDashboardOperationalService.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ForgeConductor::Application {

// One central-store observation used by both dashboard status and session
// listing. Implementations perform the session and presence reads under one
// repository snapshot. openSessions is complete up to maximumOpenSessions;
// exceeding that bound is an error. recentSessions is the intentional newest-N
// projection requested by maximumRecentSessions.
struct DashboardOperationalSourceSnapshot final {
    std::vector<Domain::AgentSession> openSessions;
    std::vector<Domain::AgentSession> recentSessions;
    std::vector<Dashboard::DashboardPresenceRecord> presence;
};

// Narrow application-owned port for released dashboard operations not exposed
// by the existing write-oriented presence and agent-session contracts. It owns
// no lifecycle for the underlying database or diagnostic sink.
class IDashboardOperationalDataSource {
public:
    virtual ~IDashboardOperationalDataSource() noexcept = default;

    [[nodiscard]] virtual Domain::Result<DashboardOperationalSourceSnapshot>
    snapshot(
        std::size_t maximumOpenSessions,
        std::size_t maximumRecentSessions,
        std::size_t maximumPresenceRecords,
        const Domain::OperationContext& context) noexcept = 0;

    // Returns the newest lines in their original order. The implementation
    // must fail rather than split or truncate a source line.
    [[nodiscard]] virtual Domain::Result<std::vector<std::string>>
    diagnosticLines(
        std::size_t maximumLines,
        std::size_t maximumLineBytes,
        std::size_t maximumAggregateBytes,
        const Domain::OperationContext& context) noexcept = 0;

    // Atomic administrative close preserving the released dashboard behavior:
    // any existing session becomes closed and the validated summary is stored
    // verbatim. Active projections are cleared in the same transaction.
    [[nodiscard]] virtual Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest& request,
        const Domain::OperationContext& context) noexcept = 0;
};

struct DashboardOperationalServiceDependencies final {
    Contracts::IAgentCatalog& agentCatalog;
    Contracts::IAgentSessionService& agentSessions;
    Contracts::IAuditRepository& auditRepository;
    Contracts::IDoctorService& doctorService;
    Contracts::IRuntimeDiagnostics& runtimeDiagnostics;
    Contracts::IToolCatalog& toolCatalog;
    Contracts::IClock& clock;
    IDashboardOperationalDataSource& dataSource;
};

// Application-owned facade for the complete non-manager operational dashboard
// surface. Its admission lifecycle is independent of the manager's paused
// operational-service state, so /api/status remains readable while paused.
class DashboardOperationalService final
    : public Dashboard::IDashboardOperationalService {
public:
    explicit DashboardOperationalService(
        DashboardOperationalServiceDependencies dependencies);
    ~DashboardOperationalService() noexcept override;

    DashboardOperationalService(const DashboardOperationalService&) = delete;
    DashboardOperationalService& operator=(
        const DashboardOperationalService&) = delete;
    DashboardOperationalService(DashboardOperationalService&&) = delete;
    DashboardOperationalService& operator=(
        DashboardOperationalService&&) = delete;

    [[nodiscard]] Domain::Result<Dashboard::DashboardStatusData> status(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::DoctorReport> doctor(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> agents(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Dashboard::DashboardSessionListing> sessions(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> audit(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::size_t> pruneSessions(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest& request,
        const Domain::OperationContext& context) noexcept override;

    // Owner-only drain boundary. Composition first closes handler admission
    // and joins every caller, then invokes shutdown from a thread holding no
    // facade call before destroying this object or its dependencies.
    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
