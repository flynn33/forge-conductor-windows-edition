#include "ForgeConductor/Dashboard/DashboardRouteCatalog.h"

#include <cstdint>
#include <string_view>

namespace ForgeConductor::Dashboard {
namespace {

struct RouteDescriptor final {
    DashboardRouteId id{DashboardRouteId::Unknown};
    DashboardRouteSurface surface{DashboardRouteSurface::Unknown};
    DashboardAllowedMethods allowedMethods{DashboardAllowedMethods::None};
    DashboardServiceRequirement serviceRequirement{
        DashboardServiceRequirement::Independent};
    bool deliberatelyUnavailable{};
};

[[nodiscard]] constexpr DashboardAllowedMethods readOnly() noexcept
{
    return DashboardAllowedMethods::Get;
}

[[nodiscard]] constexpr DashboardAllowedMethods writeOnly() noexcept
{
    return DashboardAllowedMethods::Post;
}

[[nodiscard]] constexpr DashboardAllowedMethods settingsMethods() noexcept
{
    return DashboardAllowedMethods::Get |
        DashboardAllowedMethods::Post |
        DashboardAllowedMethods::Put;
}

[[nodiscard]] RouteDescriptor descriptorFor(
    const std::string_view rawPath) noexcept
{
    if (rawPath == "/") {
        return {
            DashboardRouteId::ShellRoot,
            DashboardRouteSurface::Shell,
            readOnly()};
    }
    if (rawPath == "/index.html") {
        return {
            DashboardRouteId::ShellIndex,
            DashboardRouteSurface::Shell,
            readOnly()};
    }
    if (rawPath == "/control") {
        return {
            DashboardRouteId::ShellControl,
            DashboardRouteSurface::Shell,
            readOnly()};
    }
    if (rawPath == "/manager") {
        return {
            DashboardRouteId::ShellManager,
            DashboardRouteSurface::Shell,
            readOnly()};
    }
    if (rawPath.starts_with("/static/")) {
        return {
            DashboardRouteId::StaticResource,
            DashboardRouteSurface::StaticResource,
            readOnly()};
    }
    if (rawPath == "/api/health") {
        return {
            DashboardRouteId::TelemetryHealth,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/api/live") {
        return {
            DashboardRouteId::TelemetryLive,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/api/frame") {
        return {
            DashboardRouteId::TelemetryFrame,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/api/snapshot") {
        return {
            DashboardRouteId::TelemetrySnapshot,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/api/system") {
        return {
            DashboardRouteId::TelemetrySystem,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/api/forge") {
        return {
            DashboardRouteId::TelemetryForge,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/ping") {
        return {
            DashboardRouteId::TelemetryPing,
            DashboardRouteSurface::Telemetry,
            readOnly()};
    }
    if (rawPath == "/api/status") {
        return {
            DashboardRouteId::Status,
            DashboardRouteSurface::Status,
            readOnly()};
    }
    if (rawPath == "/api/stream") {
        return {
            DashboardRouteId::TelemetryStream,
            DashboardRouteSurface::ServerSentEvents,
            readOnly()};
    }
    if (rawPath == "/api/doctor") {
        return {
            DashboardRouteId::OperationalDoctor,
            DashboardRouteSurface::OperationalRead,
            readOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/agents") {
        return {
            DashboardRouteId::OperationalAgents,
            DashboardRouteSurface::OperationalRead,
            readOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/sessions") {
        return {
            DashboardRouteId::OperationalSessions,
            DashboardRouteSurface::OperationalRead,
            readOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/audit") {
        return {
            DashboardRouteId::OperationalAudit,
            DashboardRouteSurface::OperationalRead,
            readOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/diagnostics") {
        return {
            DashboardRouteId::OperationalDiagnostics,
            DashboardRouteSurface::OperationalRead,
            readOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/sessions/prune") {
        return {
            DashboardRouteId::OperationalSessionsPrune,
            DashboardRouteSurface::OperationalWrite,
            writeOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/sessions/close") {
        return {
            DashboardRouteId::OperationalSessionsClose,
            DashboardRouteSurface::OperationalWrite,
            writeOnly(),
            DashboardServiceRequirement::OperationalService};
    }
    if (rawPath == "/api/manager/status") {
        return {
            DashboardRouteId::ManagerStatus,
            DashboardRouteSurface::ManagerRead,
            readOnly()};
    }
    if (rawPath == "/api/manager/settings") {
        return {
            DashboardRouteId::ManagerSettings,
            DashboardRouteSurface::ManagerReadWrite,
            settingsMethods()};
    }
    if (rawPath == "/api/manager/start") {
        return {
            DashboardRouteId::ManagerStart,
            DashboardRouteSurface::ManagerWrite,
            writeOnly()};
    }
    if (rawPath == "/api/manager/stop") {
        return {
            DashboardRouteId::ManagerStop,
            DashboardRouteSurface::ManagerWrite,
            writeOnly()};
    }
    if (rawPath == "/api/manager/restart") {
        return {
            DashboardRouteId::ManagerRestart,
            DashboardRouteSurface::ManagerWrite,
            writeOnly()};
    }
    if (rawPath == "/api/manager/shutdown") {
        return {
            DashboardRouteId::ManagerShutdown,
            DashboardRouteSurface::ManagerWrite,
            writeOnly()};
    }
    if (rawPath == "/api/tools/call") {
        return {
            DashboardRouteId::UnavailableToolsCall,
            DashboardRouteSurface::Unavailable,
            writeOnly(),
            DashboardServiceRequirement::Independent,
            true};
    }
    return {};
}

[[nodiscard]] DashboardRouteQuery queryFor(
    const DashboardRouteId route,
    const std::string_view rawQuery,
    const bool hasQuery) noexcept
{
    if (route != DashboardRouteId::TelemetryStream || !hasQuery) {
        return DashboardRouteQuery::None;
    }

    const auto hasAnotherDelimiter = [](const std::string_view value) noexcept {
        return value.find('&') != std::string_view::npos ||
            value.find('?') != std::string_view::npos;
    };
    if (rawQuery.starts_with("hz=") &&
        !hasAnotherDelimiter(rawQuery) &&
        rawQuery.find('=', 3U) == std::string_view::npos) {
        return DashboardRouteQuery::StreamHertz;
    }
    if (rawQuery.starts_with("interval=") &&
        !hasAnotherDelimiter(rawQuery) &&
        rawQuery.find('=', 9U) == std::string_view::npos) {
        return DashboardRouteQuery::LegacyStreamInterval;
    }
    return DashboardRouteQuery::UnsupportedStreamQuery;
}

[[nodiscard]] DashboardRouteSurface resolvedSurface(
    const RouteDescriptor& descriptor,
    const std::string_view method) noexcept
{
    if (descriptor.id != DashboardRouteId::ManagerSettings) {
        return descriptor.surface;
    }
    if (method == "GET") {
        return DashboardRouteSurface::ManagerRead;
    }
    if (method == "POST" || method == "PUT") {
        return DashboardRouteSurface::ManagerWrite;
    }
    return DashboardRouteSurface::ManagerReadWrite;
}

[[nodiscard]] constexpr std::uint8_t bits(
    const DashboardAllowedMethods methods) noexcept
{
    return static_cast<std::uint8_t>(methods);
}

[[nodiscard]] DashboardAllowedMethods methodFlag(
    const std::string_view method) noexcept
{
    if (method == "GET") {
        return DashboardAllowedMethods::Get;
    }
    if (method == "POST") {
        return DashboardAllowedMethods::Post;
    }
    if (method == "PUT") {
        return DashboardAllowedMethods::Put;
    }
    return DashboardAllowedMethods::None;
}

[[nodiscard]] bool allows(
    const DashboardAllowedMethods allowed,
    const std::string_view method) noexcept
{
    const auto requested = methodFlag(method);
    return requested != DashboardAllowedMethods::None &&
        (bits(allowed) & bits(requested)) != 0U;
}

} // namespace

bool DashboardRouteMatch::allowsMethod(const std::string_view method) const noexcept
{
    return allows(allowedMethods_, method);
}

DashboardRouteMatch DashboardRouteCatalog::classify(
    const std::string_view method,
    const std::string_view target,
    const bool operationalServiceActive) noexcept
{
    const auto queryDelimiter = target.find('?');
    const bool hasQuery = queryDelimiter != std::string_view::npos;
    const auto rawPath = target.substr(0U, queryDelimiter);
    const auto rawQuery = hasQuery
        ? target.substr(queryDelimiter + 1U)
        : std::string_view{};

    const auto descriptor = descriptorFor(rawPath);
    const auto query = queryFor(descriptor.id, rawQuery, hasQuery);
    const auto surface = resolvedSurface(descriptor, method);

    DashboardRouteDisposition disposition{};
    // OPTIONS is globally unsupported. Apply that rule before path and service
    // disposition so every target receives the same 405 response.
    if (method == "OPTIONS") {
        disposition = DashboardRouteDisposition::MethodNotAllowed;
    } else if (descriptor.id == DashboardRouteId::Unknown) {
        disposition = DashboardRouteDisposition::NotFound;
    } else if (descriptor.deliberatelyUnavailable) {
        disposition = DashboardRouteDisposition::Unavailable;
    } else if (
        descriptor.serviceRequirement ==
            DashboardServiceRequirement::OperationalService &&
        !operationalServiceActive) {
        disposition = DashboardRouteDisposition::ServiceStopped;
    } else if (!allows(descriptor.allowedMethods, method)) {
        disposition = DashboardRouteDisposition::MethodNotAllowed;
    } else {
        disposition = DashboardRouteDisposition::Dispatch;
    }

    return DashboardRouteMatch{
        descriptor.id,
        surface,
        descriptor.allowedMethods,
        query,
        descriptor.serviceRequirement,
        disposition};
}

} // namespace ForgeConductor::Dashboard
