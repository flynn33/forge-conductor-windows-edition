#include "ForgeConductor/Dashboard/DashboardApplicationJsonCodec.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::size_t assertions{};

#define REQUIRE(...)                                                             \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(__VA_ARGS__)) {                                                    \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #__VA_ARGS__};                              \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
    REQUIRE(!result.error().message.empty());
}

[[nodiscard]] Domain::UtcTimePoint utc(
    const std::int64_t milliseconds = 1'704'164'645'678LL)
{
    return Domain::UtcTimePoint{std::chrono::milliseconds{milliseconds}};
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::SessionId sessionId()
{
    return take(Domain::SessionId::parse(
        "10000000-0000-4000-8000-000000000001"));
}

[[nodiscard]] Domain::AgentId agentId()
{
    return take(Domain::AgentId::parse("implementer"));
}

[[nodiscard]] Domain::ClientId clientId()
{
    return take(Domain::ClientId::parse("dashboard-client"));
}

[[nodiscard]] Domain::TelemetryHealthReport telemetryReport()
{
    return Domain::TelemetryHealthReport{
        true,
        "forge-telemetry",
        "windows-native-realtime",
        false,
        "native",
        "Windows collectors",
        "winui3",
        false};
}

[[nodiscard]] Domain::RuntimeDiagnosticSnapshot runtimeDiagnostics()
{
    return Domain::RuntimeDiagnosticSnapshot{
        utc(),
        2U,
        1U,
        4U,
        3U,
        1U,
        Domain::ResourcePressureLevel::Warning,
        5U,
        6U,
        7U,
        8U};
}

[[nodiscard]] Dashboard::DashboardTelemetryHealth health()
{
    return Dashboard::DashboardTelemetryHealth{
        telemetryReport(),
        2.0,
        1.75,
        true,
        runtimeDiagnostics(),
        true,
        true,
        false};
}

[[nodiscard]] Domain::ManagerStatus managerStatus()
{
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        500U,
        utc(),
        42s,
        1U,
        std::nullopt,
        true,
        3s,
        false,
        "127.0.0.1",
        7788U,
        8s,
        path("C:\\Forge"),
        "0.9.0"};
}

[[nodiscard]] Domain::AgentSpec agent()
{
    return Domain::AgentSpec{
        agentId(),
        "Implementer",
        "Makes bounded native changes.",
        {"fs.read", "fs.write"},
        {"shell.exec"},
        {"A change is requested"},
        {"Inspect the contract"},
        {"Tests pass"},
        {"summary", "evidence"},
        {"Hand off verification"},
        {"No feature loss"},
        "This body must never be emitted.",
        "builtin"};
}

[[nodiscard]] Domain::AgentSession session(
    const bool includeOptionals = false)
{
    return Domain::AgentSession{
        sessionId(),
        agentId(),
        includeOptionals
            ? std::optional<Domain::ClientId>{clientId()}
            : std::nullopt,
        Domain::SessionStatus::Running,
        includeOptionals
            ? std::optional<std::string>{"Making progress"}
            : std::nullopt,
        utc(),
        utc() + 1s};
}

[[nodiscard]] Domain::AuditEvent auditEvent(
    const bool includeOptionals = false)
{
    return Domain::AuditEvent{
        utc(),
        includeOptionals
            ? std::optional<Domain::ClientId>{clientId()}
            : std::nullopt,
        "forge.status",
        std::nullopt,
        "ok",
        includeOptionals
            ? std::optional<std::chrono::milliseconds>{17ms}
            : std::nullopt,
        includeOptionals
            ? std::optional<std::string>{"warning text"}
            : std::nullopt};
}

[[nodiscard]] Dashboard::DashboardApplicationStatus status()
{
    Dashboard::DashboardStatusData operational{
        {agent()},
        {session(false)},
        {Dashboard::DashboardPresenceRecord{
            "client-composite:one",
            "lm-studio",
            700U,
            path("C:\\Work"),
            utc()}},
        {auditEvent(false)},
        {"forge.status", "agent.run"},
        runtimeDiagnostics()};
    return Dashboard::DashboardApplicationStatus{
        Dashboard::DashboardApplicationIdentity{
            "Forge Conductor",
            "0.9.0",
            "windows-native",
            path("C:\\Forge"),
            path("C:\\Forge\\forge.sqlite3"),
            500U},
        std::move(operational),
        health(),
        managerStatus(),
        "127.0.0.1",
        7788U,
        true};
}

