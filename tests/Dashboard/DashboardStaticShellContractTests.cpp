#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.h"

#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

std::size_t assertionCount{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        fail(message);
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

[[nodiscard]] std::string assetText(
    const Dashboard::DashboardStaticAssetStore& store,
    const std::string_view rawPath)
{
    auto decoded = Dashboard::DashboardStaticResourcePath::decode(rawPath);
    if (!decoded) {
        fail(decoded.error().message);
    }
    const auto asset = take(store.findStaticAsset(decoded.value()));
    std::string text;
    text.reserve(asset->bytes().size());
    for (const auto byte : asset->bytes()) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

[[nodiscard]] std::filesystem::path repositoryRoot()
{
    auto cursor = std::filesystem::absolute(std::filesystem::path{__FILE__})
                      .parent_path();
    for (std::size_t depth{}; depth < 12U && !cursor.empty(); ++depth) {
        if (std::filesystem::exists(
                cursor / "src/Hosts/Manager/Resources/Dashboard/auth.ts")) {
            return cursor;
        }
        cursor = cursor.parent_path();
    }

    cursor = std::filesystem::current_path();
    for (std::size_t depth{}; depth < 12U && !cursor.empty(); ++depth) {
        if (std::filesystem::exists(
                cursor / "src/Hosts/Manager/Resources/Dashboard/auth.ts")) {
            return cursor;
        }
        cursor = cursor.parent_path();
    }
    fail("dashboard source root was not found");
}

[[nodiscard]] std::string sourceText(const std::string_view fileName)
{
    const auto path = repositoryRoot() /
        "src/Hosts/Manager/Resources/Dashboard" / fileName;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        fail("dashboard source pair could not be opened");
    }
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string_view section(
    const std::string_view contents,
    const std::string_view begin,
    const std::string_view end)
{
    const auto first = contents.find(begin);
    if (first == std::string_view::npos) {
        fail(std::string{"source contract section omitted "} +
             std::string{begin});
    }
    const auto last = contents.find(end, first + begin.size());
    if (last == std::string_view::npos) {
        fail(std::string{"source contract section omitted terminator "} +
             std::string{end});
    }
    return contents.substr(first, last - first);
}

[[nodiscard]] std::size_t occurrences(
    const std::string_view contents,
    const std::string_view value)
{
    std::size_t count{};
    std::size_t offset{};
    while ((offset = contents.find(value, offset)) != std::string_view::npos) {
        ++count;
        offset += value.size();
    }
    return count;
}

template <std::size_t Count>
void requireOrdered(
    const std::string_view contents,
    const std::array<std::string_view, Count>& ordered,
    const std::string_view contract)
{
    std::size_t offset{};
    for (const auto value : ordered) {
        const auto position = contents.find(value, offset);
        require(
            position != std::string_view::npos,
            std::string{contract} + " omitted or reordered " +
                std::string{value});
        offset = position + value.size();
    }
}

template <std::size_t Count>
void requireAll(
    const std::string_view contents,
    const std::array<std::string_view, Count>& required,
    const std::string_view contract)
{
    for (const auto value : required) {
        require(
            contents.find(value) != std::string_view::npos,
            std::string{contract} + " omitted " + std::string{value});
    }
}

template <std::size_t Count>
void rejectAll(
    const std::string_view contents,
    const std::array<std::string_view, Count>& rejected,
    const std::string_view contract)
{
    for (const auto value : rejected) {
        require(
            contents.find(value) == std::string_view::npos,
            std::string{contract} + " contained forbidden text " +
                std::string{value});
    }
}

void semanticShellsExposeEveryRequiredSurface()
{
    const auto store = take(Windows::WindowsDashboardStaticAssetBundle::create());
    const auto index = assetText(*store, "/static/index.html");
    const auto control = assetText(*store, "/static/control.html");
    const auto css = assetText(*store, "/static/dashboard.css");

    constexpr std::array<std::string_view, 17U> IndexContract{{
        "<!doctype html>",
        "<html lang=\"en\">",
        "href=\"#main-content\"",
        "aria-live=\"polite\"",
        "role=\"alert\"",
        "panel-sys-strip",
        "panel-load-trace",
        "panel-cpu-cores",
        "panel-gpu-cores",
        "panel-storage",
        "panel-orchestration",
        "panel-mcp-servers",
        "panel-mcp-tools",
        "panel-sub-agents",
        "panel-hot-processes",
        "panel-live-stream",
        "/static/telemetry.js",
    }};
    requireAll(index, IndexContract, "telemetry shell");

    constexpr std::array<std::string_view, 24U> ControlContract{{
        "<!doctype html>",
        "<html lang=\"en\">",
        "href=\"#main-content\"",
        "manager-start",
        "manager-stop",
        "manager-restart",
        "manager-refresh",
        "manager-shutdown",
        "manager-settings-form",
        "settings-dashboard-host",
        "settings-dashboard-port",
        "settings-refresh-interval",
        "settings-watchdog-interval",
        "settings-session-idle-ttl",
        "settings-shell-timeout",
        "settings-auto-restart",
        "settings-open-browser",
        "settings-log-level",
        "open-session-rows",
        "recent-session-rows",
        "agent-rows",
        "audit-rows",
        "diagnostic-lines",
        "doctor-run",
    }};
    requireAll(control, ControlContract, "control shell");

    constexpr std::array<std::string_view, 7U> CssContract{{
        "font-family: \"Segoe UI\"",
        ":focus-visible",
        "@media (max-width: 52rem)",
        "@media (forced-colors: active)",
        "@media (prefers-reduced-motion: reduce)",
        ".table-scroll",
        "[hidden]",
    }};
    requireAll(css, CssContract, "dashboard CSS");

    constexpr std::array<std::string_view, 4U> ForbiddenMarkup{{
        " onclick=",
        " onsubmit=",
        "<img",
        "<svg",
    }};
    rejectAll(index, ForbiddenMarkup, "telemetry shell");
    rejectAll(control, ForbiddenMarkup, "control shell");
}

void authenticationAndStreamingRemainClosedAndBounded()
{
    const auto store = take(Windows::WindowsDashboardStaticAssetBundle::create());
    const auto auth = assetText(*store, "/static/auth.js");
    const auto telemetry = assetText(*store, "/static/telemetry.js");

    constexpr std::array<std::string_view, 11U> AuthContract{{
        "window.location.hash",
        "URLSearchParams",
        "parameters.get(\"token\")",
        "window.sessionStorage.setItem",
        "window.sessionStorage.getItem",
        "window.history.replaceState",
        "headers.set(\"Authorization\"",
        "`Bearer ${this.token}`",
        "credentials: \"omit\"",
        "redirect: \"error\"",
        "destination.hash = `token=${encodeURIComponent(this.token)}`",
    }};
    requireAll(auth, AuthContract, "authentication module");

    constexpr std::array<std::string_view, 13U> StreamingContract{{
        "/api/live",
        "/api/stream?hz=2",
        "MAXIMUM_EVENT_BUFFER_BYTES = 2_097_152",
        "MAXIMUM_HISTORY_ROWS = 20",
        "MAXIMUM_PROCESS_ROWS = 12",
        "MAXIMUM_RECONNECT_DELAY_MS = 30_000",
        "pendingFrame = frame",
        "requestAnimationFrame",
        "response.body.getReader()",
        "TextDecoder(\"utf-8\", { fatal: true })",
        "visibilitychange",
        "streamController?.abort()",
        "pagehide",
    }};
    requireAll(telemetry, StreamingContract, "telemetry module");

    constexpr std::array<std::string_view, 7U> ForbiddenScript{{
        "EventSource",
        "localStorage",
        "?token=",
        "innerHTML",
        "insertAdjacentHTML",
        "document.write",
        "console.",
    }};
    rejectAll(auth, ForbiddenScript, "authentication module");
    rejectAll(telemetry, ForbiddenScript, "telemetry module");
}

void requestOwnershipAndWatchdogAreExplicitStaticContracts()
{
    // These checks prove checked-in and packaged source contracts. Live browser
    // cancellation, timing, and rendering remain separate runtime evidence.
    const auto store = take(Windows::WindowsDashboardStaticAssetBundle::create());
    const auto auth = assetText(*store, "/static/auth.js");
    const auto telemetry = assetText(*store, "/static/telemetry.js");

    requireAll(
        auth,
        std::array<std::string_view, 4U>{
            "MAXIMUM_JSON_RESPONSE_BYTES = 2_080_768",
            "new Uint8Array(MAXIMUM_JSON_RESPONSE_BYTES)",
            "async withResponse(path, init, timeoutMs, consume)",
            "return this.withResponse(path, { method: \"GET\", signal }, null, consume)",
        },
        "packaged request ownership");

    const auto json = section(auth, "async json(path, init = {})", "async post(path, body)");
    requireOrdered(
        json,
        std::array<std::string_view, 3U>{
            "return this.withResponse",
            "await readBoundedJson(response)",
            "return payload",
        },
        "packaged JSON body lifetime");

    const auto owner = section(
        auth,
        "async withResponse(path, init, timeoutMs, consume)",
        "export function requireDashboardClient");
    requireOrdered(
        owner,
        std::array<std::string_view, 5U>{
            "payload = await readBoundedJson(response)",
            "return await consume(response)",
            "controller.abort();",
            "window.clearTimeout(timeout)",
            "externalSignal?.removeEventListener",
        },
        "packaged response owner cleanup");

    const auto connect = section(
        telemetry,
        "async function connectStream()",
        "element(\"dashboard-refresh\")");
    requireOrdered(
        connect,
        std::array<std::string_view, 4U>{
            "await client.stream(\"/api/stream?hz=2\", controller.signal, async (response)",
            "startSilenceWatchdog(controller)",
            "await consumeStream(response, controller.signal)",
            "clearSilenceWatchdog()",
        },
        "packaged stream consumer lifetime");

    requireAll(
        telemetry,
        std::array<std::string_view, 10U>{
            "SILENCE_REFRESH_AFTER_MS = 2_500",
            "SILENCE_RECONNECT_AFTER_MS = 5_000",
            "SILENCE_WATCHDOG_INTERVAL_MS = 500",
            "lastStreamFrameAtMs = window.performance.now()",
            "void refreshOnce()",
            "controller.abort()",
            "void refreshInFlight.finally(() => scheduleReconnect())",
            "refreshController?.abort()",
            "visibilitychange",
            "pagehide",
        },
        "packaged silence watchdog");

    const auto watchdog = section(
        telemetry,
        "function startSilenceWatchdog(controller)",
        "async function consumeStream");
    requireOrdered(
        watchdog,
        std::array<std::string_view, 4U>{
            "silenceMs > SILENCE_RECONNECT_AFTER_MS",
            "controller.abort()",
            "silenceMs >= SILENCE_REFRESH_AFTER_MS",
            "void refreshOnce()",
        },
        "packaged silence thresholds");
}

void controlAndCollectionSemanticsAreExplicitStaticContracts()
{
    const auto store = take(Windows::WindowsDashboardStaticAssetBundle::create());
    const auto control = assetText(*store, "/static/control.js");
    const auto controlHtml = assetText(*store, "/static/control.html");
    const auto telemetry = assetText(*store, "/static/telemetry.js");

    require(
        occurrences(control, "let settingsInFlight = null") == 1U,
        "packaged control source did not declare exactly one settings owner");
    require(
        occurrences(control, "client.json(\"/api/manager/settings\")") == 1U,
        "packaged control source did not own exactly one settings GET site");
    const auto settings = section(
        control,
        "async function loadSettings()",
        "async function refreshAll()");
    requireAll(
        settings,
        std::array<std::string_view, 5U>{
            "return settingsInFlight",
            "settingsInFlight = operation",
            "settingsInFlight = null",
            "refreshInFlight !== null",
            "mutationInFlight",
        },
        "packaged settings single-flight owner");
    require(
        occurrences(
            control,
            "mutationInFlight || refreshInFlight !== null || settingsInFlight !== null") == 6U,
        "packaged mutations did not all cross-guard refresh and settings owners");
    requireAll(
        control,
        std::array<std::string_view, 8U>{
            "client.json(\"/api/status\")",
            "renderApplicationStatus(applicationStatus)",
            "application-service-state",
            "application-open-sessions",
            "application-agent-count",
            "application-presence-count",
            "application-runtime",
            "application-runtime-pressure",
        },
        "packaged composite application status");
    requireAll(
        controlHtml,
        std::array<std::string_view, 6U>{
            "application-service-state",
            "application-open-sessions",
            "application-agent-count",
            "application-presence-count",
            "application-runtime",
            "application-runtime-pressure",
        },
        "packaged composite KPI markup");

    requireAll(
        telemetry,
        std::array<std::string_view, 5U>{
            "raw === null || raw.length !== 0",
            "No MCP servers reported.",
            "No MCP tools reported.",
            "No agent sessions reported.",
            "No live feed entries reported.",
        },
        "packaged unavailable-versus-empty collections");
    require(
        occurrences(telemetry, "raw === null || raw.length !== 0") == 4U,
        "packaged collection renderers did not each retain a null-versus-empty branch");
}

void sourcePairsCarryMatchingExplicitContractMarkers()
{
    const auto store = take(Windows::WindowsDashboardStaticAssetBundle::create());
    struct Pair final {
        std::string_view file;
        std::string_view asset;
        std::string_view marker;
    };
    constexpr std::array<Pair, 3U> Pairs{{
        {"auth.ts", "/static/auth.js", "forge-dashboard-auth-v2"},
        {"telemetry.ts", "/static/telemetry.js", "forge-dashboard-telemetry-v2"},
        {"control.ts", "/static/control.js", "forge-dashboard-control-v2"},
    }};
    for (const auto& pair : Pairs) {
        const auto source = sourceText(pair.file);
        const auto packaged = assetText(*store, pair.asset);
        require(
            occurrences(source, pair.marker) == 1U,
            "TypeScript source did not contain exactly one source-pair marker");
        require(
            occurrences(packaged, pair.marker) == 1U,
            "packaged JavaScript did not contain exactly one matching source-pair marker");
    }
}

void controlModuleUsesOnlyDocumentedApplicationRoutes()
{
    const auto store = take(Windows::WindowsDashboardStaticAssetBundle::create());
    const auto control = assetText(*store, "/static/control.js");

    constexpr std::array<std::string_view, 14U> RouteContract{{
        "/api/manager/status",
        "/api/manager/settings",
        "/api/manager/start",
        "/api/manager/stop",
        "/api/manager/restart",
        "/api/manager/shutdown",
        "/api/sessions",
        "/api/agents",
        "/api/audit",
        "/api/diagnostics",
        "/api/sessions/prune",
        "/api/sessions/close",
        "/api/doctor",
        "bind_changed",
    }};
    requireAll(control, RouteContract, "control module");

    constexpr std::array<std::string_view, 12U> SettingsContract{{
        "refresh_interval_sec",
        "watchdog_interval_sec",
        "idle_ttl_sec",
        "default_timeout_sec",
        "auto_restart",
        "open_browser_on_start",
        "log_level",
        "service_active",
        "renderServiceStopped",
        "navigateToReboundDashboard",
        "manager-shutdown-dialog",
        "Closed from dashboard",
    }};
    requireAll(control, SettingsContract, "control module");

    constexpr std::array<std::string_view, 7U> ForbiddenScript{{
        "EventSource",
        "localStorage",
        "?token=",
        "innerHTML",
        "insertAdjacentHTML",
        "document.write",
        "console.",
    }};
    rejectAll(control, ForbiddenScript, "control module");
}

} // namespace

int main()
{
    try {
        semanticShellsExposeEveryRequiredSurface();
        authenticationAndStreamingRemainClosedAndBounded();
        requestOwnershipAndWatchdogAreExplicitStaticContracts();
        controlAndCollectionSemanticsAreExplicitStaticContracts();
        sourcePairsCarryMatchingExplicitContractMarkers();
        controlModuleUsesOnlyDocumentedApplicationRoutes();
        std::cout << "Dashboard static shell contract tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard static shell contract tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
