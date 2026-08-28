#pragma once

#include "ForgeConductor/Domain/AgentModels.h"

#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {

// Immutable application-boundary value produced only after the dashboard
// mutation body has passed strict schema, text, and identifier validation.
class DashboardSessionCloseRequest final {
public:
    static constexpr std::string_view DefaultSummary{
        "Closed from dashboard"};

    DashboardSessionCloseRequest(
        Domain::SessionId sessionId,
        std::string summary)
        : sessionId_{std::move(sessionId)}, summary_{std::move(summary)}
    {
    }

    DashboardSessionCloseRequest(const DashboardSessionCloseRequest&) = default;
    DashboardSessionCloseRequest(DashboardSessionCloseRequest&&) noexcept = default;
    DashboardSessionCloseRequest& operator=(
        const DashboardSessionCloseRequest&) = delete;
    DashboardSessionCloseRequest& operator=(
        DashboardSessionCloseRequest&&) = delete;

    [[nodiscard]] const Domain::SessionId& sessionId() const noexcept
    {
        return sessionId_;
    }

    [[nodiscard]] const std::string& summary() const noexcept
    {
        return summary_;
    }

private:
    Domain::SessionId sessionId_;
    std::string summary_;
};

} // namespace ForgeConductor::Dashboard
