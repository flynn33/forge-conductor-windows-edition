#include "ForgeConductor/Dashboard/DashboardRouteCatalog.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

static_assert(std::is_final_v<Dashboard::DashboardRouteCatalog>);
static_assert(std::is_final_v<Dashboard::DashboardRouteMatch>);
static_assert(noexcept(Dashboard::DashboardRouteCatalog::classify(
    std::declval<std::string_view>(),
    std::declval<std::string_view>(),
    true)));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().allowsMethod(
        std::declval<std::string_view>())));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().id()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().surface()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().allowedMethods()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().query()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>()
        .serviceRequirement()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().disposition()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRouteMatch&>().dispatchable()));

[[nodiscard]] Dashboard::DashboardRouteMatch classify(
    const std::string_view method,
    const std::string_view target,
    const bool serviceActive = true) noexcept
{
    return Dashboard::DashboardRouteCatalog::classify(
        method, target, serviceActive);
}

void requireRoute(
    const std::string_view method,
    const std::string_view target,
    const Dashboard::DashboardRouteId id,
    const Dashboard::DashboardRouteSurface surface,
    const Dashboard::DashboardServiceRequirement serviceRequirement =
        Dashboard::DashboardServiceRequirement::Independent)
{
    const auto result = classify(method, target);
    REQUIRE(result.id() == id);
    REQUIRE(result.surface() == surface);
    REQUIRE(result.serviceRequirement() == serviceRequirement);
    REQUIRE(result.disposition() ==
            Dashboard::DashboardRouteDisposition::Dispatch);
    REQUIRE(result.dispatchable());
    REQUIRE(result.allowsMethod(method));
}

void classifiesTheCompleteCanonicalInventory()
{
    using Dashboard::DashboardRouteId;
    using Dashboard::DashboardRouteSurface;
    using Dashboard::DashboardServiceRequirement;

    struct RouteCase final {
        std::string_view method;
        std::string_view target;
        DashboardRouteId id;
        DashboardRouteSurface surface;
        DashboardServiceRequirement serviceRequirement{
            DashboardServiceRequirement::Independent};
    };

    constexpr std::array routes{
        RouteCase{"GET", "/", DashboardRouteId::ShellRoot,
                  DashboardRouteSurface::Shell},
        RouteCase{"GET", "/index.html", DashboardRouteId::ShellIndex,
                  DashboardRouteSurface::Shell},
        RouteCase{"GET", "/control", DashboardRouteId::ShellControl,
                  DashboardRouteSurface::Shell},
        RouteCase{"GET", "/manager", DashboardRouteId::ShellManager,
                  DashboardRouteSurface::Shell},
        RouteCase{"GET", "/static/app.js",
                  DashboardRouteId::StaticResource,
                  DashboardRouteSurface::StaticResource},
        RouteCase{"GET", "/api/health",
                  DashboardRouteId::TelemetryHealth,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/api/live", DashboardRouteId::TelemetryLive,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/api/frame", DashboardRouteId::TelemetryFrame,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/api/snapshot",
                  DashboardRouteId::TelemetrySnapshot,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/api/system", DashboardRouteId::TelemetrySystem,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/api/forge", DashboardRouteId::TelemetryForge,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/ping", DashboardRouteId::TelemetryPing,
                  DashboardRouteSurface::Telemetry},
        RouteCase{"GET", "/api/status", DashboardRouteId::Status,
                  DashboardRouteSurface::Status},
        RouteCase{"GET", "/api/stream", DashboardRouteId::TelemetryStream,
                  DashboardRouteSurface::ServerSentEvents},
        RouteCase{
            "GET", "/api/doctor", DashboardRouteId::OperationalDoctor,
            DashboardRouteSurface::OperationalRead,
            DashboardServiceRequirement::OperationalService},
        RouteCase{
            "GET", "/api/agents", DashboardRouteId::OperationalAgents,
            DashboardRouteSurface::OperationalRead,
            DashboardServiceRequirement::OperationalService},
        RouteCase{
            "GET", "/api/sessions", DashboardRouteId::OperationalSessions,
            DashboardRouteSurface::OperationalRead,
            DashboardServiceRequirement::OperationalService},
        RouteCase{
            "GET", "/api/audit", DashboardRouteId::OperationalAudit,
            DashboardRouteSurface::OperationalRead,
            DashboardServiceRequirement::OperationalService},
        RouteCase{
            "GET", "/api/diagnostics",
            DashboardRouteId::OperationalDiagnostics,
            DashboardRouteSurface::OperationalRead,
            DashboardServiceRequirement::OperationalService},
        RouteCase{
            "POST", "/api/sessions/prune",
            DashboardRouteId::OperationalSessionsPrune,
            DashboardRouteSurface::OperationalWrite,
            DashboardServiceRequirement::OperationalService},
        RouteCase{
            "POST", "/api/sessions/close",
            DashboardRouteId::OperationalSessionsClose,
            DashboardRouteSurface::OperationalWrite,
            DashboardServiceRequirement::OperationalService},
        RouteCase{"GET", "/api/manager/status",
                  DashboardRouteId::ManagerStatus,
                  DashboardRouteSurface::ManagerRead},
        RouteCase{"GET", "/api/manager/settings",
                  DashboardRouteId::ManagerSettings,
                  DashboardRouteSurface::ManagerRead},
        RouteCase{"POST", "/api/manager/settings",
                  DashboardRouteId::ManagerSettings,
                  DashboardRouteSurface::ManagerWrite},
        RouteCase{"PUT", "/api/manager/settings",
                  DashboardRouteId::ManagerSettings,
                  DashboardRouteSurface::ManagerWrite},
        RouteCase{"POST", "/api/manager/start",
                  DashboardRouteId::ManagerStart,
                  DashboardRouteSurface::ManagerWrite},
        RouteCase{"POST", "/api/manager/stop",
                  DashboardRouteId::ManagerStop,
                  DashboardRouteSurface::ManagerWrite},
        RouteCase{"POST", "/api/manager/restart",
                  DashboardRouteId::ManagerRestart,
                  DashboardRouteSurface::ManagerWrite},
        RouteCase{"POST", "/api/manager/shutdown",
                  DashboardRouteId::ManagerShutdown,
                  DashboardRouteSurface::ManagerWrite},
    };

    for (const auto& route : routes) {
        requireRoute(
            route.method,
            route.target,
            route.id,
            route.surface,
            route.serviceRequirement);
    }

    const auto emptyStatic = classify("GET", "/static/");
    REQUIRE(emptyStatic.id() == DashboardRouteId::StaticResource);
    REQUIRE(emptyStatic.dispatchable());

    const auto toolCall = classify("POST", "/api/tools/call");
    REQUIRE(toolCall.id() == DashboardRouteId::UnavailableToolsCall);
    REQUIRE(toolCall.surface() == DashboardRouteSurface::Unavailable);
    REQUIRE(toolCall.allowsMethod("POST"));
    REQUIRE(!toolCall.allowsMethod("GET"));
    REQUIRE(toolCall.disposition() ==
            Dashboard::DashboardRouteDisposition::Unavailable);
    REQUIRE(!toolCall.dispatchable());
}

