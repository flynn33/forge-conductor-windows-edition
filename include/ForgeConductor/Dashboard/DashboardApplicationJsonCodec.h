#pragma once

#include "ForgeConductor/Dashboard/DashboardApplicationModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ForgeConductor::Dashboard {

// Pure, platform-neutral projections for the non-telemetry dashboard routes.
// Every operation validates its complete source value before returning a
// bounded UTF-8 JSON document. No partial document escapes on failure.
class DashboardApplicationJsonCodec final {
public:
    static constexpr std::size_t MaximumResponseBytes = 2'080'768U;

    [[nodiscard]] static Domain::Result<std::string> encodeHealth(
        const DashboardTelemetryHealth& health,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeStatus(
        const DashboardApplicationStatus& status,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeDoctor(
        const Domain::DoctorReport& report,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeAgents(
        const std::vector<Domain::AgentSpec>& agents,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeSessions(
        const DashboardSessionListing& sessions,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeAudit(
        const std::vector<Domain::AuditEvent>& events,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeDiagnostics(
        const std::vector<std::string>& lines,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string>
    encodePruneAcknowledgement(
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string> encodeClosedSession(
        const Domain::AgentSession& session,
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;

    [[nodiscard]] static Domain::Result<std::string>
    encodeShutdownAcknowledgement(
        std::size_t maximumBytes = MaximumResponseBytes) noexcept;
};

} // namespace ForgeConductor::Dashboard