[[nodiscard]] Domain::DoctorReport doctor()
{
    return Domain::DoctorReport{
        true,
        "0.9.0",
        path("C:\\Forge"),
        {Domain::DoctorCheck{"home_layout", true, "C:\\Forge", true},
         Domain::DoctorCheck{"binary", false, "Optional", false}},
        telemetryReport(),
        true,
        path("C:\\Forge\\forge-conductor.exe")};
}

[[nodiscard]] std::set<std::string> keys(const Json& value)
{
    std::set<std::string> result;
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        result.insert(iterator.key());
    }
    return result;
}

void encodesExpandedHealth()
{
    const auto value = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeHealth(health())));
    REQUIRE(keys(value) == std::set<std::string>{
        "auth", "collectors", "export_present", "interferes_with_mcp",
        "mode", "node_available", "node_required", "ok", "runtime",
        "runtime_diagnostics", "sample_hz_measured", "sample_hz_target",
        "service", "static_present", "stream", "stream_running", "ui"});
    REQUIRE(value.at("auth") == true);
    REQUIRE(value.at("sample_hz_target") == 2.0);
    REQUIRE(value.at("sample_hz_measured") == 1.75);
    REQUIRE(value.at("stream") == "realtime");
    REQUIRE(value.at("stream_running") == true);
    REQUIRE(value.at("export_present") == true);
    REQUIRE(value.at("static_present") == true);
    REQUIRE(value.at("node_available") == false);
    const auto& diagnostics = value.at("runtime_diagnostics");
    REQUIRE(diagnostics.at("captured_at") == "2024-01-02T03:04:05Z");
    REQUIRE(diagnostics.at("pressure") == "warning");
    REQUIRE(diagnostics.at("telemetry_pending_snapshots") == 1U);
    REQUIRE(diagnostics.at("open_databases") == 8U);
}

void encodesApplicationStatusAndNullSemantics()
{
    const auto value = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeStatus(status())));
    REQUIRE(keys(value) == std::set<std::string>{
        "agent_count", "agents", "dashboard", "home", "manager",
        "ok", "open_session_count", "open_sessions", "pid", "presence",
        "presence_count", "product", "recent_audit", "runtime",
        "runtime_diagnostics", "service_active", "store", "telemetry",
        "tools", "version"});
    REQUIRE(value.at("ok") == true);
    REQUIRE(value.at("agents") == Json::array({"implementer"}));
    REQUIRE(value.at("agent_count") == 1U);
    REQUIRE(value.at("dashboard").at("host") == "127.0.0.1");
    REQUIRE(value.at("dashboard").at("port") == 7788U);
    REQUIRE(value.at("service_active") == true);

    const auto& open = value.at("open_sessions").at(0);
    REQUIRE(open.contains("client_id"));
    REQUIRE(open.at("client_id").is_null());
    REQUIRE(open.contains("summary"));
    REQUIRE(open.at("summary").is_null());
    const auto& recent = value.at("recent_audit").at(0);
    REQUIRE(recent.at("client_id").is_null());
    REQUIRE(recent.at("duration_ms").is_null());
    REQUIRE(recent.at("error").is_null());
    REQUIRE(recent.at("args_json").is_null());

    const auto& presence = value.at("presence").at(0);
    REQUIRE(presence.at("client_id") == "client-composite:one");
    REQUIRE(presence.at("cwd") == "C:\\Work");
    REQUIRE(presence.at("last_heartbeat") == "2024-01-02T03:04:05Z");

    const auto& telemetry = value.at("telemetry");
    REQUIRE(telemetry.at("export_present") == true);
    REQUIRE(telemetry.at("static_present") == true);
    REQUIRE(telemetry.at("node_available") == false);
    REQUIRE(!telemetry.contains("sample_hz_target"));

    const auto& manager = value.at("manager");
    REQUIRE(keys(manager) == std::set<std::string>{
        "auto_restart", "dashboard", "desired_running", "home",
        "http_listening", "last_error", "manager", "ok",
        "open_browser_on_start", "pid", "restart_count", "service_active",
        "started_at", "state", "uptime_sec", "version",
        "watchdog_interval_sec"});
    REQUIRE(manager.at("dashboard").at("url") == "http://127.0.0.1:7788/");
    REQUIRE(manager.at("started_at") == "2024-01-02T03:04:05Z");
    REQUIRE(manager.at("last_error").is_null());
}

