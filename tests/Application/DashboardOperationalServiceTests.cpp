#include "ForgeConductor/Application/DashboardOperationalService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

using namespace std::chrono_literals;

std::size_t assertions{};

void require(const bool condition, const std::string_view message)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code)
{
    REQUIRE(!result.hasValue());
    REQUIRE(result.error().code == code);
}

class ClockFake final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{} + 1'700'000'000s;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return now_;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

private:
    Domain::MonotonicTimePoint now_{Domain::MonotonicTimePoint{} + 100s};
};

[[nodiscard]] Domain::OperationContext context(
    const ClockFake& clock,
    const std::string_view correlation = "dashboard-operational-test")
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("00000000-0000-4000-8000-000000000001"),
        clock.monotonicNow() + 30s,
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] bool sameContext(
    const std::optional<Domain::OperationContext>& actual,
    const Domain::OperationContext& expected) noexcept
{
    return actual && actual->operationId == expected.operationId &&
        actual->deadline == expected.deadline &&
        actual->correlationId == expected.correlationId &&
        actual->isCancellationRequested() ==
            expected.isCancellationRequested();
}

template <typename T>
[[nodiscard]] Domain::Result<T> unusedResult() noexcept
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::UnsupportedVersion,
        "The unused test operation was called."));
}

class AgentCatalogFake final : public Contracts::IAgentCatalog {
public:
    std::vector<Domain::AgentSpec> values;
    std::optional<Domain::Error> error;
    std::size_t allCalls{};
    std::optional<Domain::OperationContext> lastContext;

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> all(
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++allCalls;
            lastContext = operationContext;
            if (error) {
                return Domain::Result<std::vector<Domain::AgentSpec>>::failure(
                    *error);
            }
            return Domain::Result<std::vector<Domain::AgentSpec>>::success(
                values);
        } catch (...) {
            return unusedResult<std::vector<Domain::AgentSpec>>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSpec>> get(
        const Domain::AgentId&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<std::optional<Domain::AgentSpec>>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentSpec> recommend(
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentSpec>();
    }
};

class AgentSessionServiceFake final : public Contracts::IAgentSessionService {
public:
    std::size_t pruneValue{};
    std::optional<Domain::Error> pruneError;
    std::size_t pruneCalls{};
    std::size_t completeCalls{};
    std::size_t shutdownCalls{};
    std::optional<Domain::OperationContext> lastPruneContext;

    [[nodiscard]] Domain::Result<Domain::AgentSession> start(
        const Domain::AgentId&,
        const std::optional<Domain::ClientId>&,
        std::string_view,
        const std::optional<Domain::PathText>&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentSession>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> status(
        const Domain::SessionId&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentSession>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> complete(
        const Domain::SessionId&,
        std::string_view,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        ++completeCalls;
        return unusedResult<Domain::AgentSession>();
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneStale(
        const Domain::OperationContext& operationContext) noexcept override
    {
        ++pruneCalls;
        lastPruneContext = operationContext;
        if (pruneError) {
            return Domain::Result<std::size_t>::failure(*pruneError);
        }
        return Domain::Result<std::size_t>::success(pruneValue);
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStartOutcome> startRun(
        const Domain::AgentRunStartRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentRunStartOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStatusOutcome> runStatus(
        const Domain::AgentRunStatusRequest&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentRunStatusOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompleteOutcome> completeRun(
        const Domain::AgentRunCompleteRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentRunCompleteOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> attach(
        const Domain::AgentRunReattachRequest&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentRunReattachOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> rehydrate(
        const Domain::AgentRunRecoveryRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Domain::AgentRunRecoveryOutcome>();
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ActiveBinding>> binding(
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<std::optional<Domain::ActiveBinding>>();
    }

    [[nodiscard]] Domain::Result<bool> touchIfActive(
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<bool>();
    }

    void shutdown() noexcept override { ++shutdownCalls; }
};

class AuditRepositoryFake final : public Contracts::IAuditRepository {
public:
    std::vector<Domain::AuditEvent> values;
    std::optional<Domain::Error> error;
    std::size_t recentCalls{};
    std::size_t lastMaximumCount{};
    std::size_t closeCalls{};
    std::optional<Domain::OperationContext> lastContext;

    [[nodiscard]] Domain::Result<void> append(
        const Domain::AuditEvent&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::UnsupportedVersion,
            "The unused audit append was called."));
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> recent(
        const std::size_t maximumCount,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++recentCalls;
            lastMaximumCount = maximumCount;
            lastContext = operationContext;
            if (error) {
                return Domain::Result<std::vector<Domain::AuditEvent>>::failure(
                    *error);
            }
            return Domain::Result<std::vector<Domain::AuditEvent>>::success(
                values);
        } catch (...) {
            return unusedResult<std::vector<Domain::AuditEvent>>();
        }
    }

    void close() noexcept override { ++closeCalls; }
};

class DoctorServiceFake final : public Contracts::IDoctorService {
public:
    std::optional<Domain::DoctorReport> value;
    std::optional<Domain::Error> error;
    std::size_t runCalls{};
    std::size_t shutdownCalls{};
    std::optional<Domain::OperationContext> lastContext;

    [[nodiscard]] Domain::Result<Domain::DoctorReport> run(
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++runCalls;
            lastContext = operationContext;
            if (error) {
                return Domain::Result<Domain::DoctorReport>::failure(*error);
            }
            if (!value) {
                return unusedResult<Domain::DoctorReport>();
            }
            return Domain::Result<Domain::DoctorReport>::success(*value);
        } catch (...) {
            return unusedResult<Domain::DoctorReport>();
        }
    }

    void shutdown() noexcept override { ++shutdownCalls; }
};

class RuntimeDiagnosticsFake final : public Contracts::IRuntimeDiagnostics {
public:
    std::optional<Domain::RuntimeDiagnosticSnapshot> value;
    std::optional<Domain::Error> error;
    std::size_t snapshotCalls{};
    std::size_t shutdownCalls{};
    std::optional<Domain::OperationContext> lastContext;

    [[nodiscard]] Domain::Result<Contracts::RuntimeOwnershipLease> acquire(
        Contracts::RuntimeOwnerKind,
        const Domain::OperationContext&) noexcept override
    {
        return unusedResult<Contracts::RuntimeOwnershipLease>();
    }

    [[nodiscard]] Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++snapshotCalls;
            lastContext = operationContext;
            if (error) {
                return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::failure(
                    *error);
            }
            if (!value) {
                return unusedResult<Domain::RuntimeDiagnosticSnapshot>();
            }
            return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::success(
                *value);
        } catch (...) {
            return unusedResult<Domain::RuntimeDiagnosticSnapshot>();
        }
    }

    void shutdown() noexcept override { ++shutdownCalls; }
};

class ToolCatalogFake final : public Contracts::IToolCatalog {
public:
    std::vector<Domain::McpToolDescriptor> values;
    mutable std::size_t calls{};

    [[nodiscard]] std::span<const Domain::McpToolDescriptor> tools()
        const noexcept override
    {
        ++calls;
        return values;
    }
};

class OperationalDataSourceFake final
    : public Application::IDashboardOperationalDataSource {
public:
    Application::DashboardOperationalSourceSnapshot snapshotValue;
    std::optional<Domain::Error> snapshotError;
    std::vector<std::string> linesValue;
    std::optional<Domain::Error> linesError;
    std::optional<Domain::AgentSession> closeValue;
    std::optional<Domain::Error> closeError;
    bool echoCloseRequest{};

    std::size_t snapshotCalls{};
    std::size_t linesCalls{};
    std::size_t closeCalls{};
    std::size_t maximumOpenSessions{};
    std::size_t maximumRecentSessions{};
    std::size_t maximumPresenceRecords{};
    std::size_t maximumLines{};
    std::size_t maximumLineBytes{};
    std::size_t maximumLineAggregateBytes{};
    std::optional<Domain::OperationContext> lastContext;
    std::optional<Domain::SessionId> lastCloseId;
    std::string lastCloseSummary;

    [[nodiscard]] Domain::Result<
        Application::DashboardOperationalSourceSnapshot>
    snapshot(
        const std::size_t requestedOpenSessions,
        const std::size_t requestedRecentSessions,
        const std::size_t requestedPresenceRecords,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            {
                std::unique_lock lock{blockMutex_};
                ++snapshotCalls;
                maximumOpenSessions = requestedOpenSessions;
                maximumRecentSessions = requestedRecentSessions;
                maximumPresenceRecords = requestedPresenceRecords;
                lastContext = operationContext;
                if (blockNextSnapshot_) {
                    blockNextSnapshot_ = false;
                    snapshotBlocked_ = true;
                    blockChanged_.notify_all();
                    blockChanged_.wait(
                        lock, [&]() noexcept { return releaseSnapshot_; });
                }
            }
            if (snapshotError) {
                return Domain::Result<
                    Application::DashboardOperationalSourceSnapshot>::failure(
                    *snapshotError);
            }
            return Domain::Result<
                Application::DashboardOperationalSourceSnapshot>::success(
                snapshotValue);
        } catch (...) {
            return unusedResult<
                Application::DashboardOperationalSourceSnapshot>();
        }
    }

    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        const std::size_t requestedLines,
        const std::size_t requestedLineBytes,
        const std::size_t requestedAggregateBytes,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++linesCalls;
            maximumLines = requestedLines;
            maximumLineBytes = requestedLineBytes;
            maximumLineAggregateBytes = requestedAggregateBytes;
            lastContext = operationContext;
            if (linesError) {
                return Domain::Result<std::vector<std::string>>::failure(
                    *linesError);
            }
            return Domain::Result<std::vector<std::string>>::success(linesValue);
        } catch (...) {
            return unusedResult<std::vector<std::string>>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest& request,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++closeCalls;
            lastContext = operationContext;
            lastCloseId = request.sessionId();
            lastCloseSummary = request.summary();
            if (closeError) {
                return Domain::Result<Domain::AgentSession>::failure(*closeError);
            }
            if (!closeValue) {
                return unusedResult<Domain::AgentSession>();
            }
            auto value = *closeValue;
            if (echoCloseRequest) {
                value.id = request.sessionId();
                value.status = Domain::SessionStatus::Closed;
                value.summary = request.summary();
            }
            return Domain::Result<Domain::AgentSession>::success(
                std::move(value));
        } catch (...) {
            return unusedResult<Domain::AgentSession>();
        }
    }

    void blockOneSnapshot()
    {
        std::lock_guard lock{blockMutex_};
        blockNextSnapshot_ = true;
        snapshotBlocked_ = false;
        releaseSnapshot_ = false;
    }

    void waitUntilSnapshotBlocked()
    {
        std::unique_lock lock{blockMutex_};
        REQUIRE(blockChanged_.wait_for(
            lock, 2s, [&]() noexcept { return snapshotBlocked_; }));
    }

    void releaseBlockedSnapshot()
    {
        std::lock_guard lock{blockMutex_};
        releaseSnapshot_ = true;
        blockChanged_.notify_all();
    }

private:
    std::mutex blockMutex_;
    std::condition_variable blockChanged_;
    bool blockNextSnapshot_{};
    bool snapshotBlocked_{};
    bool releaseSnapshot_{};
};

[[nodiscard]] Domain::AgentSpec agent(
    const std::string_view id = "implement")
{
    return Domain::AgentSpec{
        parse<Domain::AgentId>(id),
        "Implement",
        "Build the selected coherent slice.",
        {"fs_read", "fs_write"},
        {"unsafe_tool"},
        {"Implementation requested"},
        {"Inspect dependencies"},
        {"Tests pass"},
        {"result"},
        {"review"},
        {"No feature loss"},
        "body excluded from dashboard",
        "builtin"};
}

[[nodiscard]] Domain::AgentSession session(
    const Domain::SessionStatus status = Domain::SessionStatus::Open,
    std::optional<std::string> summary = std::nullopt,
    const std::string_view id = "00000000-0000-4000-8000-000000000010")
{
    return Domain::AgentSession{
        parse<Domain::SessionId>(id),
        parse<Domain::AgentId>("implement"),
        parse<Domain::ClientId>("client-one"),
        status,
        std::move(summary),
        Domain::UtcTimePoint{} + 1'700'000'000s,
        Domain::UtcTimePoint{} + 1'700'000'001s};
}

[[nodiscard]] Dashboard::DashboardPresenceRecord presence()
{
    return Dashboard::DashboardPresenceRecord{
        "client-one:mcp",
        "lmstudio",
        42U,
        path("C:\\Work"),
        Domain::UtcTimePoint{} + 1'700'000'002s};
}

[[nodiscard]] Domain::AuditEvent auditEvent()
{
    return Domain::AuditEvent{
        Domain::UtcTimePoint{} + 1'700'000'003s,
        parse<Domain::ClientId>("client-one"),
        "fs_read",
        std::nullopt,
        "success",
        12ms,
        std::nullopt};
}

[[nodiscard]] Domain::RuntimeDiagnosticSnapshot runtimeSnapshot()
{
    return Domain::RuntimeDiagnosticSnapshot{
        Domain::UtcTimePoint{} + 1'700'000'004s,
        1U,
        2U,
        3U,
        4U,
        1U,
        Domain::ResourcePressureLevel::Nominal,
        5U,
        6U,
        7U,
        8U};
}

[[nodiscard]] Domain::DoctorReport doctorReport()
{
    return Domain::DoctorReport{
        true,
        "0.9.0",
        path("C:\\Forge"),
        {Domain::DoctorCheck{"home_layout", true, "ready", true}},
        Domain::TelemetryHealthReport{
            true,
            "running",
            "native-cpp",
            false,
            "integrated",
            "native",
            "winui3",
            false},
        true,
        path("C:\\Forge\\forge.exe")};
}

[[nodiscard]] Domain::McpToolDescriptor tool(
    std::string name = "fs_read")
{
    return Domain::McpToolDescriptor{
        Domain::ToolDescriptor{
            std::move(name),
            "Test descriptor",
            "filesystem",
            Domain::ToolEffect::Read,
            Domain::ToolAvailability::Available,
            true,
            false},
        R"({"type":"object"})"};
}

struct Fixture final {
    ClockFake clock;
    AgentCatalogFake catalog;
    AgentSessionServiceFake agentSessions;
    AuditRepositoryFake audit;
    DoctorServiceFake doctor;
    RuntimeDiagnosticsFake runtime;
    ToolCatalogFake tools;
    OperationalDataSourceFake source;
    std::unique_ptr<Application::DashboardOperationalService> service;

    Fixture()
    {
        catalog.values = {agent()};
        audit.values = {auditEvent()};
        doctor.value = doctorReport();
        runtime.value = runtimeSnapshot();
        tools.values = {tool()};
        source.snapshotValue.openSessions = {session()};
        source.snapshotValue.recentSessions = {
            session(
                Domain::SessionStatus::Closed,
                std::string{"done"},
                "00000000-0000-4000-8000-000000000011")};
        source.snapshotValue.presence = {presence()};
        source.linesValue = {R"({"event":"ready"})"};
        source.closeValue = session(
            Domain::SessionStatus::Closed,
            std::string{"Closed from dashboard"});
        service = std::make_unique<Application::DashboardOperationalService>(
            Application::DashboardOperationalServiceDependencies{
                catalog,
                agentSessions,
                audit,
                doctor,
                runtime,
                tools,
                clock,
                source});
    }
};

void testTypeAndAtomicSourceContract()
{
    static_assert(std::is_final_v<Application::DashboardOperationalService>);
    static_assert(std::is_base_of_v<
        Dashboard::IDashboardOperationalService,
        Application::DashboardOperationalService>);
    static_assert(!std::is_copy_constructible_v<
        Application::DashboardOperationalService>);
    static_assert(!std::is_move_constructible_v<
        Application::DashboardOperationalService>);
    static_assert(std::has_virtual_destructor_v<
        Application::IDashboardOperationalDataSource>);
    REQUIRE(Dashboard::DashboardApplicationLimits::MaximumRecentSessions == 40U);
    REQUIRE(Dashboard::DashboardApplicationLimits::MaximumAuditEvents == 80U);
    REQUIRE(Dashboard::DashboardApplicationLimits::MaximumDiagnosticLines == 100U);
}

void testStatusCompositionAndShortCircuiting()
{
    Fixture fixture;
    const auto operationContext = context(fixture.clock, "status-composition");
    auto result = fixture.service->status(operationContext);
    REQUIRE(result.hasValue());
    REQUIRE(result.value().agents.size() == 1U);
    REQUIRE(result.value().openSessions.size() == 1U);
    REQUIRE(result.value().presence.size() == 1U);
    REQUIRE(result.value().recentAudit.size() == 1U);
    REQUIRE(result.value().toolNames == std::vector<std::string>{"fs_read"});
    REQUIRE(
        result.value().runtimeDiagnostics.timestamp ==
        runtimeSnapshot().timestamp);
    REQUIRE(result.value().runtimeDiagnostics.ownedOperations == 1U);
    REQUIRE(result.value().runtimeDiagnostics.openDatabases == 8U);
    REQUIRE(fixture.catalog.allCalls == 1U);
    REQUIRE(fixture.source.snapshotCalls == 1U);
    REQUIRE(
        fixture.source.maximumOpenSessions ==
        Dashboard::DashboardApplicationLimits::MaximumOpenSessions);
    REQUIRE(
        fixture.source.maximumRecentSessions ==
        Dashboard::DashboardApplicationLimits::MaximumRecentSessions);
    REQUIRE(
        fixture.source.maximumPresenceRecords ==
        Dashboard::DashboardApplicationLimits::MaximumPresenceRecords);
    REQUIRE(
        fixture.audit.lastMaximumCount ==
        Dashboard::DashboardApplicationLimits::MaximumStatusAuditEvents);
    REQUIRE(fixture.tools.calls == 1U);
    REQUIRE(fixture.runtime.snapshotCalls == 1U);
    REQUIRE(fixture.doctor.runCalls == 0U);
    REQUIRE(fixture.agentSessions.pruneCalls == 0U);
    REQUIRE(fixture.agentSessions.completeCalls == 0U);
    REQUIRE(sameContext(fixture.catalog.lastContext, operationContext));
    REQUIRE(sameContext(fixture.source.lastContext, operationContext));
    REQUIRE(sameContext(fixture.audit.lastContext, operationContext));
    REQUIRE(sameContext(fixture.runtime.lastContext, operationContext));

    Fixture failed;
    failed.catalog.error = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy, "catalog busy", true);
    auto failure = failed.service->status(context(failed.clock));
    requireError(failure, Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(failed.source.snapshotCalls == 0U);
    REQUIRE(failed.audit.recentCalls == 0U);
    REQUIRE(failed.tools.calls == 0U);
    REQUIRE(failed.runtime.snapshotCalls == 0U);

    Fixture sourceFailed;
    sourceFailed.source.snapshotError = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy, "snapshot busy", true);
    failure = sourceFailed.service->status(context(sourceFailed.clock));
    requireError(failure, Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(sourceFailed.catalog.allCalls == 1U);
    REQUIRE(sourceFailed.audit.recentCalls == 0U);
    REQUIRE(sourceFailed.tools.calls == 0U);
    REQUIRE(sourceFailed.runtime.snapshotCalls == 0U);

    Fixture auditFailed;
    auditFailed.audit.error = Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, "audit failed");
    failure = auditFailed.service->status(context(auditFailed.clock));
    requireError(failure, Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(auditFailed.tools.calls == 0U);
    REQUIRE(auditFailed.runtime.snapshotCalls == 0U);

    Fixture runtimeFailed;
    runtimeFailed.runtime.error = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable, "runtime unavailable");
    failure = runtimeFailed.service->status(context(runtimeFailed.clock));
    requireError(failure, Domain::ErrorCodes::HostCapabilityUnavailable);
}

void testReadRoutesAndExactSourceLimits()
{
    Fixture fixture;
    const auto operationContext = context(fixture.clock, "read-routes");

    auto agents = fixture.service->agents(operationContext);
    REQUIRE(agents.hasValue());
    REQUIRE(agents.value().front().id.value() == "implement");

    auto sessions = fixture.service->sessions(operationContext);
    REQUIRE(sessions.hasValue());
    REQUIRE(sessions.value().open.size() == 1U);
    REQUIRE(sessions.value().recent.size() == 1U);
    REQUIRE(fixture.source.snapshotCalls == 1U);
    REQUIRE(
        fixture.source.maximumRecentSessions ==
        Dashboard::DashboardApplicationLimits::MaximumRecentSessions);

    auto audit = fixture.service->audit(operationContext);
    REQUIRE(audit.hasValue());
    REQUIRE(audit.value().front().tool == "fs_read");
    REQUIRE(
        fixture.audit.lastMaximumCount ==
        Dashboard::DashboardApplicationLimits::MaximumAuditEvents);

    auto lines = fixture.service->diagnosticLines(operationContext);
    REQUIRE(lines.hasValue());
    REQUIRE(lines.value() == fixture.source.linesValue);
    REQUIRE(
        fixture.source.maximumLines ==
        Dashboard::DashboardApplicationLimits::MaximumDiagnosticLines);
    REQUIRE(
        fixture.source.maximumLineBytes ==
        Dashboard::DashboardApplicationLimits::MaximumDiagnosticLineBytes);
    REQUIRE(
        fixture.source.maximumLineAggregateBytes ==
        Dashboard::DashboardApplicationLimits::MaximumDiagnosticTextBytes);

    auto doctor = fixture.service->doctor(operationContext);
    REQUIRE(doctor.hasValue());
    REQUIRE(doctor.value().checks.size() == 1U);
    REQUIRE(doctor.value().checks.front().name == "home_layout");
    REQUIRE(sameContext(fixture.doctor.lastContext, operationContext));

    fixture.agentSessions.pruneValue = 7U;
    auto pruned = fixture.service->pruneSessions(operationContext);
    REQUIRE(pruned.hasValue());
    REQUIRE(pruned.value() == 7U);
    REQUIRE(fixture.agentSessions.pruneCalls == 1U);
    REQUIRE(sameContext(
        fixture.agentSessions.lastPruneContext, operationContext));

    fixture.agentSessions.pruneError = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy, "prune busy", true);
    requireError(
        fixture.service->pruneSessions(operationContext),
        Domain::ErrorCodes::DatabaseBusy);
}

void testCollectionAndAggregateBounds()
{
    {
        Fixture fixture;
        fixture.catalog.values.assign(
            Dashboard::DashboardApplicationLimits::MaximumAgentSpecs,
            agent());
        REQUIRE(fixture.service->agents(context(fixture.clock)).hasValue());
        fixture.catalog.values.push_back(agent());
        requireError(
            fixture.service->agents(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        auto exact = agent();
        const auto fixedBytes = exact.id.value().size() +
            exact.displayName.size() + exact.source.size() +
            exact.tools.front().size() + exact.tools.back().size() +
            exact.toolsForbidden.front().size() + exact.whenToUse.front().size() +
            exact.firstMoves.front().size() + exact.doneDefinition.front().size() +
            exact.outputSchema.front().size() + exact.handoff.front().size() +
            exact.qualityBar.front().size();
        REQUIRE(
            fixedBytes <
            Dashboard::DashboardApplicationLimits::MaximumAgentTextBytes);
        exact.description.assign(
            Dashboard::DashboardApplicationLimits::MaximumAgentTextBytes -
                fixedBytes,
            'a');
        fixture.catalog.values = {exact};
        REQUIRE(fixture.service->agents(context(fixture.clock)).hasValue());
        fixture.catalog.values.front().description.push_back('a');
        requireError(
            fixture.service->agents(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.source.snapshotValue.openSessions.assign(
            Dashboard::DashboardApplicationLimits::MaximumOpenSessions,
            session());
        fixture.source.snapshotValue.recentSessions.clear();
        REQUIRE(fixture.service->sessions(context(fixture.clock)).hasValue());
        fixture.source.snapshotValue.openSessions.push_back(session());
        requireError(
            fixture.service->sessions(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.source.snapshotValue.recentSessions.assign(
            Dashboard::DashboardApplicationLimits::MaximumRecentSessions,
            session(Domain::SessionStatus::Closed, std::string{"done"}));
        REQUIRE(fixture.service->sessions(context(fixture.clock)).hasValue());
        fixture.source.snapshotValue.recentSessions.push_back(
            session(Domain::SessionStatus::Closed, std::string{"done"}));
        requireError(
            fixture.service->sessions(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.source.snapshotValue.presence.assign(
            Dashboard::DashboardApplicationLimits::MaximumPresenceRecords,
            presence());
        REQUIRE(fixture.service->status(context(fixture.clock)).hasValue());
        fixture.source.snapshotValue.presence.push_back(presence());
        requireError(
            fixture.service->status(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.audit.values.assign(
            Dashboard::DashboardApplicationLimits::MaximumAuditEvents,
            auditEvent());
        REQUIRE(fixture.service->audit(context(fixture.clock)).hasValue());
        fixture.audit.values.push_back(auditEvent());
        requireError(
            fixture.service->audit(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.doctor.value->checks.assign(
            Dashboard::DashboardApplicationLimits::MaximumDoctorChecks,
            Domain::DoctorCheck{"check", true, "ready", true});
        REQUIRE(fixture.service->doctor(context(fixture.clock)).hasValue());
        fixture.doctor.value->checks.push_back(
            Domain::DoctorCheck{"overflow", true, "ready", true});
        requireError(
            fixture.service->doctor(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.source.linesValue.assign(
            Dashboard::DashboardApplicationLimits::MaximumDiagnosticLines,
            "line");
        REQUIRE(
            fixture.service->diagnosticLines(context(fixture.clock)).hasValue());
        fixture.source.linesValue.push_back("overflow");
        requireError(
            fixture.service->diagnosticLines(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.source.linesValue.assign(
            Dashboard::DashboardApplicationLimits::MaximumDiagnosticTextBytes /
                Dashboard::DashboardApplicationLimits::
                    MaximumDiagnosticLineBytes,
            std::string(
                Dashboard::DashboardApplicationLimits::
                    MaximumDiagnosticLineBytes,
                'x'));
        REQUIRE(
            fixture.service->diagnosticLines(context(fixture.clock)).hasValue());
        fixture.source.linesValue.push_back("x");
        requireError(
            fixture.service->diagnosticLines(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.tools.values.clear();
        fixture.tools.values.reserve(
            Dashboard::DashboardApplicationLimits::MaximumToolNames);
        for (std::size_t index{};
             index < Dashboard::DashboardApplicationLimits::MaximumToolNames;
             ++index) {
            auto name = "tool_" + std::to_string(index);
            name.append(
                Dashboard::DashboardApplicationLimits::MaximumToolNameBytes -
                    name.size(),
                'x');
            fixture.tools.values.push_back(tool(std::move(name)));
        }
        REQUIRE(fixture.service->status(context(fixture.clock)).hasValue());
        fixture.tools.values.push_back(tool("tool_overflow"));
        requireError(
            fixture.service->status(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.agentSessions.pruneValue =
            Domain::AgentSessionLimits::MaximumSessionQueryRows;
        const auto exact = fixture.service->pruneSessions(
            context(fixture.clock));
        REQUIRE(exact.hasValue());
        REQUIRE(
            exact.value() ==
            Domain::AgentSessionLimits::MaximumSessionQueryRows);
        fixture.agentSessions.pruneValue =
            Domain::AgentSessionLimits::MaximumSessionQueryRows + 1U;
        requireError(
            fixture.service->pruneSessions(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
}

void testInvalidProducerOutputsAreRejected()
{
    {
        Fixture fixture;
        fixture.source.snapshotValue.openSessions.front().status =
            Domain::SessionStatus::Closed;
        requireError(
            fixture.service->sessions(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.source.snapshotValue.openSessions.front().updatedAt =
            fixture.source.snapshotValue.openSessions.front().createdAt - 1s;
        requireError(
            fixture.service->sessions(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.source.snapshotValue.presence.front().processId = 0U;
        requireError(
            fixture.service->status(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.audit.values.front().duration = -1ms;
        requireError(
            fixture.service->audit(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.runtime.value->pressure =
            static_cast<Domain::ResourcePressureLevel>(255U);
        requireError(
            fixture.service->status(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.source.linesValue = {
            std::string{"bad"} + static_cast<char>(0xc0U)};
        requireError(
            fixture.service->diagnosticLines(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.source.linesValue = {
            std::string(
                Dashboard::DashboardApplicationLimits::
                    MaximumDiagnosticLineBytes + 1U,
                'x')};
        requireError(
            fixture.service->diagnosticLines(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
    {
        Fixture fixture;
        fixture.tools.values.push_back(tool());
        requireError(
            fixture.service->status(context(fixture.clock)),
            Domain::ErrorCodes::IntegrityFailure);
    }
    {
        Fixture fixture;
        fixture.doctor.value->version.clear();
        requireError(
            fixture.service->doctor(context(fixture.clock)),
            Domain::ErrorCodes::LimitExceeded);
    }
}

void testAdministrativeClosePreservesReleasedSemantics()
{
    Fixture fixture;
    fixture.source.echoCloseRequest = true;
    const Dashboard::DashboardSessionCloseRequest request{
        parse<Domain::SessionId>("00000000-0000-4000-8000-000000000099"),
        "Operator supplied close summary"};
    const auto operationContext = context(fixture.clock, "admin-close");
    auto result = fixture.service->closeSession(request, operationContext);
    REQUIRE(result.hasValue());
    REQUIRE(result.value().id == request.sessionId());
    REQUIRE(result.value().status == Domain::SessionStatus::Closed);
    REQUIRE(result.value().summary == request.summary());
    REQUIRE(fixture.source.closeCalls == 1U);
    REQUIRE(fixture.source.lastCloseId == request.sessionId());
    REQUIRE(fixture.source.lastCloseSummary == request.summary());
    REQUIRE(sameContext(fixture.source.lastContext, operationContext));
    REQUIRE(fixture.agentSessions.completeCalls == 0U);

    Fixture mismatch;
    mismatch.source.echoCloseRequest = false;
    mismatch.source.closeValue = session(
        Domain::SessionStatus::Closed,
        std::string{"rewritten completion report"},
        "00000000-0000-4000-8000-000000000099");
    requireError(
        mismatch.service->closeSession(request, context(mismatch.clock)),
        Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(mismatch.agentSessions.completeCalls == 0U);

    Fixture invalid;
    const Dashboard::DashboardSessionCloseRequest oversized{
        request.sessionId(),
        std::string(
            Domain::AgentSessionLimits::MaximumSummaryUnits + 1U,
            'x')};
    requireError(
        invalid.service->closeSession(oversized, context(invalid.clock)),
        Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(invalid.source.closeCalls == 0U);

    Fixture failed;
    failed.source.closeError = Domain::makeError(
        Domain::ErrorCodes::SessionNotFound, "missing");
    requireError(
        failed.service->closeSession(request, context(failed.clock)),
        Domain::ErrorCodes::SessionNotFound);
    REQUIRE(failed.agentSessions.completeCalls == 0U);
}

void testContextAndIndependentShutdownLifecycle()
{
    {
        Fixture fixture;
        std::stop_source cancellation;
        cancellation.request_stop();
        auto cancelled = context(fixture.clock, "cancelled");
        cancelled.cancellation = cancellation.get_token();
        requireError(
            fixture.service->agents(cancelled),
            Domain::ErrorCodes::Cancelled);
        REQUIRE(fixture.catalog.allCalls == 0U);

        auto expired = context(fixture.clock, "expired");
        expired.deadline = fixture.clock.monotonicNow();
        requireError(
            fixture.service->status(expired),
            Domain::ErrorCodes::DeadlineExceeded);
        REQUIRE(fixture.source.snapshotCalls == 0U);
    }
    {
        Fixture fixture;
        fixture.service->shutdown();
        fixture.service->shutdown();
        requireError(
            fixture.service->status(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.service->doctor(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.service->agents(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.service->sessions(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.service->audit(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.service->diagnosticLines(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.service->pruneSessions(context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        const Dashboard::DashboardSessionCloseRequest request{
            parse<Domain::SessionId>(
                "00000000-0000-4000-8000-000000000099"),
            "closed"};
        requireError(
            fixture.service->closeSession(request, context(fixture.clock)),
            Domain::ErrorCodes::Cancelled);
        REQUIRE(fixture.catalog.allCalls == 0U);
        REQUIRE(fixture.agentSessions.shutdownCalls == 0U);
        REQUIRE(fixture.audit.closeCalls == 0U);
        REQUIRE(fixture.doctor.shutdownCalls == 0U);
        REQUIRE(fixture.runtime.shutdownCalls == 0U);
    }
    {
        Fixture fixture;
        fixture.source.blockOneSnapshot();
        std::optional<Domain::Result<Dashboard::DashboardSessionListing>>
            operationResult;
        std::atomic<bool> shutdownReturned{};
        std::thread operation{[&]() {
            operationResult.emplace(
                fixture.service->sessions(context(fixture.clock, "blocked")));
        }};
        fixture.source.waitUntilSnapshotBlocked();
        std::thread shutdown{[&]() {
            fixture.service->shutdown();
            shutdownReturned.store(true, std::memory_order_release);
        }};
        std::this_thread::sleep_for(20ms);
        REQUIRE(!shutdownReturned.load(std::memory_order_acquire));
        requireError(
            fixture.service->agents(context(fixture.clock, "during-shutdown")),
            Domain::ErrorCodes::Cancelled);
        fixture.source.releaseBlockedSnapshot();
        operation.join();
        shutdown.join();
        REQUIRE(operationResult.has_value());
        REQUIRE(operationResult->hasValue());
        REQUIRE(shutdownReturned.load(std::memory_order_acquire));
        REQUIRE(fixture.agentSessions.shutdownCalls == 0U);
        REQUIRE(fixture.audit.closeCalls == 0U);
        REQUIRE(fixture.doctor.shutdownCalls == 0U);
        REQUIRE(fixture.runtime.shutdownCalls == 0U);
    }
    {
        Fixture fixture;
        fixture.service.reset();
        REQUIRE(fixture.agentSessions.shutdownCalls == 0U);
        REQUIRE(fixture.audit.closeCalls == 0U);
        REQUIRE(fixture.doctor.shutdownCalls == 0U);
        REQUIRE(fixture.runtime.shutdownCalls == 0U);
    }
}

} // namespace

int main()
{
    try {
        testTypeAndAtomicSourceContract();
        testStatusCompositionAndShortCircuiting();
        testReadRoutesAndExactSourceLimits();
        testCollectionAndAggregateBounds();
        testInvalidProducerOutputsAreRejected();
        testAdministrativeClosePreservesReleasedSemantics();
        testContextAndIndependentShutdownLifecycle();
        std::cout << "Dashboard operational service tests passed: "
                  << assertions << " assertions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard operational service test failed after "
                  << assertions << " assertions: " << error.what() << '\n';
        return 1;
    }
}