void exposesExactAllowedMethodSets()
{
    constexpr std::array readRoutes{
        "/", "/index.html", "/control", "/manager", "/static/app.css",
        "/api/health", "/api/live", "/api/frame", "/api/snapshot",
        "/api/system", "/api/forge", "/ping", "/api/status",
        "/api/stream", "/api/doctor", "/api/agents", "/api/sessions",
        "/api/audit", "/api/diagnostics", "/api/manager/status"};
    constexpr std::array unsupportedMethods{
        "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "HEAD", "get",
        "M-SEARCH", ""};

    for (const auto path : readRoutes) {
        const auto accepted = classify("GET", path);
        REQUIRE(accepted.allowsMethod("GET"));
        REQUIRE(!accepted.allowsMethod("POST"));
        REQUIRE(!accepted.allowsMethod("PUT"));
        for (const auto method : unsupportedMethods) {
            const auto rejected = classify(method, path);
            REQUIRE(rejected.id() == accepted.id());
            REQUIRE(rejected.disposition() ==
                    Dashboard::DashboardRouteDisposition::MethodNotAllowed);
            REQUIRE(!rejected.dispatchable());
            REQUIRE(!rejected.allowsMethod(method));
        }
    }

    constexpr std::array writeRoutes{
        "/api/sessions/prune", "/api/sessions/close",
        "/api/manager/start", "/api/manager/stop",
        "/api/manager/restart", "/api/manager/shutdown"};
    for (const auto path : writeRoutes) {
        const auto accepted = classify("POST", path);
        REQUIRE(accepted.allowsMethod("POST"));
        REQUIRE(!accepted.allowsMethod("GET"));
        REQUIRE(!accepted.allowsMethod("PUT"));
        for (const auto method : {
                 "GET", "PUT", "PATCH", "DELETE", "OPTIONS", "HEAD",
                 "post", ""}) {
            const auto rejected = classify(method, path);
            REQUIRE(rejected.id() == accepted.id());
            REQUIRE(rejected.disposition() ==
                    Dashboard::DashboardRouteDisposition::MethodNotAllowed);
            REQUIRE(!rejected.dispatchable());
            REQUIRE(!rejected.allowsMethod(method));
        }
    }

    for (const auto method : {"GET", "POST", "PUT"}) {
        const auto accepted = classify(method, "/api/manager/settings");
        REQUIRE(accepted.dispatchable());
        REQUIRE(accepted.allowsMethod("GET"));
        REQUIRE(accepted.allowsMethod("POST"));
        REQUIRE(accepted.allowsMethod("PUT"));
    }
    for (const auto method : {
             "PATCH", "DELETE", "OPTIONS", "HEAD", "get", "post", ""}) {
        const auto rejected = classify(method, "/api/manager/settings");
        REQUIRE(rejected.id() == Dashboard::DashboardRouteId::ManagerSettings);
        REQUIRE(rejected.surface() ==
                Dashboard::DashboardRouteSurface::ManagerReadWrite);
        REQUIRE(rejected.disposition() ==
                Dashboard::DashboardRouteDisposition::MethodNotAllowed);
        REQUIRE(!rejected.allowsMethod(method));
    }

    for (const auto method : {"GET", "POST", "PUT", "PATCH", "DELETE"}) {
        const auto unavailable = classify(method, "/api/tools/call");
        REQUIRE(unavailable.id() ==
                Dashboard::DashboardRouteId::UnavailableToolsCall);
        REQUIRE(unavailable.disposition() ==
                Dashboard::DashboardRouteDisposition::Unavailable);
    }
}

