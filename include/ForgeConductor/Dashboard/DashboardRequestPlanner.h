#pragma once

#include "ForgeConductor/Dashboard/DashboardRequestPolicy.h"
#include "ForgeConductor/Dashboard/DashboardRouteCatalog.h"
#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"
#include "ForgeConductor/Dashboard/DashboardStreamQueryDecoder.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <cstdint>
#include <utility>
#include <variant>

namespace ForgeConductor::Dashboard {

enum class DashboardRequestPlanKind : std::uint8_t {
    Rejected,
    Dispatch,
    StaticResource,
    ServerSentEvents,
};

// Immutable result of applying request policy, route classification, and all
// route-specific decoding. It owns every derived value needed by a later
// handler and borrows no request storage.
class DashboardRequestPlan final {
public:
    DashboardRequestPlan(const DashboardRequestPlan&) = default;
    DashboardRequestPlan(DashboardRequestPlan&&) noexcept = default;
    DashboardRequestPlan& operator=(const DashboardRequestPlan&) = delete;
    DashboardRequestPlan& operator=(DashboardRequestPlan&&) = delete;

    [[nodiscard]] DashboardRequestPlanKind kind() const noexcept;

    [[nodiscard]] const DashboardHttpRejection* rejection() const noexcept;

    // Present for every non-rejected plan.
    [[nodiscard]] const DashboardRouteMatch* route() const noexcept;

    // Present only when kind() is StaticResource.
    [[nodiscard]] const DashboardStaticResourcePath* staticResource()
        const noexcept;

    // Present only when kind() is ServerSentEvents.
    [[nodiscard]] const DashboardStreamRateSelection* streamRate()
        const noexcept;

private:
    friend class DashboardRequestPlanner;

    struct StaticResourcePlan final {
        DashboardRouteMatch route;
        DashboardStaticResourcePath resource;
    };

    struct ServerSentEventsPlan final {
        DashboardRouteMatch route;
        DashboardStreamRateSelection rate;
    };

    explicit DashboardRequestPlan(DashboardHttpRejection rejection) noexcept
        : value_{std::move(rejection)}
    {
    }

    explicit DashboardRequestPlan(DashboardRouteMatch route) noexcept
        : value_{route}
    {
    }

    DashboardRequestPlan(
        DashboardRouteMatch route,
        DashboardStaticResourcePath resource) noexcept
        : value_{StaticResourcePlan{route, std::move(resource)}}
    {
    }

    DashboardRequestPlan(
        DashboardRouteMatch route,
        const DashboardStreamRateSelection rate) noexcept
        : value_{ServerSentEventsPlan{route, rate}}
    {
    }

    std::variant<
        DashboardHttpRejection,
        DashboardRouteMatch,
        StaticResourcePlan,
        ServerSentEventsPlan>
        value_;
};

// Pure application-boundary planner. Transport code must call this only after
// the HTTP parser has accepted a complete request.
class DashboardRequestPlanner final {
public:
    [[nodiscard]] static DashboardRequestPlan plan(
        const DashboardRequestPolicy& policy,
        const DashboardHttpRequest& request,
        bool operationalServiceActive,
        const Domain::ResourceBudgets& budgets) noexcept;
};

} // namespace ForgeConductor::Dashboard
