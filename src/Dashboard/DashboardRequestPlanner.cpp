#include "ForgeConductor/Dashboard/DashboardRequestPlanner.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {
namespace {

[[nodiscard]] constexpr std::uint8_t bits(
    const DashboardAllowedMethods methods) noexcept
{
    return static_cast<std::uint8_t>(methods);
}

[[nodiscard]] constexpr std::string_view allowHeaderValue(
    const DashboardAllowedMethods methods) noexcept
{
    constexpr auto Get = bits(DashboardAllowedMethods::Get);
    constexpr auto Post = bits(DashboardAllowedMethods::Post);
    constexpr auto Put = bits(DashboardAllowedMethods::Put);

    switch (bits(methods) & (Get | Post | Put)) {
    case Get:
        return "GET";
    case Post:
        return "POST";
    case Put:
        return "PUT";
    case Get | Post:
        return "GET, POST";
    case Get | Put:
        return "GET, PUT";
    case Post | Put:
        return "POST, PUT";
    case Get | Post | Put:
        return "GET, POST, PUT";
    default:
        return {};
    }
}

[[nodiscard]] DashboardHttpRejection rejection(
    const std::uint16_t status,
    std::string code,
    std::string message,
    const DashboardAllowedMethods allowedMethods =
        DashboardAllowedMethods::None)
{
    std::vector<DashboardHttpHeader> responseHeaders;
    const auto allow = allowHeaderValue(allowedMethods);
    if (!allow.empty()) {
        responseHeaders.push_back({"allow", std::string{allow}});
    }
    return DashboardHttpRejection{
        status,
        std::move(code),
        std::move(message),
        std::move(responseHeaders)};
}

[[nodiscard]] DashboardHttpRejection notFound()
{
    return rejection(
        404U,
        "not_found",
        "The requested dashboard resource was not found.");
}

[[nodiscard]] DashboardHttpRejection internalFailure()
{
    return rejection(
        500U,
        "internal_failure",
        "Dashboard request planning failed.");
}

} // namespace

DashboardRequestPlanKind DashboardRequestPlan::kind() const noexcept
{
    switch (value_.index()) {
    case 0U:
        return DashboardRequestPlanKind::Rejected;
    case 1U:
        return DashboardRequestPlanKind::Dispatch;
    case 2U:
        return DashboardRequestPlanKind::StaticResource;
    default:
        return DashboardRequestPlanKind::ServerSentEvents;
    }
}

const DashboardHttpRejection* DashboardRequestPlan::rejection() const noexcept
{
    return std::get_if<DashboardHttpRejection>(&value_);
}

const DashboardRouteMatch* DashboardRequestPlan::route() const noexcept
{
    if (const auto* route = std::get_if<DashboardRouteMatch>(&value_)) {
        return route;
    }
    if (const auto* resource = std::get_if<StaticResourcePlan>(&value_)) {
        return &resource->route;
    }
    if (const auto* stream = std::get_if<ServerSentEventsPlan>(&value_)) {
        return &stream->route;
    }
    return nullptr;
}

const DashboardStaticResourcePath* DashboardRequestPlan::staticResource()
    const noexcept
{
    const auto* resource = std::get_if<StaticResourcePlan>(&value_);
    return resource == nullptr ? nullptr : &resource->resource;
}

const DashboardStreamRateSelection* DashboardRequestPlan::streamRate()
    const noexcept
{
    const auto* stream = std::get_if<ServerSentEventsPlan>(&value_);
    return stream == nullptr ? nullptr : &stream->rate;
}

DashboardRequestPlan DashboardRequestPlanner::plan(
    const DashboardRequestPolicy& policy,
    const DashboardHttpRequest& request,
    const bool operationalServiceActive,
    const Domain::ResourceBudgets& budgets) noexcept
{
    try {
        if (auto policyRejection = policy.rejectionFor(request)) {
            return DashboardRequestPlan{std::move(*policyRejection)};
        }

        const auto route = DashboardRouteCatalog::classify(
            request.method(), request.target(), operationalServiceActive);
        switch (route.disposition()) {
        case DashboardRouteDisposition::MethodNotAllowed:
            return DashboardRequestPlan{rejection(
                405U,
                "method_not_allowed",
                "The method is not allowed for this dashboard route.",
                route.allowedMethods())};
        case DashboardRouteDisposition::ServiceStopped:
            return DashboardRequestPlan{rejection(
                503U,
                "service_stopped",
                "The operational service is stopped.")};
        case DashboardRouteDisposition::NotFound:
        case DashboardRouteDisposition::Unavailable:
            return DashboardRequestPlan{notFound()};
        case DashboardRouteDisposition::Dispatch:
            break;
        }

        if (route.surface() == DashboardRouteSurface::StaticResource) {
            auto resource = DashboardStaticResourcePath::decode(request.path());
            if (!resource) {
                return DashboardRequestPlan{notFound()};
            }
            return DashboardRequestPlan{
                route, std::move(resource).value()};
        }

        if (route.surface() == DashboardRouteSurface::ServerSentEvents) {
            const auto decoded = DashboardStreamQueryDecoder::decode(
                route.query(), request.target(), budgets);
            if (!decoded) {
                return DashboardRequestPlan{rejection(
                    400U,
                    "invalid_query",
                    "The dashboard stream query is invalid.")};
            }
            return DashboardRequestPlan{route, *decoded.selection()};
        }

        return DashboardRequestPlan{route};
    } catch (...) {
        return DashboardRequestPlan{internalFailure()};
    }
}

} // namespace ForgeConductor::Dashboard