void rejectsOptionsGloballyBeforeRouteDisposition()
{
    struct OptionsCase final {
        std::string_view target;
        bool serviceActive;
        Dashboard::DashboardRouteId id;
    };
    constexpr std::array cases{
        OptionsCase{"/", true, Dashboard::DashboardRouteId::ShellRoot},
        OptionsCase{
            "/api/stream?hz=2", true,
            Dashboard::DashboardRouteId::TelemetryStream},
        OptionsCase{
            "/api/manager/settings", true,
            Dashboard::DashboardRouteId::ManagerSettings},
        OptionsCase{
            "/api/doctor", false,
            Dashboard::DashboardRouteId::OperationalDoctor},
        OptionsCase{
            "/api/tools/call", true,
            Dashboard::DashboardRouteId::UnavailableToolsCall},
        OptionsCase{
            "/not-a-route", true,
            Dashboard::DashboardRouteId::Unknown},
    };

    for (const auto& entry : cases) {
        const auto result = classify(
            "OPTIONS", entry.target, entry.serviceActive);
        REQUIRE(result.id() == entry.id);
        REQUIRE(result.disposition() ==
                Dashboard::DashboardRouteDisposition::MethodNotAllowed);
        REQUIRE(!result.dispatchable());
        REQUIRE(!result.allowsMethod("OPTIONS"));
    }
}

