#include "ForgeConductor/Dashboard/DashboardApplicationJsonCodec.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::int64_t FirstUnsupportedUtcMillisecond =
    253'402'300'800'000LL;
constexpr std::size_t MaximumGeneralTextBytes = 64U * 1024U;
constexpr std::size_t MaximumAgentItems = 256U;
constexpr std::size_t MaximumAgentItemBytes = 4U * 1024U;

class ApplicationCodecException final : public std::runtime_error {
public:
    ApplicationCodecException(std::string code, std::string message)
        : std::runtime_error{std::move(message)}, code_{std::move(code)}
    {
    }

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

[[noreturn]] void reject(
    const std::string_view code,
    const std::string_view message)
{
    throw ApplicationCodecException{std::string{code}, std::string{message}};
}

void validateMaximumBytes(const std::size_t maximumBytes)
{
    if (maximumBytes == 0U ||
        maximumBytes > DashboardApplicationJsonCodec::MaximumResponseBytes) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application maximum response bytes must be between 1 and 2080768.");
    }
}

class BoundedJsonWriter final {
public:
    explicit BoundedJsonWriter(const std::size_t maximumBytes)
        : maximumBytes_{maximumBytes}
    {
        output_.reserve((std::min)(maximumBytes, std::size_t{16U * 1024U}));
    }

    void raw(const std::string_view value)
    {
        if (output_.size() > maximumBytes_ ||
            value.size() > maximumBytes_ - output_.size()) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard application JSON exceeds the configured response-byte limit.");
        }
        output_.append(value);
    }

    void character(const char value)
    {
        if (output_.size() >= maximumBytes_) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard application JSON exceeds the configured response-byte limit.");
        }
        output_.push_back(value);
    }

    void boolean(const bool value) { raw(value ? "true" : "false"); }
    void nullValue() { raw("null"); }

    template <typename Integer>
    void integer(const Integer value)
    {
        std::array<char, 32U> buffer{};
        const auto result = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), value);
        if (result.ec != std::errc{}) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard application integer formatting failed.");
        }
        raw(std::string_view{
            buffer.data(),
            static_cast<std::size_t>(result.ptr - buffer.data())});
    }

    void number(const double value)
    {
        if (!std::isfinite(value)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard application numeric values must be finite.");
        }
        if (value == 0.0) {
            raw("0");
            return;
        }
        std::array<char, 64U> buffer{};
        const auto result = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            value,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{}) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard application number formatting failed.");
        }
        raw(std::string_view{
            buffer.data(),
            static_cast<std::size_t>(result.ptr - buffer.data())});
    }

    void string(const std::string_view value)
    {
        if (value.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(value)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard application text contains an embedded NUL or invalid UTF-8.");
        }
        constexpr char Hex[] = "0123456789abcdef";
        character('"');
        for (const unsigned char byte : value) {
            switch (byte) {
            case '"': raw("\\\""); break;
            case '\\': raw("\\\\"); break;
            case '\b': raw("\\b"); break;
            case '\f': raw("\\f"); break;
            case '\n': raw("\\n"); break;
            case '\r': raw("\\r"); break;
            case '\t': raw("\\t"); break;
            default:
                if (byte < 0x20U) {
                    raw("\\u00");
                    character(Hex[(byte >> 4U) & 0x0fU]);
                    character(Hex[byte & 0x0fU]);
                } else {
                    character(static_cast<char>(byte));
                }
                break;
            }
        }
        character('"');
    }

    void member(bool& first, const std::string_view name)
    {
        if (!first) character(',');
        first = false;
        string(name);
        character(':');
    }

    [[nodiscard]] std::string finish() && { return std::move(output_); }

private:
    const std::size_t maximumBytes_;
    std::string output_;
};

class TextBudget final {
public:
    explicit TextBudget(const std::size_t maximumBytes)
        : maximumBytes_{maximumBytes}
    {
    }

    void add(const std::string_view value)
    {
        if (bytes_ > maximumBytes_ || value.size() > maximumBytes_ - bytes_) {
            reject(
                Domain::ErrorCodes::LimitExceeded,
                "Dashboard application source text exceeds its aggregate limit.");
        }
        requireText(value);
        bytes_ += value.size();
    }

private:
    static void requireText(const std::string_view value)
    {
        if (value.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(value)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard application text contains an embedded NUL or invalid UTF-8.");
        }
    }

    const std::size_t maximumBytes_;
    std::size_t bytes_{};
};

