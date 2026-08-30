#include "ManagerDashboardOperationalDataSource.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticLogTailReader.h"
#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsDashboardOperationalRepository.h"

#include <utility>

namespace ForgeConductor::Composition::Windows {
namespace {

[[nodiscard]] Domain::Error internalError()
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The Manager dashboard operational data source failed safely.");
}

} // namespace

ManagerDashboardOperationalDataSource::ManagerDashboardOperationalDataSource(
    Persistence::Windows::WindowsDashboardOperationalRepository&
        operationalRepository,
    Persistence::Windows::WindowsAgentSessionRepository& sessionRepository,
    Infrastructure::Windows::WindowsDiagnosticLogTailReader&
        diagnosticLogTailReader,
    Contracts::IClock& clock) noexcept
    : operationalRepository_{operationalRepository},
      sessionRepository_{sessionRepository},
      diagnosticLogTailReader_{diagnosticLogTailReader},
      clock_{clock}
{
}

Domain::Result<Application::DashboardOperationalSourceSnapshot>
ManagerDashboardOperationalDataSource::snapshot(
    const std::size_t maximumOpenSessions,
    const std::size_t maximumRecentSessions,
    const std::size_t maximumPresenceRecords,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto projection = operationalRepository_.snapshot(
            maximumOpenSessions,
            maximumRecentSessions,
            maximumPresenceRecords,
            context);
        if (!projection) {
            return Domain::Result<
                Application::DashboardOperationalSourceSnapshot>::failure(
                    std::move(projection).error());
        }

        auto source = std::move(projection).value();
        Application::DashboardOperationalSourceSnapshot result;
        result.openSessions = std::move(source.openSessions);
        result.recentSessions = std::move(source.recentSessions);
        result.presence.reserve(source.presence.size());
        for (auto& presence : source.presence) {
            result.presence.push_back(Dashboard::DashboardPresenceRecord{
                std::move(presence.clientId),
                std::move(presence.hostKind),
                presence.processId,
                std::move(presence.workingDirectory),
                presence.lastHeartbeat});
        }
        return Domain::Result<
            Application::DashboardOperationalSourceSnapshot>::success(
                std::move(result));
    } catch (...) {
        return Domain::Result<
            Application::DashboardOperationalSourceSnapshot>::failure(
                internalError());
    }
}

Domain::Result<std::vector<std::string>>
ManagerDashboardOperationalDataSource::diagnosticLines(
    const std::size_t maximumLines,
    const std::size_t maximumLineBytes,
    const std::size_t maximumAggregateBytes,
    const Domain::OperationContext& context) noexcept
{
    return diagnosticLogTailReader_.newestLines(
        maximumLines,
        maximumLineBytes,
        maximumAggregateBytes,
        context);
}

Domain::Result<Domain::AgentSession>
ManagerDashboardOperationalDataSource::closeSession(
    const Dashboard::DashboardSessionCloseRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return sessionRepository_.administrativelyClose(
        request.sessionId(), request.summary(), clock_.utcNow(), context);
}

} // namespace ForgeConductor::Composition::Windows