void preservesServiceAvailabilityBoundaries()
{
    constexpr std::array independentRoutes{
        std::pair{"GET", "/"},
        std::pair{"GET", "/index.html"},
        std::pair{"GET", "/control"},
        std::pair{"GET", "/manager"},
        std::pair{"GET", "/static/app.js"},
        std::pair{"GET", "/api/health"},
        std::pair{"GET", "/api/live"},
        std::pair{"GET", "/api/frame"},
        std::pair{"GET", "/api/snapshot"},
        std::pair{"GET", "/api/system"},
        std::pair{"GET", "/api/forge"},
        std::pair{"GET", "/ping"},
        std::pair{"GET", "/api/status"},
        std::pair{"GET", "/api/stream?hz=2"},
        std::pair{"GET", "/api/manager/status"},
        std::pair{"GET", "/api/manager/settings"},
        std::pair{"POST", "/api/manager/settings"},
        std::pair{"PUT", "/api/manager/settings"},
        std::pair{"POST", "/api/manager/start"},
        std::pair{"POST", "/api/manager/stop"},
        std::pair{"POST", "/api/manager/restart"},
        std::pair{"POST", "/api/manager/shutdown"},
    };
    for (const auto& [method, target] : independentRoutes) {
        const auto result = classify(method, target, false);
        REQUIRE(result.serviceRequirement() ==
                Dashboard::DashboardServiceRequirement::Independent);
        REQUIRE(result.disposition() ==
                Dashboard::DashboardRouteDisposition::Dispatch);
        REQUIRE(result.dispatchable());
    }

    constexpr std::array operationalRoutes{
        std::pair{"GET", "/api/doctor"},
        std::pair{"GET", "/api/agents"},
        std::pair{"GET", "/api/sessions"},
        std::pair{"GET", "/api/audit"},
        std::pair{"GET", "/api/diagnostics"},
        std::pair{"POST", "/api/sessions/prune"},
        std::pair{"POST", "/api/sessions/close"},
    };
    for (const auto& [method, target] : operationalRoutes) {
        const auto result = classify(method, target, false);
        REQUIRE(result.serviceRequirement() ==
                Dashboard::DashboardServiceRequirement::OperationalService);
        REQUIRE(result.disposition() ==
                Dashboard::DashboardRouteDisposition::ServiceStopped);
        REQUIRE(!result.dispatchable());
        REQUIRE(result.allowsMethod(method));
    }

    const auto stoppedPrecedesMethod = classify(
        "DELETE", "/api/doctor", false);
    REQUIRE(stoppedPrecedesMethod.disposition() ==
            Dashboard::DashboardRouteDisposition::ServiceStopped);
    REQUIRE(!stoppedPrecedesMethod.allowsMethod("DELETE"));

    const auto unknown = classify("GET", "/api/not-a-route", false);
    REQUIRE(unknown.disposition() ==
            Dashboard::DashboardRouteDisposition::NotFound);
    const auto unavailable = classify("POST", "/api/tools/call", false);
    REQUIRE(unavailable.disposition() ==
            Dashboard::DashboardRouteDisposition::Unavailable);
}

void classifiesOnlyTheDocumentedStreamQueries()
{
    using Dashboard::DashboardRouteDisposition;
    using Dashboard::DashboardRouteId;
    using Dashboard::DashboardRouteQuery;

    const auto noQuery = classify("GET", "/api/stream");
    REQUIRE(noQuery.id() == DashboardRouteId::TelemetryStream);
    REQUIRE(noQuery.query() == DashboardRouteQuery::None);
    REQUIRE(noQuery.dispatchable());

    for (const auto target : {
             "/api/stream?hz=", "/api/stream?hz=1",
             "/api/stream?hz=2.0", "/api/stream?hz=-1",
             "/api/stream?hz=1e0", "/api/stream?hz=%32"}) {
        const auto result = classify("GET", target);
        REQUIRE(result.id() == DashboardRouteId::TelemetryStream);
        REQUIRE(result.query() == DashboardRouteQuery::StreamHertz);
        REQUIRE(result.disposition() == DashboardRouteDisposition::Dispatch);
    }

    for (const auto target : {
             "/api/stream?interval=", "/api/stream?interval=2",
             "/api/stream?interval=0.016", "/api/stream?interval=%32"}) {
        const auto result = classify("GET", target);
        REQUIRE(result.id() == DashboardRouteId::TelemetryStream);
        REQUIRE(result.query() == DashboardRouteQuery::LegacyStreamInterval);
        REQUIRE(result.disposition() == DashboardRouteDisposition::Dispatch);
    }

    for (const auto target : {
             "/api/stream?", "/api/stream?HZ=2", "/api/stream?hZ=2",
             "/api/stream?%68z=2", "/api/stream?rate=2",
             "/api/stream?intervals=2", "/api/stream?hz=1&interval=2",
             "/api/stream?interval=2&hz=1", "/api/stream?hz=1&",
             "/api/stream?hz==1", "/api/stream?interval==2",
             "/api/stream?hz=1?interval=2"}) {
        const auto result = classify("GET", target);
        REQUIRE(result.id() == DashboardRouteId::TelemetryStream);
        REQUIRE(result.query() ==
                DashboardRouteQuery::UnsupportedStreamQuery);
        REQUIRE(result.disposition() == DashboardRouteDisposition::Dispatch);
    }

    for (const auto target : {
             "/?hz=2", "/static/app.js?hz=2", "/api/status?hz=2",
             "/api/live?interval=2", "/api/doctor?hz=2",
             "/api/manager/status?hz=2"}) {
        const auto result = classify("GET", target);
        REQUIRE(result.query() == DashboardRouteQuery::None);
        REQUIRE(result.dispatchable());
    }
}