void requireText(
    const std::string_view value,
    const std::size_t maximumBytes,
    const std::string_view field,
    const bool allowEmpty = true)
{
    if ((!allowEmpty && value.empty()) || value.size() > maximumBytes) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            std::string{"Dashboard application field "} + std::string{field} +
                " violates its byte limit.");
    }
    if (value.find('\0') != std::string_view::npos ||
        !Domain::isValidUtf8(value)) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{"Dashboard application field "} + std::string{field} +
                " contains an embedded NUL or invalid UTF-8.");
    }
}

void requireCollection(
    const std::size_t actual,
    const std::size_t maximum,
    const std::string_view field)
{
    if (actual > maximum) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            std::string{"Dashboard application collection "} +
                std::string{field} + " exceeds its hard limit.");
    }
}

[[nodiscard]] std::int64_t utcMilliseconds(
    const Domain::UtcTimePoint timestamp)
{
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(
        timestamp.time_since_epoch());
    const auto value = milliseconds.count();
    if (value < 0 || value >= FirstUnsupportedUtcMillisecond) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application timestamps must be UTC instants from 1970 through 9999.");
    }
    return value;
}

[[nodiscard]] std::string iso8601Utc(const Domain::UtcTimePoint timestamp)
{
    using namespace std::chrono;
    const milliseconds sinceEpoch{utcMilliseconds(timestamp)};
    const auto day = floor<days>(sinceEpoch);
    const year_month_day calendar{sys_days{day}};
    const hh_mm_ss time{sinceEpoch - day};
    if (!calendar.ok()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application timestamp is not a valid UTC calendar instant.");
    }
    std::array<char, 25U> buffer{};
    const auto written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<int>(calendar.year()),
        static_cast<unsigned>(calendar.month()),
        static_cast<unsigned>(calendar.day()),
        static_cast<long long>(time.hours().count()),
        static_cast<long long>(time.minutes().count()),
        static_cast<long long>(time.seconds().count()),
        static_cast<long long>(time.subseconds().count()));
    if (written != 24) {
        reject(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard application timestamp formatting failed.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] std::string iso8601UtcSeconds(
    const Domain::UtcTimePoint timestamp)
{
    const auto full = iso8601Utc(timestamp);
    return full.substr(0U, 19U) + 'Z';
}

[[nodiscard]] std::string_view sessionStatusName(
    const Domain::SessionStatus status)
{
    switch (status) {
    case Domain::SessionStatus::Open: return "open";
    case Domain::SessionStatus::Active: return "active";
    case Domain::SessionStatus::Running: return "running";
    case Domain::SessionStatus::Started: return "started";
    case Domain::SessionStatus::Closed: return "closed";
    case Domain::SessionStatus::Completed: return "completed";
    case Domain::SessionStatus::Failed: return "failed";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard application session contains an unknown status.");
}

[[nodiscard]] std::string_view pressureName(
    const Domain::ResourcePressureLevel pressure)
{
    switch (pressure) {
    case Domain::ResourcePressureLevel::Nominal: return "nominal";
    case Domain::ResourcePressureLevel::Warning: return "warning";
    case Domain::ResourcePressureLevel::Critical: return "critical";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard application diagnostics contain an unknown pressure state.");
}

[[nodiscard]] std::string_view managerStateName(
    const Domain::ManagerServiceState state)
{
    switch (state) {
    case Domain::ManagerServiceState::Stopped: return "stopped";
    case Domain::ManagerServiceState::Starting: return "starting";
    case Domain::ManagerServiceState::Running: return "running";
    case Domain::ManagerServiceState::Restarting: return "restarting";
    case Domain::ManagerServiceState::Stopping: return "stopping";
    case Domain::ManagerServiceState::Failed: return "failed";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard application manager contains an unknown service state.");
}

[[nodiscard]] std::string dashboardUrl(
    const std::string_view host,
    const std::uint16_t port)
{
    if (host == "127.0.0.1") {
        return "http://127.0.0.1:" + std::to_string(port) + '/';
    }
    if (host == "::1") {
        return "http://[::1]:" + std::to_string(port) + '/';
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard application manager requires a canonical loopback host.");
}

void writeRuntimeDiagnostics(
    BoundedJsonWriter& writer,
    const Domain::RuntimeDiagnosticSnapshot& value)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "captured_at");
    writer.string(iso8601UtcSeconds(value.timestamp));
    writer.member(first, "owned_operations");
    writer.integer(value.ownedOperations);
    writer.member(first, "pending_callbacks");
    writer.integer(value.pendingCallbacks);
    writer.member(first, "background_threads");
    writer.integer(value.backgroundThreads);
    writer.member(first, "open_repositories");
    writer.integer(value.openRepositories);
    writer.member(first, "telemetry_pending_snapshots");
    writer.integer(value.telemetryPendingSnapshots);
    writer.member(first, "pressure");
    writer.string(pressureName(value.pressure));
    writer.member(first, "active_timers");
    writer.integer(value.activeTimers);
    writer.member(first, "child_processes");
    writer.integer(value.childProcesses);
    writer.member(first, "process_readers");
    writer.integer(value.processReaders);
    writer.member(first, "open_databases");
    writer.integer(value.openDatabases);
    writer.character('}');
}

void validateTelemetryReport(
    const Domain::TelemetryHealthReport& report,
    TextBudget& budget)
{
    for (const auto* value : {
             &report.service,
             &report.runtime,
             &report.mode,
             &report.collectors,
             &report.ui}) {
        requireText(*value, MaximumGeneralTextBytes, "telemetry text");
        budget.add(*value);
    }
}

void writeTelemetryReport(
    BoundedJsonWriter& writer,
    const Domain::TelemetryHealthReport& report,
    const bool concreteEnvironment,
    const DashboardTelemetryHealth* expanded,
    const bool includeExpanded)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "ok");
    writer.boolean(report.ok);
    writer.member(first, "service");
    writer.string(report.service);
    writer.member(first, "runtime");
    writer.string(report.runtime);
    writer.member(first, "interferes_with_mcp");
    writer.boolean(report.interferesWithMcp);
    writer.member(first, "mode");
    writer.string(report.mode);
    writer.member(first, "auth");
    writer.boolean(true);
    writer.member(first, "collectors");
    writer.string(report.collectors);
    writer.member(first, "ui");
    writer.string(report.ui);
    writer.member(first, "export_present");
    if (concreteEnvironment) writer.boolean(expanded->exportPresent);
    else writer.nullValue();
    writer.member(first, "static_present");
    if (concreteEnvironment) writer.boolean(expanded->staticPresent);
    else writer.nullValue();
    writer.member(first, "node_available");
    if (concreteEnvironment) writer.boolean(expanded->nodeAvailable);
    else writer.nullValue();
    writer.member(first, "node_required");
    writer.boolean(report.nodeRequired);
    if (expanded != nullptr && includeExpanded) {
        writer.member(first, "sample_hz_target");
        writer.number(expanded->targetSampleHz);
        writer.member(first, "sample_hz_measured");
        writer.number(expanded->measuredSampleHz);
        writer.member(first, "stream");
        writer.string("realtime");
        writer.member(first, "stream_running");
        writer.boolean(expanded->streamRunning);
        writer.member(first, "runtime_diagnostics");
        writeRuntimeDiagnostics(writer, expanded->runtimeDiagnostics);
    }
    writer.character('}');
}

void validateHealth(const DashboardTelemetryHealth& health)
{
    TextBudget budget{MaximumGeneralTextBytes};
    validateTelemetryReport(health.report, budget);
    if (!std::isfinite(health.targetSampleHz) ||
        !std::isfinite(health.measuredSampleHz) ||
        health.targetSampleHz < 0.0 || health.measuredSampleHz < 0.0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application health sample rates must be finite and nonnegative.");
    }
    static_cast<void>(utcMilliseconds(health.runtimeDiagnostics.timestamp));
    static_cast<void>(pressureName(health.runtimeDiagnostics.pressure));
}

void writeStringArray(
    BoundedJsonWriter& writer,
    const std::vector<std::string>& values)
{
    writer.character('[');
    bool first{true};
    for (const auto& value : values) {
        if (!first) writer.character(',');
        first = false;
        writer.string(value);
    }
    writer.character(']');
}

void validateAgent(const Domain::AgentSpec& agent, TextBudget& budget)
{
    budget.add(agent.id.value());
    budget.add(agent.displayName);
    budget.add(agent.description);
    budget.add(agent.source);
    for (const auto* values : {
             &agent.tools,
             &agent.toolsForbidden,
             &agent.whenToUse,
             &agent.firstMoves,
             &agent.doneDefinition,
             &agent.outputSchema,
             &agent.handoff,
             &agent.qualityBar}) {
        requireCollection(values->size(), MaximumAgentItems, "agent playbook");
        for (const auto& value : *values) {
            requireText(value, MaximumAgentItemBytes, "agent playbook item");
            budget.add(value);
        }
    }
}

void writeAgent(BoundedJsonWriter& writer, const Domain::AgentSpec& agent)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "id");
    writer.string(agent.id.value());
    writer.member(first, "display_name");
    writer.string(agent.displayName);
    writer.member(first, "description");
    writer.string(agent.description);
    writer.member(first, "tools");
    writeStringArray(writer, agent.tools);
    writer.member(first, "source");
    writer.string(agent.source);
    writer.member(first, "playbook");
    writer.character('{');
    bool playbookFirst{true};
    writer.member(playbookFirst, "when_to_use");
    writeStringArray(writer, agent.whenToUse);
    writer.member(playbookFirst, "first_moves");
    writeStringArray(writer, agent.firstMoves);
    writer.member(playbookFirst, "done_definition");
    writeStringArray(writer, agent.doneDefinition);
    writer.member(playbookFirst, "output_schema");
    writeStringArray(writer, agent.outputSchema);
    writer.member(playbookFirst, "tools_primary");
    writeStringArray(writer, agent.tools);
    writer.member(playbookFirst, "tools_forbidden");
    writeStringArray(writer, agent.toolsForbidden);
    writer.member(playbookFirst, "handoff");
    writeStringArray(writer, agent.handoff);
    writer.member(playbookFirst, "quality_bar");
    writeStringArray(writer, agent.qualityBar);
    writer.character('}');
    writer.character('}');
}