void encodesDoctorAndAgentsWithoutBodies()
{
    const auto doctorValue = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeDoctor(doctor())));
    REQUIRE(keys(doctorValue) == std::set<std::string>{
        "binary", "checks", "home", "ok", "telemetry", "version"});
    REQUIRE(keys(doctorValue.at("checks").at(0)) ==
            std::set<std::string>{"detail", "name", "ok"});
    REQUIRE(!doctorValue.at("checks").at(0).contains("hard"));
    REQUIRE(doctorValue.at("binary").at("installed") == true);

    auto source = agent();
    source.body = std::string{"body\0is not an encoded field", 28U};
    const auto agentsValue = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeAgents({source})));
    const auto& encoded = agentsValue.at("agents").at(0);
    REQUIRE(keys(encoded) == std::set<std::string>{
        "description", "display_name", "id", "playbook", "source", "tools"});
    REQUIRE(!encoded.contains("body"));
    REQUIRE(encoded.at("playbook").at("tools_primary") == encoded.at("tools"));
    REQUIRE(encoded.at("playbook").at("tools_forbidden").at(0) == "shell.exec");
}

void distinguishesEndpointOmissionFromStatusNulls()
{
    Dashboard::DashboardSessionListing listing{{session(false)}, {session(true)}};
    const auto sessionsValue = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeSessions(listing)));
    const auto& absent = sessionsValue.at("open").at(0);
    REQUIRE(!absent.contains("client_id"));
    REQUIRE(!absent.contains("summary"));
    const auto& present = sessionsValue.at("recent").at(0);
    REQUIRE(present.at("client_id") == "dashboard-client");
    REQUIRE(present.at("summary") == "Making progress");
    REQUIRE(present.at("created_at") == "2024-01-02T03:04:05Z");

    const auto auditValue = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeAudit(
            {auditEvent(false), auditEvent(true)})));
    const auto& absentAudit = auditValue.at("events").at(0);
    REQUIRE(keys(absentAudit) ==
            std::set<std::string>{"status", "timestamp", "tool"});
    const auto& presentAudit = auditValue.at("events").at(1);
    REQUIRE(presentAudit.at("client_id") == "dashboard-client");
    REQUIRE(presentAudit.at("duration_ms") == 17);
    REQUIRE(presentAudit.at("error") == "warning text");
    REQUIRE(!presentAudit.contains("args_json"));
}

void encodesDiagnosticsAndAcknowledgementsExactly()
{
    const auto diagnostics = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(
            {"first", "quoted \"line\"", "snowman \xE2\x98\x83"})));
    REQUIRE(diagnostics.at("lines").size() == 3U);
    REQUIRE(diagnostics.at("lines").at(1) == "quoted \"line\"");
    REQUIRE(diagnostics.at("lines").at(2) == "snowman \xE2\x98\x83");

    REQUIRE(take(Dashboard::DashboardApplicationJsonCodec::
                     encodePruneAcknowledgement()) ==
            "{\"ok\":true,\"message\":\"Pruned stale sessions\"}");
    REQUIRE(take(Dashboard::DashboardApplicationJsonCodec::
                     encodeRestartAcknowledgement()) ==
            "{\"ok\":true,\"message\":\"Manager restart accepted\",\"state\":\"restarting\"}");
    REQUIRE(take(Dashboard::DashboardApplicationJsonCodec::
                     encodeShutdownAcknowledgement()) ==
            "{\"ok\":true,\"message\":\"Manager shutting down\",\"state\":\"stopping\"}");
    const auto closed = Json::parse(take(
        Dashboard::DashboardApplicationJsonCodec::encodeClosedSession(
            session(false))));
    REQUIRE(closed.at("ok") == true);
    REQUIRE(!closed.at("session").contains("client_id"));
    REQUIRE(!closed.at("session").contains("summary"));
}

