#pragma once

#include "ForgeConductor/Application/DashboardOperationalService.h"

namespace ForgeConductor::Contracts {
class IClock;
}

namespace ForgeConductor::Infrastructure::Windows {
class WindowsDiagnosticLogTailReader;
}

namespace ForgeConductor::Persistence::Windows {
class WindowsAgentSessionRepository;
class WindowsDashboardOperationalRepository;
}

namespace ForgeConductor::Composition::Windows {

// Outer Manager-composition adapter for the application-owned operational
// port. It owns no injected dependency and adds no independent lifecycle
// transition; the composition root retains every dependency until dashboard
// admission and operational calls have drained.
class ManagerDashboardOperationalDataSource final
    : public Application::IDashboardOperationalDataSource {
public:
    ManagerDashboardOperationalDataSource(
        Persistence::Windows::WindowsDashboardOperationalRepository&
            operationalRepository,
        Persistence::Windows::WindowsAgentSessionRepository& sessionRepository,
        Infrastructure::Windows::WindowsDiagnosticLogTailReader&
            diagnosticLogTailReader,
        Contracts::IClock& clock) noexcept;

    ~ManagerDashboardOperationalDataSource() noexcept override = default;

    ManagerDashboardOperationalDataSource(
        const ManagerDashboardOperationalDataSource&) = delete;
    ManagerDashboardOperationalDataSource& operator=(
        const ManagerDashboardOperationalDataSource&) = delete;
    ManagerDashboardOperationalDataSource(
        ManagerDashboardOperationalDataSource&&) = delete;
    ManagerDashboardOperationalDataSource& operator=(
        ManagerDashboardOperationalDataSource&&) = delete;

    [[nodiscard]] Domain::Result<
        Application::DashboardOperationalSourceSnapshot>
    snapshot(
        std::size_t maximumOpenSessions,
        std::size_t maximumRecentSessions,
        std::size_t maximumPresenceRecords,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        std::size_t maximumLines,
        std::size_t maximumLineBytes,
        std::size_t maximumAggregateBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest& request,
        const Domain::OperationContext& context) noexcept override;

private:
    Persistence::Windows::WindowsDashboardOperationalRepository&
        operationalRepository_;
    Persistence::Windows::WindowsAgentSessionRepository& sessionRepository_;
    Infrastructure::Windows::WindowsDiagnosticLogTailReader&
        diagnosticLogTailReader_;
    Contracts::IClock& clock_;
};

} // namespace ForgeConductor::Composition::Windows