void validateSession(const Domain::AgentSession& session, TextBudget& budget)
{
    budget.add(session.id.value());
    budget.add(session.agentId.value());
    if (session.clientId) budget.add(session.clientId->value());
    if (session.summary) budget.add(*session.summary);
    static_cast<void>(sessionStatusName(session.status));
    static_cast<void>(utcMilliseconds(session.createdAt));
    static_cast<void>(utcMilliseconds(session.updatedAt));
}

void writeSession(
    BoundedJsonWriter& writer,
    const Domain::AgentSession& session,
    const bool preserveNullOptionals)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "id");
    writer.string(session.id.value());
    writer.member(first, "agent_id");
    writer.string(session.agentId.value());
    if (session.clientId || preserveNullOptionals) {
        writer.member(first, "client_id");
        if (session.clientId) writer.string(session.clientId->value());
        else writer.nullValue();
    }
    writer.member(first, "status");
    writer.string(sessionStatusName(session.status));
    if (session.summary || preserveNullOptionals) {
        writer.member(first, "summary");
        if (session.summary) writer.string(*session.summary);
        else writer.nullValue();
    }
    writer.member(first, "created_at");
    writer.string(iso8601UtcSeconds(session.createdAt));
    writer.member(first, "updated_at");
    writer.string(iso8601UtcSeconds(session.updatedAt));
    writer.character('}');
}