void preservesUndecodedExactRawPathIdentity()
{
    constexpr std::array unknownTargets{
        "", "api/status", "//api/status", "/API/status", "/api/Status",
        "/api/status/", "/api//status", "/api/%73tatus",
        "/%61pi/status", "/api/status%2f", "/api/status#fragment",
        "/static", "/staticx/app.js", "/static%2fapp.js",
        "/api/stream/", "/api/streaming", "/api/%73tream?hz=2",
        "/api/sessions/prune/", "/api/sessions/%70rune",
        "/api/manager/settings/", "/api/manager/%73ettings",
        "/api/tools/call/", "/api/tools/%63all", "/unknown"};

    for (const auto target : unknownTargets) {
        const auto result = classify("GET", target);
        REQUIRE(result.id() == Dashboard::DashboardRouteId::Unknown);
        REQUIRE(result.surface() == Dashboard::DashboardRouteSurface::Unknown);
        REQUIRE(result.allowedMethods() ==
                Dashboard::DashboardAllowedMethods::None);
        REQUIRE(result.serviceRequirement() ==
                Dashboard::DashboardServiceRequirement::Independent);
        REQUIRE(result.query() == Dashboard::DashboardRouteQuery::None);
        REQUIRE(result.disposition() ==
                Dashboard::DashboardRouteDisposition::NotFound);
        REQUIRE(!result.dispatchable());
        REQUIRE(!result.allowsMethod("GET"));
    }

    const auto staticEncodedTraversal = classify(
        "GET", "/static/%2e%2e/%2fapi/status");
    REQUIRE(staticEncodedTraversal.id() ==
            Dashboard::DashboardRouteId::StaticResource);
    REQUIRE(staticEncodedTraversal.dispatchable());

    const auto staticRawTraversal = classify("GET", "/static/../api/status");
    REQUIRE(staticRawTraversal.id() ==
            Dashboard::DashboardRouteId::StaticResource);
    REQUIRE(staticRawTraversal.dispatchable());

    const auto statusQuery = classify(
        "GET", "/api/status?next=/api/tools/call?hz=2");
    REQUIRE(statusQuery.id() == Dashboard::DashboardRouteId::Status);
    REQUIRE(statusQuery.query() == Dashboard::DashboardRouteQuery::None);

    const auto unavailableQuery = classify(
        "POST", "/api/tools/call?next=/api/status");
    REQUIRE(unavailableQuery.id() ==
            Dashboard::DashboardRouteId::UnavailableToolsCall);
    REQUIRE(unavailableQuery.disposition() ==
            Dashboard::DashboardRouteDisposition::Unavailable);

    const std::string embeddedNull{"/api/status\0/extra", 18U};
    const auto embeddedNullResult = classify("GET", embeddedNull);
    REQUIRE(embeddedNullResult.id() == Dashboard::DashboardRouteId::Unknown);
    REQUIRE(embeddedNullResult.disposition() ==
            Dashboard::DashboardRouteDisposition::NotFound);
}

void remainsNoAllocationAndDeterministicAtTheParserTargetBound()
{
    std::string longUnknown(8192U, 'x');
    longUnknown.front() = '/';
    const auto first = classify("GET", longUnknown, true);
    const auto second = classify("GET", longUnknown, true);
    REQUIRE(first == second);
    REQUIRE(first.id() == Dashboard::DashboardRouteId::Unknown);
    REQUIRE(first.disposition() ==
            Dashboard::DashboardRouteDisposition::NotFound);

    std::string longStatic = "/static/";
    longStatic.append(8192U - longStatic.size(), 'a');
    const auto active = classify("GET", longStatic, true);
    const auto stopped = classify("GET", longStatic, false);
    REQUIRE(active == stopped);
    REQUIRE(active.id() == Dashboard::DashboardRouteId::StaticResource);
    REQUIRE(active.dispatchable());
}

} // namespace

int main()
{
    try {
        classifiesTheCompleteCanonicalInventory();
        exposesExactAllowedMethodSets();
        rejectsOptionsGloballyBeforeRouteDisposition();
        preservesServiceAvailabilityBoundaries();
        classifiesOnlyTheDocumentedStreamQueries();
        preservesUndecodedExactRawPathIdentity();
        remainsNoAllocationAndDeterministicAtTheParserTargetBound();
        std::cout << "Dashboard route catalog tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard route catalog tests failed: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard route catalog tests failed with an unknown error.\n";
        return 1;
    }
}
