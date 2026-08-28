#include "ForgeConductor/Application/DashboardOperationalService.h"

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

namespace ForgeConductor::Application {
namespace {

constexpr std::size_t MaximumAgentItems = 256U;
constexpr std::size_t MaximumAgentItemBytes = 4U * 1024U;
constexpr std::size_t MaximumGeneralTextBytes = 64U * 1024U;
constexpr std::int64_t FirstUnsupportedUtcMillisecond =
    253'402'300'800'000LL;

[[nodiscard]] Domain::Error internalError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::move(message));
}

[[nodiscard]] Domain::Error integrityError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] Domain::Error limitError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::LimitExceeded, std::move(message));
}

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagateFailure(Domain::Result<U> result)
{
    return Domain::Result<T>::failure(std::move(result).error());
}

[[nodiscard]] Domain::Result<void> validateProducerText(
    const std::string_view value,
    const std::size_t maximumBytes,
    const bool allowEmpty,
    const std::string_view field)
{
    if ((!allowEmpty && value.empty()) || value.size() > maximumBytes) {
        return Domain::Result<void>::failure(limitError(
            "Dashboard operational source field exceeds its bound: " +
            std::string{field} + '.'));
    }
    if (value.find('\0') != std::string_view::npos ||
        !Domain::isValidUtf8(value)) {
        return Domain::Result<void>::failure(integrityError(
            "Dashboard operational source field is not valid UTF-8 text: " +
            std::string{field} + '.'));
    }
    return Domain::Result<void>::success();
}

class TextBudget final {
public:
    explicit TextBudget(const std::size_t maximumBytes) noexcept
        : maximumBytes_{maximumBytes}
    {
    }

    [[nodiscard]] Domain::Result<void> add(
        const std::string_view value,
        const std::string_view field) noexcept
    {
        try {
            auto valid = validateProducerText(
                value, (std::numeric_limits<std::size_t>::max)(), true, field);
            if (!valid) {
                return valid;
            }
            if (bytes_ > maximumBytes_ ||
                value.size() > maximumBytes_ - bytes_) {
                return Domain::Result<void>::failure(limitError(
                    "Dashboard operational source text exceeds its aggregate "
                    "bound."));
            }
            bytes_ += value.size();
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(internalError(
                "Dashboard operational source text could not be bounded."));
        }
    }

private:
    const std::size_t maximumBytes_;
    std::size_t bytes_{};
};