void validateAuditEvent(const Domain::AuditEvent& event, TextBudget& budget)
{
    static_cast<void>(utcMilliseconds(event.timestamp));
    budget.add(event.tool);
    budget.add(event.status);
    if (event.clientId) budget.add(event.clientId->value());
    if (event.error) budget.add(*event.error);
    if (event.duration && event.duration->count() < 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application audit duration must not be negative.");
    }
}

void writeAuditEvent(
    BoundedJsonWriter& writer,
    const Domain::AuditEvent& event,
    const bool preserveNullOptionals)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "timestamp");
    writer.string(iso8601UtcSeconds(event.timestamp));
    if (event.clientId || preserveNullOptionals) {
        writer.member(first, "client_id");
        if (event.clientId) writer.string(event.clientId->value());
        else writer.nullValue();
    }
    writer.member(first, "tool");
    writer.string(event.tool);
    writer.member(first, "status");
    writer.string(event.status);
    if (event.duration || preserveNullOptionals) {
        writer.member(first, "duration_ms");
        if (event.duration) writer.integer(event.duration->count());
        else writer.nullValue();
    }
    if (event.error || preserveNullOptionals) {
        writer.member(first, "error");
        if (event.error) writer.string(*event.error);
        else writer.nullValue();
    }
    if (preserveNullOptionals) {
        writer.member(first, "args_json");
        writer.nullValue();
    }
    writer.character('}');
}

void validateManagerStatus(const Domain::ManagerStatus& status)
{
    static_cast<void>(managerStateName(status.state));
    if (status.startedAt) static_cast<void>(utcMilliseconds(*status.startedAt));
    if ((status.uptime && status.uptime->count() < 0) ||
        status.watchdogInterval.count() <= 0 ||
        status.dashboardRefreshInterval.count() <= 0 ||
        status.dashboardPort == 0U) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application manager status contains an invalid duration or port.");
    }
    static_cast<void>(dashboardUrl(status.dashboardHost, status.dashboardPort));
    requireText(
        status.home.value(), Domain::PathText::MaximumBytes, "manager home", false);
    requireText(
        status.version,
        DashboardApplicationLimits::MaximumVersionBytes,
        "manager version",
        false);
    if (status.lastError) {
        requireText(*status.lastError, MaximumGeneralTextBytes, "manager last error");
    }
}