void enforcesExplicitCollectionCaps()
{
    Dashboard::DashboardSessionListing listing;
    listing.recent.assign(
        Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
        session(false));
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodeSessions(listing));
    listing.recent.push_back(session(false));
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeSessions(listing),
        Domain::ErrorCodes::LimitExceeded);

    std::vector<Domain::AuditEvent> events(
        Dashboard::DashboardApplicationLimits::MaximumAuditEvents,
        auditEvent(false));
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodeAudit(events));
    events.push_back(auditEvent(false));
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeAudit(events),
        Domain::ErrorCodes::LimitExceeded);

    std::vector<std::string> lines(
        Dashboard::DashboardApplicationLimits::MaximumDiagnosticLines,
        "line");
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(lines));
    lines.push_back("overflow");
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(lines),
        Domain::ErrorCodes::LimitExceeded);

    auto statusValue = status();
    statusValue.operational.recentAudit.assign(
        Dashboard::DashboardApplicationLimits::MaximumStatusAuditEvents,
        auditEvent(false));
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodeStatus(statusValue));
    statusValue.operational.recentAudit.push_back(auditEvent(false));
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeStatus(statusValue),
        Domain::ErrorCodes::LimitExceeded);

    auto doctorValue = doctor();
    doctorValue.checks.assign(
        Dashboard::DashboardApplicationLimits::MaximumDoctorChecks,
        Domain::DoctorCheck{"check", true, "detail", true});
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodeDoctor(doctorValue));
    doctorValue.checks.push_back(
        Domain::DoctorCheck{"overflow", true, "detail", true});
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDoctor(doctorValue),
        Domain::ErrorCodes::LimitExceeded);
}

void enforcesTextNumericTimestampAndByteContracts()
{
    auto healthValue = health();
    healthValue.measuredSampleHz = std::numeric_limits<double>::quiet_NaN();
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeHealth(healthValue),
        Domain::ErrorCodes::InvalidRequest);
    healthValue = health();
    healthValue.targetSampleHz = -0.5;
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeHealth(healthValue),
        Domain::ErrorCodes::InvalidRequest);

    std::string invalidUtf8{"\xC3\x28", 2U};
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics({invalidUtf8}),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(
            {std::string{"embedded\0nul", 12U}}),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidSession = session(false);
    invalidSession.status = static_cast<Domain::SessionStatus>(255);
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeClosedSession(
            invalidSession),
        Domain::ErrorCodes::InvalidRequest);
    invalidSession = session(false);
    invalidSession.createdAt = utc(-1);
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeClosedSession(
            invalidSession),
        Domain::ErrorCodes::InvalidRequest);
    invalidSession = session(false);
    invalidSession.updatedAt = utc(253'402'300'800'000LL);
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeClosedSession(
            invalidSession),
        Domain::ErrorCodes::InvalidRequest);

    invalidSession = session(false);
    invalidSession.status = Domain::SessionStatus::Closed;
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeSessions(
            Dashboard::DashboardSessionListing{{invalidSession}, {}}),
        Domain::ErrorCodes::InvalidRequest);
    auto invalidStatus = status();
    invalidStatus.operational.openSessions.front().status =
        Domain::SessionStatus::Completed;
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeStatus(invalidStatus),
        Domain::ErrorCodes::InvalidRequest);
    invalidStatus = status();
    invalidStatus.identity.processId = 0U;
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeStatus(invalidStatus),
        Domain::ErrorCodes::InvalidRequest);
    invalidStatus = status();
    invalidStatus.operational.presence.front().processId = 0U;
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeStatus(invalidStatus),
        Domain::ErrorCodes::InvalidRequest);

    auto event = auditEvent(true);
    event.duration = -1ms;
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeAudit({event}),
        Domain::ErrorCodes::InvalidRequest);

    event = auditEvent(false);
    event.tool.assign(
        Dashboard::DashboardApplicationLimits::MaximumAuditTextBytes,
        'x');
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeAudit({event}),
        Domain::ErrorCodes::LimitExceeded);

    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics({}, 0U),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(
            {}, Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes + 1U),
        Domain::ErrorCodes::InvalidRequest);

    const auto acknowledgement = take(
        Dashboard::DashboardApplicationJsonCodec::encodePruneAcknowledgement());
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodePruneAcknowledgement(
        acknowledgement.size()));
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodePruneAcknowledgement(
            acknowledgement.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    const auto encodedDiagnostics = take(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics({"one"}));
    REQUIRE(Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(
        {"one"}, encodedDiagnostics.size()));
    requireError(
        Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(
            {"one"}, encodedDiagnostics.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
}

} // namespace

int main()
{
    try {
        encodesExpandedHealth();
        encodesApplicationStatusAndNullSemantics();
        encodesDoctorAndAgentsWithoutBodies();
        distinguishesEndpointOmissionFromStatusNulls();
        encodesDiagnosticsAndAcknowledgementsExactly();
        enforcesExplicitCollectionCaps();
        enforcesTextNumericTimestampAndByteContracts();
        std::cout << "Dashboard application JSON codec tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard application JSON codec tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard application JSON codec tests failed with an unknown error.\n";
        return 1;
    }
}