[[nodiscard]] Domain::Result<void> validateCollection(
    const std::size_t actual,
    const std::size_t maximum,
    const std::string_view field)
{
    if (actual > maximum) {
        return Domain::Result<void>::failure(limitError(
            "Dashboard operational source collection exceeds its bound: " +
            std::string{field} + '.'));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool isSupportedUtc(
    const Domain::UtcTimePoint timestamp) noexcept
{
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(
        timestamp.time_since_epoch());
    return milliseconds.count() >= 0 &&
        milliseconds.count() < FirstUnsupportedUtcMillisecond;
}

[[nodiscard]] bool isKnownSessionStatus(
    const Domain::SessionStatus status) noexcept
{
    switch (status) {
    case Domain::SessionStatus::Open:
    case Domain::SessionStatus::Active:
    case Domain::SessionStatus::Running:
    case Domain::SessionStatus::Started:
    case Domain::SessionStatus::Closed:
    case Domain::SessionStatus::Completed:
    case Domain::SessionStatus::Failed:
        return true;
    }
    return false;
}

[[nodiscard]] bool isKnownPressure(
    const Domain::ResourcePressureLevel pressure) noexcept
{
    switch (pressure) {
    case Domain::ResourcePressureLevel::Nominal:
    case Domain::ResourcePressureLevel::Warning:
    case Domain::ResourcePressureLevel::Critical:
        return true;
    }
    return false;
}

[[nodiscard]] Domain::Result<void> addAgentList(
    const std::vector<std::string>& values,
    TextBudget& budget,
    const std::string_view field)
{
    auto valid = validateCollection(values.size(), MaximumAgentItems, field);
    if (!valid) {
        return valid;
    }
    for (const auto& value : values) {
        valid = validateProducerText(
            value, MaximumAgentItemBytes, true, field);
        if (!valid) {
            return valid;
        }
        valid = budget.add(value, field);
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateAgent(
    const Domain::AgentSpec& agent,
    TextBudget& budget)
{
    for (const auto* value : {
             &agent.id.value(),
             &agent.displayName,
             &agent.description,
             &agent.source}) {
        auto valid = budget.add(*value, "agent");
        if (!valid) {
            return valid;
        }
    }
    for (const auto* values : {
             &agent.tools,
             &agent.toolsForbidden,
             &agent.whenToUse,
             &agent.firstMoves,
             &agent.doneDefinition,
             &agent.outputSchema,
             &agent.handoff,
             &agent.qualityBar}) {
        auto valid = addAgentList(*values, budget, "agent playbook");
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateAgents(
    const std::vector<Domain::AgentSpec>& agents)
{
    auto valid = validateCollection(
        agents.size(),
        Dashboard::DashboardApplicationLimits::MaximumAgentSpecs,
        "agents");
    if (!valid) {
        return valid;
    }
    TextBudget budget{
        Dashboard::DashboardApplicationLimits::MaximumAgentTextBytes};
    for (const auto& agent : agents) {
        valid = validateAgent(agent, budget);
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateSession(
    const Domain::AgentSession& session,
    TextBudget& budget)
{
    if (!isKnownSessionStatus(session.status) ||
        !isSupportedUtc(session.createdAt) ||
        !isSupportedUtc(session.updatedAt) ||
        session.updatedAt < session.createdAt) {
        return Domain::Result<void>::failure(integrityError(
            "Dashboard operational source returned an invalid agent session."));
    }
    for (const auto* value : {&session.id.value(), &session.agentId.value()}) {
        auto valid = budget.add(*value, "session identity");
        if (!valid) {
            return valid;
        }
    }
    if (session.clientId) {
        auto valid = budget.add(
            session.clientId->value(), "session client identity");
        if (!valid) {
            return valid;
        }
    }
    if (session.summary) {
        auto valid = budget.add(*session.summary, "session summary");
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validatePresence(
    const std::vector<Dashboard::DashboardPresenceRecord>& presence)
{
    auto valid = validateCollection(
        presence.size(),
        Dashboard::DashboardApplicationLimits::MaximumPresenceRecords,
        "presence");
    if (!valid) {
        return valid;
    }
    TextBudget budget{
        Dashboard::DashboardApplicationLimits::MaximumPresenceTextBytes};
    for (const auto& record : presence) {
        valid = validateProducerText(
            record.clientId,
            Dashboard::DashboardApplicationLimits::
                MaximumPresenceClientIdBytes,
            false,
            "presence client id");
        if (!valid) {
            return valid;
        }
        valid = validateProducerText(
            record.hostKind,
            Dashboard::DashboardApplicationLimits::
                MaximumPresenceHostKindBytes,
            false,
            "presence host kind");
        if (!valid) {
            return valid;
        }
        if (record.processId == 0U || !isSupportedUtc(record.lastHeartbeat)) {
            return Domain::Result<void>::failure(integrityError(
                "Dashboard operational source returned invalid presence "
                "metadata."));
        }
        for (const auto* value : {
                 &record.clientId,
                 &record.hostKind,
                 &record.workingDirectory.value()}) {
            valid = budget.add(*value, "presence");
            if (!valid) {
                return valid;
            }
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateSourceSnapshot(
    const DashboardOperationalSourceSnapshot& snapshot)
{
    auto valid = validateCollection(
        snapshot.openSessions.size(),
        Dashboard::DashboardApplicationLimits::MaximumOpenSessions,
        "open sessions");
    if (!valid) {
        return valid;
    }
    valid = validateCollection(
        snapshot.recentSessions.size(),
        Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
        "recent sessions");
    if (!valid) {
        return valid;
    }

    TextBudget sessions{
        Dashboard::DashboardApplicationLimits::MaximumSessionTextBytes};
    for (const auto& session : snapshot.openSessions) {
        if (!Domain::isOpen(session.status)) {
            return Domain::Result<void>::failure(integrityError(
                "Dashboard operational source placed a terminal session in "
                "the open projection."));
        }
        valid = validateSession(session, sessions);
        if (!valid) {
            return valid;
        }
    }
    for (const auto& session : snapshot.recentSessions) {
        valid = validateSession(session, sessions);
        if (!valid) {
            return valid;
        }
    }
    return validatePresence(snapshot.presence);
}

[[nodiscard]] Domain::Result<void> validateAuditEvents(
    const std::vector<Domain::AuditEvent>& events,
    const std::size_t maximumEvents)
{
    auto valid = validateCollection(events.size(), maximumEvents, "audit");
    if (!valid) {
        return valid;
    }
    TextBudget budget{
        Dashboard::DashboardApplicationLimits::MaximumAuditTextBytes};
    for (const auto& event : events) {
        if (!isSupportedUtc(event.timestamp) ||
            (event.duration && event.duration->count() < 0)) {
            return Domain::Result<void>::failure(integrityError(
                "Dashboard operational source returned an invalid audit event."));
        }
        for (const auto* value : {&event.tool, &event.status}) {
            valid = budget.add(*value, "audit");
            if (!valid) {
                return valid;
            }
        }
        if (event.clientId) {
            valid = budget.add(event.clientId->value(), "audit client id");
            if (!valid) {
                return valid;
            }
        }
        if (event.error) {
            valid = budget.add(*event.error, "audit error");
            if (!valid) {
                return valid;
            }
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateRuntimeDiagnostics(
    const Domain::RuntimeDiagnosticSnapshot& snapshot)
{
    if (!isSupportedUtc(snapshot.timestamp) ||
        !isKnownPressure(snapshot.pressure)) {
        return Domain::Result<void>::failure(integrityError(
            "Dashboard operational source returned invalid runtime "
            "diagnostics."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateTelemetryReport(
    const Domain::TelemetryHealthReport& telemetry,
    TextBudget& budget)
{
    for (const auto* value : {
             &telemetry.service,
             &telemetry.runtime,
             &telemetry.mode,
             &telemetry.collectors,
             &telemetry.ui}) {
        auto valid = validateProducerText(
            *value, MaximumGeneralTextBytes, true, "doctor telemetry");
        if (!valid) {
            return valid;
        }
        valid = budget.add(*value, "doctor telemetry");
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateDoctorReport(
    const Domain::DoctorReport& report)
{
    auto valid = validateCollection(
        report.checks.size(),
        Dashboard::DashboardApplicationLimits::MaximumDoctorChecks,
        "doctor checks");
    if (!valid) {
        return valid;
    }
    valid = validateProducerText(
        report.version,
        Dashboard::DashboardApplicationLimits::MaximumVersionBytes,
        false,
        "doctor version");
    if (!valid) {
        return valid;
    }
    TextBudget budget{
        Dashboard::DashboardApplicationLimits::MaximumDoctorTextBytes};
    for (const auto* value : {
             &report.version,
             &report.home.value(),
             &report.binaryPath.value()}) {
        valid = budget.add(*value, "doctor");
        if (!valid) {
            return valid;
        }
    }
    for (const auto& check : report.checks) {
        valid = validateProducerText(
            check.name, MaximumGeneralTextBytes, false, "doctor check name");
        if (!valid) {
            return valid;
        }
        valid = validateProducerText(
            check.detail, MaximumGeneralTextBytes, true, "doctor check detail");
        if (!valid) {
            return valid;
        }
        valid = budget.add(check.name, "doctor check name");
        if (!valid) {
            return valid;
        }
        valid = budget.add(check.detail, "doctor check detail");
        if (!valid) {
            return valid;
        }
    }
    return validateTelemetryReport(report.telemetry, budget);
}

[[nodiscard]] Domain::Result<std::vector<std::string>> copyToolNames(
    const Contracts::IToolCatalog& catalog) noexcept
{
    try {
        const auto descriptors = catalog.tools();
        auto valid = validateCollection(
            descriptors.size(),
            Dashboard::DashboardApplicationLimits::MaximumToolNames,
            "tools");
        if (!valid) {
            return propagateFailure<std::vector<std::string>>(std::move(valid));
        }

        std::vector<std::string> names;
        names.reserve(descriptors.size());
        std::set<std::string_view, std::less<>> uniqueNames;
        TextBudget budget{
            Dashboard::DashboardApplicationLimits::MaximumToolNameTextBytes};
        for (const auto& descriptor : descriptors) {
            auto descriptorValid =
                Domain::validateToolDescriptor(descriptor.tool);
            if (!descriptorValid) {
                return Domain::Result<std::vector<std::string>>::failure(
                    integrityError(
                        "Dashboard operational tool catalog contains an "
                        "invalid descriptor."));
            }
            valid = validateProducerText(
                descriptor.tool.name,
                Dashboard::DashboardApplicationLimits::MaximumToolNameBytes,
                false,
                "tool name");
            if (!valid) {
                return propagateFailure<std::vector<std::string>>(
                    std::move(valid));
            }
            valid = budget.add(descriptor.tool.name, "tool name");
            if (!valid) {
                return propagateFailure<std::vector<std::string>>(
                    std::move(valid));
            }
            if (!uniqueNames.insert(descriptor.tool.name).second) {
                return Domain::Result<std::vector<std::string>>::failure(
                    integrityError(
                        "Dashboard operational tool catalog contains a "
                        "duplicate name."));
            }
            names.push_back(descriptor.tool.name);
        }
        return Domain::Result<std::vector<std::string>>::success(
            std::move(names));
    } catch (...) {
        return Domain::Result<std::vector<std::string>>::failure(internalError(
            "Dashboard operational tool names could not be copied."));
    }
}

[[nodiscard]] Domain::Result<void> validateDiagnosticLines(
    const std::vector<std::string>& lines)
{
    auto valid = validateCollection(
        lines.size(),
        Dashboard::DashboardApplicationLimits::MaximumDiagnosticLines,
        "diagnostic lines");
    if (!valid) {
        return valid;
    }
    TextBudget budget{
        Dashboard::DashboardApplicationLimits::MaximumDiagnosticTextBytes};
    for (const auto& line : lines) {
        valid = validateProducerText(
            line,
            Dashboard::DashboardApplicationLimits::MaximumDiagnosticLineBytes,
            true,
            "diagnostic line");
        if (!valid) {
            return valid;
        }
        valid = budget.add(line, "diagnostic line");
        if (!valid) {
            return valid;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateCloseRequest(
    const Dashboard::DashboardSessionCloseRequest& request)
{
    const auto& summary = request.summary();
    if (summary.find('\0') != std::string::npos ||
        !Domain::isValidUtf8(summary)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard session-close summary is not valid UTF-8 text."));
    }
    const auto scalarCount = static_cast<std::size_t>(std::count_if(
        summary.begin(), summary.end(), [](const char value) noexcept {
            return (static_cast<unsigned char>(value) & 0xc0U) != 0x80U;
        }));
    if (scalarCount > Domain::AgentSessionLimits::MaximumSummaryUnits) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge,
            "Dashboard session-close summary exceeds its scalar limit."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateClosedSession(
    const Domain::AgentSession& session,
    const Dashboard::DashboardSessionCloseRequest& request)
{
    TextBudget budget{
        Dashboard::DashboardApplicationLimits::MaximumSessionTextBytes};
    auto valid = validateSession(session, budget);
    if (!valid) {
        return valid;
    }
    if (session.id != request.sessionId() ||
        session.status != Domain::SessionStatus::Closed ||
        session.summary != std::optional<std::string>{request.summary()}) {
        return Domain::Result<void>::failure(integrityError(
            "Dashboard operational source returned a mismatched administrative "
            "session close."));
    }
    return Domain::Result<void>::success();
}

} // namespace

class DashboardOperationalService::Impl final {
public:
    explicit Impl(DashboardOperationalServiceDependencies dependencies) noexcept
        : agentCatalog_{dependencies.agentCatalog},
          agentSessions_{dependencies.agentSessions},
          auditRepository_{dependencies.auditRepository},
          doctorService_{dependencies.doctorService},
          runtimeDiagnostics_{dependencies.runtimeDiagnostics},
          toolCatalog_{dependencies.toolCatalog},
          clock_{dependencies.clock},
          dataSource_{dependencies.dataSource}
    {
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Dashboard::DashboardStatusData> status(
        const Domain::OperationContext& context) noexcept
    {
        return execute<Dashboard::DashboardStatusData>(context, [&]() {
            auto agents = agentCatalog_.all(context);
            if (!agents) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(agents));
            }
            auto valid = validateAgents(agents.value());
            if (!valid) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(valid));
            }

            auto source = dataSource_.snapshot(
                Dashboard::DashboardApplicationLimits::MaximumOpenSessions,
                Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
                Dashboard::DashboardApplicationLimits::MaximumPresenceRecords,
                context);
            if (!source) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(source));
            }
            valid = validateSourceSnapshot(source.value());
            if (!valid) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(valid));
            }

            auto audit = auditRepository_.recent(
                Dashboard::DashboardApplicationLimits::
                    MaximumStatusAuditEvents,
                context);
            if (!audit) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(audit));
            }
            valid = validateAuditEvents(
                audit.value(),
                Dashboard::DashboardApplicationLimits::
                    MaximumStatusAuditEvents);
            if (!valid) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(valid));
            }

            auto tools = copyToolNames(toolCatalog_);
            if (!tools) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(tools));
            }

            auto runtime = runtimeDiagnostics_.snapshot(context);
            if (!runtime) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(runtime));
            }
            valid = validateRuntimeDiagnostics(runtime.value());
            if (!valid) {
                return propagateFailure<Dashboard::DashboardStatusData>(
                    std::move(valid));
            }

            auto sourceValue = std::move(source).value();
            return Domain::Result<Dashboard::DashboardStatusData>::success(
                Dashboard::DashboardStatusData{
                    std::move(agents).value(),
                    std::move(sourceValue.openSessions),
                    std::move(sourceValue.presence),
                    std::move(audit).value(),
                    std::move(tools).value(),
                    std::move(runtime).value()});
        });
    }

    [[nodiscard]] Domain::Result<Domain::DoctorReport> doctor(
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::DoctorReport>(context, [&]() {
            auto report = doctorService_.run(context);
            if (!report) {
                return report;
            }
            auto valid = validateDoctorReport(report.value());
            if (!valid) {
                return propagateFailure<Domain::DoctorReport>(
                    std::move(valid));
            }
            return report;
        });
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> agents(
        const Domain::OperationContext& context) noexcept
    {
        return execute<std::vector<Domain::AgentSpec>>(context, [&]() {
            auto values = agentCatalog_.all(context);
            if (!values) {
                return values;
            }
            auto valid = validateAgents(values.value());
            if (!valid) {
                return propagateFailure<std::vector<Domain::AgentSpec>>(
                    std::move(valid));
            }
            return values;
        });
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardSessionListing> sessions(
        const Domain::OperationContext& context) noexcept
    {
        return execute<Dashboard::DashboardSessionListing>(context, [&]() {
            auto source = dataSource_.snapshot(
                Dashboard::DashboardApplicationLimits::MaximumOpenSessions,
                Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
                Dashboard::DashboardApplicationLimits::MaximumPresenceRecords,
                context);
            if (!source) {
                return propagateFailure<Dashboard::DashboardSessionListing>(
                    std::move(source));
            }
            auto valid = validateSourceSnapshot(source.value());
            if (!valid) {
                return propagateFailure<Dashboard::DashboardSessionListing>(
                    std::move(valid));
            }
            auto value = std::move(source).value();
            return Domain::Result<Dashboard::DashboardSessionListing>::success(
                Dashboard::DashboardSessionListing{
                    std::move(value.openSessions),
                    std::move(value.recentSessions)});
        });
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> audit(
        const Domain::OperationContext& context) noexcept
    {
        return execute<std::vector<Domain::AuditEvent>>(context, [&]() {
            auto values = auditRepository_.recent(
                Dashboard::DashboardApplicationLimits::MaximumAuditEvents,
                context);
            if (!values) {
                return values;
            }
            auto valid = validateAuditEvents(
                values.value(),
                Dashboard::DashboardApplicationLimits::MaximumAuditEvents);
            if (!valid) {
                return propagateFailure<std::vector<Domain::AuditEvent>>(
                    std::move(valid));
            }
            return values;
        });
    }

    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        const Domain::OperationContext& context) noexcept
    {
        return execute<std::vector<std::string>>(context, [&]() {
            auto lines = dataSource_.diagnosticLines(
                Dashboard::DashboardApplicationLimits::MaximumDiagnosticLines,
                Dashboard::DashboardApplicationLimits::
                    MaximumDiagnosticLineBytes,
                Dashboard::DashboardApplicationLimits::
                    MaximumDiagnosticTextBytes,
                context);
            if (!lines) {
                return lines;
            }
            auto valid = validateDiagnosticLines(lines.value());
            if (!valid) {
                return propagateFailure<std::vector<std::string>>(
                    std::move(valid));
            }
            return lines;
        });
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneSessions(
        const Domain::OperationContext& context) noexcept
    {
        return execute<std::size_t>(context, [&]() {
            auto result = agentSessions_.pruneStale(context);
            if (!result) {
                return result;
            }
            if (result.value() >
                Domain::AgentSessionLimits::MaximumSessionQueryRows) {
                return Domain::Result<std::size_t>::failure(limitError(
                    "Dashboard session prune exceeded its bounded result."));
            }
            return result;
        });
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentSession>(context, [&]() {
            auto valid = validateCloseRequest(request);
            if (!valid) {
                return propagateFailure<Domain::AgentSession>(
                    std::move(valid));
            }
            auto closed = dataSource_.closeSession(request, context);
            if (!closed) {
                return closed;
            }
            valid = validateClosedSession(closed.value(), request);
            if (!valid) {
                return propagateFailure<Domain::AgentSession>(
                    std::move(valid));
            }
            return closed;
        });
    }

    void shutdown() noexcept
    {
        try {
            std::unique_lock lock{lifecycleMutex_};
            if (shutdownComplete_) {
                return;
            }
            if (!accepting_) {
                lifecycleChanged_.wait(
                    lock, [&]() noexcept { return shutdownComplete_; });
                return;
            }
            accepting_ = false;
            lifecycleChanged_.wait(
                lock, [&]() noexcept { return activeOperations_ == 0U; });
            shutdownComplete_ = true;
            lock.unlock();
            lifecycleChanged_.notify_all();
        } catch (...) {
            // This facade owns no injected dependency. A synchronization
            // failure must never trigger dependency shutdown as a fallback.
        }
    }

private:
    class Admission final {
    public:
        explicit Admission(Impl& owner) noexcept : owner_{&owner} {}
        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;
        Admission(Admission&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }
        Admission& operator=(Admission&& other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }
        ~Admission() noexcept { release(); }

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseOperation();
                owner_ = nullptr;
            }
        }

        Impl* owner_{};
    };

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept
    {
        try {
            if (context.isCancellationRequested()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The dashboard operational request was cancelled."));
            }
            if (context.isExpired(clock_.monotonicNow())) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The dashboard operational request deadline expired."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(internalError(
                "The dashboard operational context could not be validated."));
        }
    }

    [[nodiscard]] Domain::Result<Admission> admit(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context);
            if (!valid) {
                return propagateFailure<Admission>(std::move(valid));
            }
            std::lock_guard lock{lifecycleMutex_};
            valid = validateContext(context);
            if (!valid) {
                return propagateFailure<Admission>(std::move(valid));
            }
            if (!accepting_) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The dashboard operational service is shut down."));
            }
            if (activeOperations_ ==
                (std::numeric_limits<std::size_t>::max)()) {
                return Domain::Result<Admission>::failure(limitError(
                    "Dashboard operational admission counter overflowed."));
            }
            ++activeOperations_;
            return Domain::Result<Admission>::success(Admission{*this});
        } catch (...) {
            return Domain::Result<Admission>::failure(internalError(
                "The dashboard operational request could not be admitted."));
        }
    }

    void releaseOperation() noexcept
    {
        try {
            std::lock_guard lock{lifecycleMutex_};
            if (activeOperations_ != 0U) {
                --activeOperations_;
            }
            if (!accepting_ && activeOperations_ == 0U) {
                lifecycleChanged_.notify_all();
            }
        } catch (...) {
        }
    }

    template <typename T, typename Function>
    [[nodiscard]] Domain::Result<T> execute(
        const Domain::OperationContext& context,
        Function&& operation) noexcept
    {
        try {
            auto admitted = admit(context);
            if (!admitted) {
                return propagateFailure<T>(std::move(admitted));
            }
            [[maybe_unused]] auto admission = std::move(admitted).value();
            return std::forward<Function>(operation)();
        } catch (...) {
            return Domain::Result<T>::failure(internalError(
                "The dashboard operational application boundary failed "
                "internally."));
        }
    }

    Contracts::IAgentCatalog& agentCatalog_;
    Contracts::IAgentSessionService& agentSessions_;
    Contracts::IAuditRepository& auditRepository_;
    Contracts::IDoctorService& doctorService_;
    Contracts::IRuntimeDiagnostics& runtimeDiagnostics_;
    Contracts::IToolCatalog& toolCatalog_;
    Contracts::IClock& clock_;
    IDashboardOperationalDataSource& dataSource_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::size_t activeOperations_{};
    bool accepting_{true};
    bool shutdownComplete_{};
};

DashboardOperationalService::DashboardOperationalService(
    DashboardOperationalServiceDependencies dependencies)
    : implementation_{std::make_unique<Impl>(dependencies)}
{
}

DashboardOperationalService::~DashboardOperationalService() noexcept = default;

Domain::Result<Dashboard::DashboardStatusData>
DashboardOperationalService::status(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->status(context);
}

Domain::Result<Domain::DoctorReport> DashboardOperationalService::doctor(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->doctor(context);
}

Domain::Result<std::vector<Domain::AgentSpec>>
DashboardOperationalService::agents(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->agents(context);
}

Domain::Result<Dashboard::DashboardSessionListing>
DashboardOperationalService::sessions(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->sessions(context);
}

Domain::Result<std::vector<Domain::AuditEvent>>
DashboardOperationalService::audit(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->audit(context);
}

Domain::Result<std::vector<std::string>>
DashboardOperationalService::diagnosticLines(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->diagnosticLines(context);
}

Domain::Result<std::size_t> DashboardOperationalService::pruneSessions(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->pruneSessions(context);
}

Domain::Result<Domain::AgentSession>
DashboardOperationalService::closeSession(
    const Dashboard::DashboardSessionCloseRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->closeSession(request, context);
}

void DashboardOperationalService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