void writeManagerStatus(
    BoundedJsonWriter& writer,
    const Domain::ManagerStatus& status)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "ok");
    writer.boolean(status.ok);
    writer.member(first, "manager");
    writer.boolean(status.isManager);
    writer.member(first, "state");
    writer.string(managerStateName(status.state));
    writer.member(first, "desired_running");
    writer.boolean(status.desiredRunning);
    writer.member(first, "http_listening");
    writer.boolean(status.httpListening);
    writer.member(first, "service_active");
    writer.boolean(status.serviceActive);
    writer.member(first, "pid");
    writer.integer(status.processId);
    writer.member(first, "started_at");
    if (status.startedAt) writer.string(iso8601UtcSeconds(*status.startedAt));
    else writer.nullValue();
    writer.member(first, "uptime_sec");
    if (status.uptime) writer.integer(status.uptime->count());
    else writer.nullValue();
    writer.member(first, "restart_count");
    writer.integer(status.restartCount);
    writer.member(first, "last_error");
    if (status.lastError) writer.string(*status.lastError);
    else writer.nullValue();
    writer.member(first, "auto_restart");
    writer.boolean(status.autoRestart);
    writer.member(first, "watchdog_interval_sec");
    writer.integer(status.watchdogInterval.count());
    writer.member(first, "open_browser_on_start");
    writer.boolean(status.openBrowserOnStart);
    writer.member(first, "dashboard");
    writer.character('{');
    bool dashboardFirst{true};
    writer.member(dashboardFirst, "host");
    writer.string(status.dashboardHost);
    writer.member(dashboardFirst, "port");
    writer.integer(status.dashboardPort);
    writer.member(dashboardFirst, "url");
    writer.string(dashboardUrl(status.dashboardHost, status.dashboardPort));
    writer.member(dashboardFirst, "refresh_interval_sec");
    writer.integer(status.dashboardRefreshInterval.count());
    writer.character('}');
    writer.member(first, "home");
    writer.string(status.home.value());
    writer.member(first, "version");
    writer.string(status.version);
    writer.character('}');
}

void validateStatus(const DashboardApplicationStatus& status)
{
    requireText(
        status.identity.product,
        DashboardApplicationLimits::MaximumProductBytes,
        "product",
        false);
    requireText(
        status.identity.version,
        DashboardApplicationLimits::MaximumVersionBytes,
        "version",
        false);
    requireText(
        status.identity.runtime,
        DashboardApplicationLimits::MaximumRuntimeBytes,
        "runtime",
        false);
    requireText(
        status.identity.home.value(),
        Domain::PathText::MaximumBytes,
        "home",
        false);
    requireText(
        status.identity.store.value(),
        Domain::PathText::MaximumBytes,
        "store",
        false);
    TextBudget identityText{
        DashboardApplicationLimits::MaximumIdentityTextBytes};
    identityText.add(status.identity.product);
    identityText.add(status.identity.version);
    identityText.add(status.identity.runtime);
    identityText.add(status.identity.home.value());
    identityText.add(status.identity.store.value());
    if (status.identity.processId == 0U) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application identity requires a nonzero process id.");
    }
    requireText(
        status.dashboardHost,
        DashboardApplicationLimits::MaximumDashboardHostBytes,
        "dashboard host",
        false);
    if (status.dashboardPort == 0U) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard application status requires a nonzero listener port.");
    }
    static_cast<void>(dashboardUrl(status.dashboardHost, status.dashboardPort));

    const auto& data = status.operational;
    requireCollection(
        data.agents.size(),
        DashboardApplicationLimits::MaximumAgentSpecs,
        "status agents");
    requireCollection(
        data.openSessions.size(),
        DashboardApplicationLimits::MaximumOpenSessions,
        "status open sessions");
    requireCollection(
        data.presence.size(),
        DashboardApplicationLimits::MaximumPresenceRecords,
        "status presence");
    requireCollection(
        data.recentAudit.size(),
        DashboardApplicationLimits::MaximumStatusAuditEvents,
        "status recent audit");
    requireCollection(
        data.toolNames.size(),
        DashboardApplicationLimits::MaximumToolNames,
        "status tools");

    TextBudget agentText{DashboardApplicationLimits::MaximumAgentTextBytes};
    for (const auto& agent : data.agents) agentText.add(agent.id.value());
    TextBudget sessionText{DashboardApplicationLimits::MaximumSessionTextBytes};
    for (const auto& session : data.openSessions) {
        validateSession(session, sessionText);
        if (!Domain::isOpen(session.status)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard application status contains a non-open session in open_sessions.");
        }
    }
    TextBudget presenceText{DashboardApplicationLimits::MaximumPresenceTextBytes};
    for (const auto& presence : data.presence) {
        requireText(
            presence.clientId,
            DashboardApplicationLimits::MaximumPresenceClientIdBytes,
            "presence client id",
            false);
        requireText(
            presence.hostKind,
            DashboardApplicationLimits::MaximumPresenceHostKindBytes,
            "presence host kind",
            false);
        presenceText.add(presence.clientId);
        presenceText.add(presence.hostKind);
        presenceText.add(presence.workingDirectory.value());
        if (presence.processId == 0U) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard application presence requires a nonzero process id.");
        }
        static_cast<void>(utcMilliseconds(presence.lastHeartbeat));
    }
    TextBudget auditText{DashboardApplicationLimits::MaximumAuditTextBytes};
    for (const auto& event : data.recentAudit) {
        validateAuditEvent(event, auditText);
    }
    TextBudget toolText{DashboardApplicationLimits::MaximumToolNameTextBytes};
    for (const auto& tool : data.toolNames) {
        requireText(
            tool,
            DashboardApplicationLimits::MaximumToolNameBytes,
            "tool name",
            false);
        toolText.add(tool);
    }
    validateHealth(status.telemetry);
    validateManagerStatus(status.manager);
    static_cast<void>(utcMilliseconds(data.runtimeDiagnostics.timestamp));
    static_cast<void>(pressureName(data.runtimeDiagnostics.pressure));
}

void writePresence(
    BoundedJsonWriter& writer,
    const DashboardPresenceRecord& presence)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "client_id");
    writer.string(presence.clientId);
    writer.member(first, "host_kind");
    writer.string(presence.hostKind);
    writer.member(first, "pid");
    writer.integer(presence.processId);
    writer.member(first, "cwd");
    writer.string(presence.workingDirectory.value());
    writer.member(first, "last_heartbeat");
    writer.string(iso8601UtcSeconds(presence.lastHeartbeat));
    writer.character('}');
}

void writeStatus(
    BoundedJsonWriter& writer,
    const DashboardApplicationStatus& status)
{
    const auto& data = status.operational;
    writer.character('{');
    bool first{true};
    writer.member(first, "ok");
    writer.boolean(true);
    writer.member(first, "version");
    writer.string(status.identity.version);
    writer.member(first, "product");
    writer.string(status.identity.product);
    writer.member(first, "runtime");
    writer.string(status.identity.runtime);
    writer.member(first, "home");
    writer.string(status.identity.home.value());
    writer.member(first, "store");
    writer.string(status.identity.store.value());
    writer.member(first, "agents");
    writer.character('[');
    for (std::size_t index{}; index < data.agents.size(); ++index) {
        if (index != 0U) writer.character(',');
        writer.string(data.agents[index].id.value());
    }
    writer.character(']');
    writer.member(first, "agent_count");
    writer.integer(data.agents.size());
    writer.member(first, "open_sessions");
    writer.character('[');
    for (std::size_t index{}; index < data.openSessions.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeSession(writer, data.openSessions[index], true);
    }
    writer.character(']');
    writer.member(first, "open_session_count");
    writer.integer(data.openSessions.size());
    writer.member(first, "presence");
    writer.character('[');
    for (std::size_t index{}; index < data.presence.size(); ++index) {
        if (index != 0U) writer.character(',');
        writePresence(writer, data.presence[index]);
    }
    writer.character(']');
    writer.member(first, "presence_count");
    writer.integer(data.presence.size());
    writer.member(first, "recent_audit");
    writer.character('[');
    for (std::size_t index{}; index < data.recentAudit.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeAuditEvent(writer, data.recentAudit[index], true);
    }
    writer.character(']');
    writer.member(first, "tools");
    writeStringArray(writer, data.toolNames);
    writer.member(first, "telemetry");
    writeTelemetryReport(
        writer, status.telemetry.report, true, &status.telemetry, false);
    writer.member(first, "dashboard");
    writer.character('{');
    bool dashboardFirst{true};
    writer.member(dashboardFirst, "host");
    writer.string(status.dashboardHost);
    writer.member(dashboardFirst, "port");
    writer.integer(status.dashboardPort);
    writer.character('}');
    writer.member(first, "pid");
    writer.integer(status.identity.processId);
    writer.member(first, "runtime_diagnostics");
    writeRuntimeDiagnostics(writer, data.runtimeDiagnostics);
    writer.member(first, "manager");
    writeManagerStatus(writer, status.manager);
    writer.member(first, "service_active");
    writer.boolean(status.serviceActive);
    writer.character('}');
}

void validateDoctor(const Domain::DoctorReport& report)
{
    requireCollection(
        report.checks.size(),
        DashboardApplicationLimits::MaximumDoctorChecks,
        "doctor checks");
    requireText(
        report.version,
        DashboardApplicationLimits::MaximumVersionBytes,
        "doctor version",
        false);
    TextBudget budget{DashboardApplicationLimits::MaximumDoctorTextBytes};
    budget.add(report.version);
    budget.add(report.home.value());
    budget.add(report.binaryPath.value());
    for (const auto& check : report.checks) {
        requireText(check.name, MaximumGeneralTextBytes, "doctor check name", false);
        requireText(check.detail, MaximumGeneralTextBytes, "doctor check detail");
        budget.add(check.name);
        budget.add(check.detail);
    }
    validateTelemetryReport(report.telemetry, budget);
}

void writeDoctor(BoundedJsonWriter& writer, const Domain::DoctorReport& report)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "ok");
    writer.boolean(report.ok);
    writer.member(first, "version");
    writer.string(report.version);
    writer.member(first, "home");
    writer.string(report.home.value());
    writer.member(first, "checks");
    writer.character('[');
    for (std::size_t index{}; index < report.checks.size(); ++index) {
        if (index != 0U) writer.character(',');
        const auto& check = report.checks[index];
        writer.character('{');
        bool checkFirst{true};
        writer.member(checkFirst, "name");
        writer.string(check.name);
        writer.member(checkFirst, "ok");
        writer.boolean(check.ok);
        writer.member(checkFirst, "detail");
        writer.string(check.detail);
        writer.character('}');
    }
    writer.character(']');
    writer.member(first, "telemetry");
    writeTelemetryReport(writer, report.telemetry, false, nullptr, false);
    writer.member(first, "binary");
    writer.character('{');
    bool binaryFirst{true};
    writer.member(binaryFirst, "installed");
    writer.boolean(report.binaryInstalled);
    writer.member(binaryFirst, "path");
    writer.string(report.binaryPath.value());
    writer.character('}');
    writer.character('}');
}

template <typename Validate, typename Write>
[[nodiscard]] Domain::Result<std::string> encode(
    const std::size_t maximumBytes,
    Validate&& validate,
    Write&& write) noexcept
{
    try {
        validateMaximumBytes(maximumBytes);
        validate();
        BoundedJsonWriter writer{maximumBytes};
        write(writer);
        auto payload = std::move(writer).finish();
        if (payload.empty()) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard application encoder produced an empty document.");
        }
        return Domain::Result<std::string>::success(std::move(payload));
    } catch (const ApplicationCodecException& error) {
        return Domain::Result<std::string>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard application JSON encoding failed unexpectedly."));
    }
}

} // namespace

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeHealth(
    const DashboardTelemetryHealth& health,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] { validateHealth(health); },
        [&](BoundedJsonWriter& writer) {
            writeTelemetryReport(writer, health.report, true, &health, true);
        });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeStatus(
    const DashboardApplicationStatus& status,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] { validateStatus(status); },
        [&](BoundedJsonWriter& writer) { writeStatus(writer, status); });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeDoctor(
    const Domain::DoctorReport& report,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] { validateDoctor(report); },
        [&](BoundedJsonWriter& writer) { writeDoctor(writer, report); });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeAgents(
    const std::vector<Domain::AgentSpec>& agents,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] {
            requireCollection(
                agents.size(),
                DashboardApplicationLimits::MaximumAgentSpecs,
                "agents");
            TextBudget text{DashboardApplicationLimits::MaximumAgentTextBytes};
            for (const auto& agent : agents) validateAgent(agent, text);
        },
        [&](BoundedJsonWriter& writer) {
            writer.character('{');
            bool first{true};
            writer.member(first, "ok");
            writer.boolean(true);
            writer.member(first, "agents");
            writer.character('[');
            for (std::size_t index{}; index < agents.size(); ++index) {
                if (index != 0U) writer.character(',');
                writeAgent(writer, agents[index]);
            }
            writer.character(']');
            writer.character('}');
        });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeSessions(
    const DashboardSessionListing& sessions,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] {
            requireCollection(
                sessions.open.size(),
                DashboardApplicationLimits::MaximumOpenSessions,
                "open sessions");
            requireCollection(
                sessions.recent.size(),
                DashboardApplicationLimits::MaximumRecentSessions,
                "recent sessions");
            TextBudget text{DashboardApplicationLimits::MaximumSessionTextBytes};
            for (const auto& session : sessions.open) {
                validateSession(session, text);
                if (!Domain::isOpen(session.status)) {
                    reject(
                        Domain::ErrorCodes::InvalidRequest,
                        "Dashboard application sessions contain a non-open row in open.");
                }
            }
            for (const auto& session : sessions.recent) validateSession(session, text);
        },
        [&](BoundedJsonWriter& writer) {
            writer.character('{');
            bool first{true};
            writer.member(first, "ok");
            writer.boolean(true);
            writer.member(first, "open");
            writer.character('[');
            for (std::size_t index{}; index < sessions.open.size(); ++index) {
                if (index != 0U) writer.character(',');
                writeSession(writer, sessions.open[index], false);
            }
            writer.character(']');
            writer.member(first, "recent");
            writer.character('[');
            for (std::size_t index{}; index < sessions.recent.size(); ++index) {
                if (index != 0U) writer.character(',');
                writeSession(writer, sessions.recent[index], false);
            }
            writer.character(']');
            writer.character('}');
        });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeAudit(
    const std::vector<Domain::AuditEvent>& events,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] {
            requireCollection(
                events.size(),
                DashboardApplicationLimits::MaximumAuditEvents,
                "audit events");
            TextBudget text{DashboardApplicationLimits::MaximumAuditTextBytes};
            for (const auto& event : events) validateAuditEvent(event, text);
        },
        [&](BoundedJsonWriter& writer) {
            writer.character('{');
            bool first{true};
            writer.member(first, "ok");
            writer.boolean(true);
            writer.member(first, "events");
            writer.character('[');
            for (std::size_t index{}; index < events.size(); ++index) {
                if (index != 0U) writer.character(',');
                writeAuditEvent(writer, events[index], false);
            }
            writer.character(']');
            writer.character('}');
        });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeDiagnostics(
    const std::vector<std::string>& lines,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] {
            requireCollection(
                lines.size(),
                DashboardApplicationLimits::MaximumDiagnosticLines,
                "diagnostic lines");
            TextBudget text{DashboardApplicationLimits::MaximumDiagnosticTextBytes};
            for (const auto& line : lines) {
                requireText(
                    line,
                    DashboardApplicationLimits::MaximumDiagnosticLineBytes,
                    "diagnostic line");
                text.add(line);
            }
        },
        [&](BoundedJsonWriter& writer) {
            writer.character('{');
            bool first{true};
            writer.member(first, "ok");
            writer.boolean(true);
            writer.member(first, "lines");
            writeStringArray(writer, lines);
            writer.character('}');
        });
}

Domain::Result<std::string>
DashboardApplicationJsonCodec::encodePruneAcknowledgement(
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [] {},
        [](BoundedJsonWriter& writer) {
            writer.raw("{\"ok\":true,\"message\":\"Pruned stale sessions\"}");
        });
}

Domain::Result<std::string> DashboardApplicationJsonCodec::encodeClosedSession(
    const Domain::AgentSession& session,
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [&] {
            TextBudget text{DashboardApplicationLimits::MaximumSessionTextBytes};
            validateSession(session, text);
        },
        [&](BoundedJsonWriter& writer) {
            writer.character('{');
            bool first{true};
            writer.member(first, "ok");
            writer.boolean(true);
            writer.member(first, "session");
            writeSession(writer, session, false);
            writer.character('}');
        });
}

Domain::Result<std::string>
DashboardApplicationJsonCodec::encodeRestartAcknowledgement(
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [] {},
        [](BoundedJsonWriter& writer) {
            writer.raw(
                "{\"ok\":true,\"message\":\"Manager restart accepted\",\"state\":\"restarting\"}");
        });
}

Domain::Result<std::string>
DashboardApplicationJsonCodec::encodeShutdownAcknowledgement(
    const std::size_t maximumBytes) noexcept
{
    return encode(
        maximumBytes,
        [] {},
        [](BoundedJsonWriter& writer) {
            writer.raw(
                "{\"ok\":true,\"message\":\"Manager shutting down\",\"state\":\"stopping\"}");
        });
}

} // namespace ForgeConductor::Dashboard
