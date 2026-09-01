#include "ForgeConductor/Application/DashboardConnectionApplication.h"

#include "ForgeConductor/Dashboard/DashboardRequestPlanner.h"
#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"
#include "ForgeConductor/Dashboard/DashboardSessionCloseRequestDecoder.h"
#include "ForgeConductor/Dashboard/DashboardStaticAssetStore.h"
#include "ForgeConductor/Domain/Error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view SessionUuid{
    "12345678-1234-4234-8234-123456789abc"};

std::size_t assertions{};

[[noreturn]] void fail(
    const std::string_view expression,
    const std::size_t line)
{
    throw std::runtime_error{
        "requirement failed at line " + std::to_string(line) + ": " +
        std::string{expression}};
}

void require(
    const bool condition,
    const std::string_view expression,
    const std::size_t line)
{
    ++assertions;
    if (!condition) fail(expression, line);
}

#define REQUIRE(condition) \
    require(static_cast<bool>(condition), #condition, __LINE__)

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::UtcTimePoint utc(
    const std::int64_t milliseconds = 1'704'164'645'678LL)
{
    return Domain::UtcTimePoint{std::chrono::milliseconds{milliseconds}};
}

[[nodiscard]] Domain::SessionId sessionId()
{
    return take(Domain::SessionId::parse(SessionUuid));
}

[[nodiscard]] Domain::AgentId agentId()
{
    return take(Domain::AgentId::parse("dashboard-agent"));
}

[[nodiscard]] Domain::Sha256Digest bearerToken()
{
    return take(Domain::Sha256Digest::parse(std::string(64U, 'a')));
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view operation =
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
    const std::string_view correlation = "dashboard-correlation")
{
    return Domain::OperationContext{
        take(Domain::OperationId::parse(operation)),
        Domain::MonotonicTimePoint{} + 5min,
        {},
        take(Domain::CorrelationId::parse(correlation))};
}

[[nodiscard]] bool sameContext(
    const Domain::OperationContext& left,
    const Domain::OperationContext& right) noexcept
{
    return left.operationId == right.operationId &&
        left.deadline == right.deadline &&
        left.cancellation == right.cancellation &&
        left.correlationId == right.correlationId;
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<std::byte>(character));
    }
    return result;
}

[[nodiscard]] std::string text(const std::span<const std::byte> value)
{
    if (value.empty()) return {};
    return std::string{
        reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::string wireText(
    const Dashboard::DashboardPreparedExchange& exchange)
{
    REQUIRE(exchange.kind() == Dashboard::DashboardPreparedExchange::Kind::Complete);
    REQUIRE(exchange.completeExchange() != nullptr);
    return text(exchange.completeExchange()->encodedResponse().bytes());
}

[[nodiscard]] std::string wireBody(
    const Dashboard::DashboardPreparedExchange& exchange)
{
    const auto wire = wireText(exchange);
    const auto separator = wire.find("\r\n\r\n");
    REQUIRE(separator != std::string::npos);
    return wire.substr(separator + 4U);
}

[[nodiscard]] Json wireJson(
    const Dashboard::DashboardPreparedExchange& exchange)
{
    return Json::parse(wireBody(exchange));
}

[[nodiscard]] std::uint16_t wireStatus(
    const Dashboard::DashboardPreparedExchange& exchange)
{
    const auto wire = wireText(exchange);
    REQUIRE(wire.starts_with("HTTP/1.1 "));
    return static_cast<std::uint16_t>(std::stoul(wire.substr(9U, 3U)));
}

[[nodiscard]] std::size_t contentLength(const std::string_view wire)
{
    constexpr std::string_view Name{"Content-Length: "};
    const auto begin = wire.find(Name);
    REQUIRE(begin != std::string_view::npos);
    const auto valueBegin = begin + Name.size();
    const auto end = wire.find("\r\n", valueBegin);
    REQUIRE(end != std::string_view::npos);
    return std::stoull(std::string{wire.substr(valueBegin, end - valueBegin)});
}

[[nodiscard]] std::string bearer()
{
    return "Bearer " + std::string(64U, 'a');
}

[[nodiscard]] std::vector<Dashboard::DashboardHttpHeader> headers(
    const bool authorized,
    const bool mutation = false)
{
    std::vector<Dashboard::DashboardHttpHeader> result{
        {"host", "127.0.0.1:47820"}};
    if (authorized) result.push_back({"authorization", bearer()});
    if (mutation) {
        result.push_back({"content-type", "application/json"});
        result.push_back({"origin", "http://127.0.0.1:47820"});
        result.push_back({"sec-fetch-site", "same-origin"});
    }
    return result;
}

[[nodiscard]] Dashboard::DashboardHttpRequest request(
    std::string method,
    std::string target,
    const bool authorized = true,
    const bool mutation = false,
    std::string body = {})
{
    return Dashboard::DashboardHttpRequest{
        std::move(method),
        std::move(target),
        headers(authorized, mutation),
        bytes(body)};
}

[[nodiscard]] Dashboard::DashboardRequestPolicy policy()
{
    return take(Dashboard::DashboardRequestPolicy::create(
        "127.0.0.1", 47'820U, bearerToken()));
}

[[nodiscard]] Domain::ResourceBudgets budgets()
{
    auto result = Domain::budgetsForProfile(
        Domain::ResourceProfile::Standard16GiB);
    result.telemetrySampleHz = 2.0;
    return result;
}

[[nodiscard]] Domain::RuntimeDiagnosticSnapshot runtimeDiagnostics()
{
    return Domain::RuntimeDiagnosticSnapshot{
        utc(), 1U, 2U, 3U, 4U, 0U,
        Domain::ResourcePressureLevel::Nominal,
        5U, 0U, 0U, 1U};
}

[[nodiscard]] Domain::TelemetryHealthReport telemetryReport()
{
    return Domain::TelemetryHealthReport{
        true,
        "Forge Conductor telemetry",
        "windows-native",
        false,
        "embedded",
        "Windows SDK collectors",
        "WinUI dashboard",
        false};
}

[[nodiscard]] Dashboard::DashboardTelemetryHealth telemetryHealth()
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

[[nodiscard]] Domain::TelemetrySnapshot telemetrySnapshot()
{
    Domain::CpuMetrics cpu{};
    cpu.logicalProcessorCount = Domain::makeAvailableTelemetryMetric<std::uint32_t>(
        1U, utc(), "test.fixture");
    cpu.physicalCoreCount = Domain::makeAvailableTelemetryMetric<std::uint32_t>(
        1U, utc(), "test.fixture");
    cpu.brand = Domain::makeAvailableTelemetryMetric(
        std::string{"Test CPU"}, utc(), "test.fixture");
    Domain::SystemMetrics system{
        utc(),
        "forge-host",
        "windows",
        "x64",
        std::move(cpu),
        Domain::RamMetrics{},
        {},
        Domain::DiskIoMetrics{},
        {},
        {},
        Domain::PowerMetrics{}};
    Domain::ForgeSnapshot forge{
        utc(),
        path("C:\\Forge"),
        "windows-native",
        0U,
        0U,
        {},
        {},
        0U,
        Domain::TelemetryHealth::Ok};
    return Domain::TelemetrySnapshot{
        std::move(system),
        std::move(forge),
        utc() + 1s,
        {},
        "windows-native"};
}

[[nodiscard]] Domain::AgentSpec agent()
{
    return Domain::AgentSpec{
        agentId(),
        "Dashboard agent",
        "Exercises the operational route.",
        {"project_memory.status"},
        {},
        {"Use for tests."},
        {"Inspect."},
        {"Return a bounded result."},
        {"result"},
        {},
        {"Be deterministic."},
        "body omitted from dashboard JSON",
        "builtin"};
}

[[nodiscard]] Domain::AgentSession session(
    const Domain::SessionStatus status = Domain::SessionStatus::Running)
{
    return Domain::AgentSession{
        sessionId(),
        agentId(),
        take(Domain::ClientId::parse("dashboard-client")),
        status,
        std::string{"test session"},
        utc(),
        utc() + 1s};
}

[[nodiscard]] Domain::AuditEvent auditEvent()
{
    return Domain::AuditEvent{
        utc(),
        take(Domain::ClientId::parse("dashboard-client")),
        "project_memory.status",
        bearerToken(),
        "success",
        15ms,
        std::nullopt};
}

[[nodiscard]] Dashboard::DashboardStatusData statusData()
{
    return Dashboard::DashboardStatusData{
        {agent()},
        {session()},
        {Dashboard::DashboardPresenceRecord{
            "dashboard-client",
            "codex",
            42U,
            path("C:\\Forge\\Project"),
            utc()}},
        {auditEvent()},
        {"project_memory.status"},
        runtimeDiagnostics()};
}

[[nodiscard]] Domain::DoctorReport doctorReport()
{
    return Domain::DoctorReport{
        true,
        "0.9.0-alpha",
        path("C:\\Forge"),
        {Domain::DoctorCheck{"database", true, "ready", true}},
        telemetryReport(),
        true,
        path("C:\\Forge\\forge-conductor.exe")};
}

[[nodiscard]] Domain::ManagerSettings managerSettings()
{
    Domain::ManagerSettings result;
    result.dashboardHost = "127.0.0.1";
    result.dashboardPort = 47'820U;
    result.dashboardRefreshInterval = 8s;
    result.autoRestart = true;
    result.watchdogInterval = 3s;
    result.openBrowserOnStart = false;
    result.sessionIdleTtl = 14'400s;
    result.shellTimeout = 30s;
    result.logLevel = Domain::LogLevel::Info;
    return result;
}

[[nodiscard]] Domain::ManagerStatus managerStatus()
{
    const auto settings = managerSettings();
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        77U,
        utc(),
        120s,
        1U,
        std::nullopt,
        settings.autoRestart,
        settings.watchdogInterval,
        settings.openBrowserOnStart,
        settings.dashboardHost,
        settings.dashboardPort,
        settings.dashboardRefreshInterval,
        path("C:\\Forge"),
        "0.9.0-alpha"};
}

[[nodiscard]] Domain::ManagerSettingsUpdateOutcome managerUpdateOutcome()
{
    return Domain::ManagerSettingsUpdateOutcome{
        managerSettings(), true, false, managerStatus()};
}

[[nodiscard]] Dashboard::DashboardApplicationIdentity identity()
{
    return Dashboard::DashboardApplicationIdentity{
        "Forge Conductor",
        "0.9.0-alpha",
        "windows-native",
        path("C:\\Forge"),
        path("C:\\Forge\\forge.sqlite3"),
        123U};
}

[[nodiscard]] Domain::Error dependencyError(
    const std::string_view code)
{
    return Domain::makeError(
        code,
        "private dependency detail token=secret",
        true,
        "private-evidence");
}

struct SubscriptionState final {
    std::size_t closes{};
    std::size_t destructions{};
};

class Subscription final : public Dashboard::IDashboardSseSubscription {
public:
    Subscription(
        std::shared_ptr<SubscriptionState> state,
        const double deliveryHz) noexcept
        : state_{std::move(state)}, deliveryHz_{deliveryHz}
    {
    }

    ~Subscription() noexcept override { ++state_->destructions; }

    [[nodiscard]] double deliveryHz() const noexcept override
    {
        return deliveryHz_;
    }

    void attachReadySink(
        std::weak_ptr<Dashboard::IDashboardSseReadySink>) noexcept override
    {
    }

    [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame takeLatest()
        noexcept override
    {
        return {};
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return 0U;
    }

    void close() noexcept override
    {
        if (!closed_) {
            closed_ = true;
            ++state_->closes;
        }
    }

private:
    const std::shared_ptr<SubscriptionState> state_;
    const double deliveryHz_{};
    bool closed_{};
};

class AssetStoreFake final : public Dashboard::IDashboardAssetStore {
public:
    explicit AssetStoreFake(
        const Dashboard::DashboardStaticAssetStore& backing) noexcept
        : backing_{backing}
    {
    }

    std::optional<Domain::Error> failure;
    bool returnNull{};
    mutable std::size_t staticCalls{};
    mutable std::size_t shellCalls{};

    [[nodiscard]] Domain::Result<Dashboard::DashboardStaticAssetHandle>
    findStaticAsset(
        const Dashboard::DashboardStaticResourcePath& resource) const noexcept
        override
    {
        ++staticCalls;
        if (failure) {
            return Domain::Result<Dashboard::DashboardStaticAssetHandle>::failure(
                *failure);
        }
        if (returnNull) {
            return Domain::Result<Dashboard::DashboardStaticAssetHandle>::success(
                {});
        }
        return backing_.findStaticAsset(resource);
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardStaticAssetHandle>
    findShellAsset(const Dashboard::DashboardShellAssetId id) const noexcept
        override
    {
        ++shellCalls;
        if (failure) {
            return Domain::Result<Dashboard::DashboardStaticAssetHandle>::failure(
                *failure);
        }
        if (returnNull) {
            return Domain::Result<Dashboard::DashboardStaticAssetHandle>::success(
                {});
        }
        return backing_.findShellAsset(id);
    }

private:
    const Dashboard::DashboardStaticAssetStore& backing_;
};

class TelemetrySourceFake final : public Dashboard::IDashboardTelemetrySource {
public:
    Dashboard::DashboardTelemetryHealth healthValue{telemetryHealth()};
    std::optional<Domain::Error> healthFailure;
    std::optional<Domain::Error> latestFailure;
    std::optional<Domain::Error> subscribeFailure;
    bool returnNullSnapshot{};
    std::optional<double> subscriptionDeliveryHz;
    std::size_t healthCalls{};
    std::size_t latestCalls{};
    std::size_t subscribeCalls{};
    std::optional<Domain::OperationContext> lastHealthContext;
    std::optional<Domain::OperationContext> lastLatestContext;
    std::optional<Domain::OperationContext> lastSubscribeContext;
    std::optional<Dashboard::DashboardStreamRateSelection> lastRate;
    std::shared_ptr<SubscriptionState> subscriptionState{
        std::make_shared<SubscriptionState>()};

    [[nodiscard]] Domain::Result<Dashboard::DashboardTelemetryHealth> health(
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            ++healthCalls;
            lastHealthContext = operation;
            if (healthFailure) {
                return Domain::Result<Dashboard::DashboardTelemetryHealth>::failure(
                    *healthFailure);
            }
            return Domain::Result<Dashboard::DashboardTelemetryHealth>::success(
                healthValue);
        } catch (...) {
            return Domain::Result<Dashboard::DashboardTelemetryHealth>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardTelemetryObservation>
    latest(const Domain::OperationContext& operation) noexcept override
    {
        try {
            ++latestCalls;
            lastLatestContext = operation;
            if (latestFailure) {
                return Domain::Result<Dashboard::DashboardTelemetryObservation>::failure(
                    *latestFailure);
            }
            return Domain::Result<Dashboard::DashboardTelemetryObservation>::success(
                Dashboard::DashboardTelemetryObservation{
                    returnNullSnapshot
                        ? std::shared_ptr<const Domain::TelemetrySnapshot>{}
                        : std::make_shared<const Domain::TelemetrySnapshot>(
                              telemetrySnapshot()),
                    1.75});
        } catch (...) {
            return Domain::Result<Dashboard::DashboardTelemetryObservation>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }

    [[nodiscard]] Domain::Result<
        std::unique_ptr<Dashboard::IDashboardSseSubscription>>
    subscribe(
        const Dashboard::DashboardStreamRateSelection& rate,
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            ++subscribeCalls;
            lastRate = rate;
            lastSubscribeContext = operation;
            if (subscribeFailure) {
                return Domain::Result<std::unique_ptr<
                    Dashboard::IDashboardSseSubscription>>::failure(
                        *subscribeFailure);
            }
            std::unique_ptr<Dashboard::IDashboardSseSubscription> subscription =
                std::make_unique<Subscription>(
                    subscriptionState,
                    subscriptionDeliveryHz.value_or(rate.deliveryHz));
            return Domain::Result<std::unique_ptr<
                Dashboard::IDashboardSseSubscription>>::success(
                    std::move(subscription));
        } catch (...) {
            return Domain::Result<std::unique_ptr<
                Dashboard::IDashboardSseSubscription>>::failure(
                    dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }

    void shutdown() noexcept override {}
};

class OperationalServiceFake final
    : public Dashboard::IDashboardOperationalService {
public:
    enum class Operation : std::size_t {
        Status,
        Doctor,
        Agents,
        Sessions,
        Audit,
        Diagnostics,
        Prune,
        Close,
        Count,
    };

    std::array<std::optional<Domain::Error>,
        static_cast<std::size_t>(Operation::Count)> failures{};
    std::array<std::size_t,
        static_cast<std::size_t>(Operation::Count)> calls{};
    std::array<std::optional<Domain::OperationContext>,
        static_cast<std::size_t>(Operation::Count)> contexts{};
    std::optional<std::string> closedSessionId;
    std::optional<std::string> closedSummary;

    void setFailure(const Operation operation, const Domain::Error& error)
    {
        failures[index(operation)] = error;
    }

    [[nodiscard]] std::size_t callCount(const Operation operation) const noexcept
    {
        return calls[index(operation)];
    }

    [[nodiscard]] const std::optional<Domain::OperationContext>& lastContext(
        const Operation operation) const noexcept
    {
        return contexts[index(operation)];
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardStatusData> status(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(Operation::Status, operation, statusData());
    }

    [[nodiscard]] Domain::Result<Domain::DoctorReport> doctor(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(Operation::Doctor, operation, doctorReport());
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> agents(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(
            Operation::Agents,
            operation,
            std::vector<Domain::AgentSpec>{agent()});
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardSessionListing> sessions(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(
            Operation::Sessions,
            operation,
            Dashboard::DashboardSessionListing{
                {session()}, {session(Domain::SessionStatus::Closed)}});
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> audit(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(
            Operation::Audit,
            operation,
            std::vector<Domain::AuditEvent>{auditEvent()});
    }

    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(
            Operation::Diagnostics,
            operation,
            std::vector<std::string>{"diagnostic line"});
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneSessions(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(Operation::Prune, operation, std::size_t{3U});
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest& closeRequest,
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            closedSessionId = closeRequest.sessionId().value();
            closedSummary = closeRequest.summary();
            auto closed = session(Domain::SessionStatus::Closed);
            closed.summary = closeRequest.summary();
            return finish(Operation::Close, operation, std::move(closed));
        } catch (...) {
            return Domain::Result<Domain::AgentSession>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }

    void shutdown() noexcept override {}

private:
    [[nodiscard]] static constexpr std::size_t index(
        const Operation operation) noexcept
    {
        return static_cast<std::size_t>(operation);
    }

    template <typename Value>
    [[nodiscard]] Domain::Result<Value> finish(
        const Operation which,
        const Domain::OperationContext& operation,
        Value value) noexcept
    {
        try {
            ++calls[index(which)];
            contexts[index(which)] = operation;
            if (failures[index(which)]) {
                return Domain::Result<Value>::failure(*failures[index(which)]);
            }
            return Domain::Result<Value>::success(std::move(value));
        } catch (...) {
            return Domain::Result<Value>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }
};

class ManagerClientFake final : public Contracts::IManagerClient {
public:
    enum class Operation : std::size_t {
        Status,
        Settings,
        Control,
        UpdateSettings,
        Restart,
        Shutdown,
        Count,
    };

    std::array<std::optional<Domain::Error>,
        static_cast<std::size_t>(Operation::Count)> failures{};
    std::array<std::size_t,
        static_cast<std::size_t>(Operation::Count)> calls{};
    std::array<std::optional<Domain::OperationContext>,
        static_cast<std::size_t>(Operation::Count)> contexts{};
    std::optional<Domain::ManagerControlAction> lastControl;
    std::optional<Domain::ManagerSettingsPatch> lastPatch;
    std::optional<bool> lastApply;
    Domain::ManagerStatus statusValue{managerStatus()};
    Domain::ManagerSettings settingsValue{managerSettings()};
    Domain::ManagerSettingsUpdateOutcome updateValue{managerUpdateOutcome()};

    void setFailure(const Operation operation, const Domain::Error& error)
    {
        failures[index(operation)] = error;
    }

    [[nodiscard]] std::size_t callCount(const Operation operation) const noexcept
    {
        return calls[index(operation)];
    }

    [[nodiscard]] const std::optional<Domain::OperationContext>& lastContext(
        const Operation operation) const noexcept
    {
        return contexts[index(operation)];
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(Operation::Status, operation, statusValue);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& operation) noexcept override
    {
        return finish(Operation::Settings, operation, settingsValue);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& controlRequest,
        const Domain::OperationContext& operation) noexcept override
    {
        lastControl = controlRequest.action;
        return finish(Operation::Control, operation, statusValue);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patchValue,
        const bool applyImmediately,
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            lastPatch = patchValue;
            lastApply = applyImmediately;
        } catch (...) {
            return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
        return finish(
            Operation::UpdateSettings,
            operation,
            updateValue);
    }

    [[nodiscard]] Domain::Result<void> requestShutdown(
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            ++calls[index(Operation::Shutdown)];
            contexts[index(Operation::Shutdown)] = operation;
            if (failures[index(Operation::Shutdown)]) {
                return Domain::Result<void>::failure(
                    *failures[index(Operation::Shutdown)]);
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }

    [[nodiscard]] Domain::Result<void> requestRestart(
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            ++calls[index(Operation::Restart)];
            contexts[index(Operation::Restart)] = operation;
            if (failures[index(Operation::Restart)]) {
                return Domain::Result<void>::failure(
                    *failures[index(Operation::Restart)]);
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }

    void shutdown() noexcept override {}

private:
    [[nodiscard]] static constexpr std::size_t index(
        const Operation operation) noexcept
    {
        return static_cast<std::size_t>(operation);
    }

    template <typename Value>
    [[nodiscard]] Domain::Result<Value> finish(
        const Operation which,
        const Domain::OperationContext& operation,
        Value value) noexcept
    {
        try {
            ++calls[index(which)];
            contexts[index(which)] = operation;
            if (failures[index(which)]) {
                return Domain::Result<Value>::failure(*failures[index(which)]);
            }
            return Domain::Result<Value>::success(std::move(value));
        } catch (...) {
            return Domain::Result<Value>::failure(
                dependencyError(Domain::ErrorCodes::InternalFailure));
        }
    }
};

[[nodiscard]] Dashboard::DashboardStaticResourcePath staticPath(
    const std::string_view value)
{
    return take(Dashboard::DashboardStaticResourcePath::decode(value));
}

[[nodiscard]] std::unique_ptr<Dashboard::DashboardStaticAssetStore>
createAssetStore()
{
    std::vector<Dashboard::DashboardStaticAssetDefinition> assets{
        {staticPath("/static/root.html"), bytes("root shell")},
        {staticPath("/static/index.html"), bytes("index shell")},
        {staticPath("/static/control.html"), bytes("control shell")},
        {staticPath("/static/manager.html"), bytes("manager shell")},
        {staticPath("/static/app.js"), bytes("window.forge = true;")}};
    std::vector<Dashboard::DashboardShellAssetMapping> mappings{
        {Dashboard::DashboardShellAssetId::Root,
         staticPath("/static/root.html")},
        {Dashboard::DashboardShellAssetId::Index,
         staticPath("/static/index.html")},
        {Dashboard::DashboardShellAssetId::Control,
         staticPath("/static/control.html")},
        {Dashboard::DashboardShellAssetId::Manager,
         staticPath("/static/manager.html")}};
    return take(Dashboard::DashboardStaticAssetStore::create(
        std::move(assets), std::move(mappings)));
}

struct Fixture final {
    std::unique_ptr<Dashboard::DashboardStaticAssetStore> backing{
        createAssetStore()};
    AssetStoreFake assets{*backing};
    TelemetrySourceFake telemetry;
    OperationalServiceFake operational;
    ManagerClientFake manager;
    Application::DashboardConnectionApplication application{
        policy(),
        budgets(),
        identity(),
        "127.0.0.1",
        47'820U,
        assets,
        telemetry,
        operational,
        manager};

    [[nodiscard]] Dashboard::DashboardPreparedExchange prepare(
        Dashboard::DashboardHttpRequest input,
        const bool operationalServiceActive = true,
        Domain::OperationContext operation = context())
    {
        return take(application.prepare(
            std::move(input),
            operationalServiceActive,
            std::move(operation)));
    }

    [[nodiscard]] std::size_t dependencyCalls() const noexcept
    {
        std::size_t total = assets.staticCalls + assets.shellCalls +
            telemetry.healthCalls + telemetry.latestCalls +
            telemetry.subscribeCalls;
        for (const auto value : operational.calls) total += value;
        for (const auto value : manager.calls) total += value;
        return total;
    }
};

void requireJsonResponse(
    const Dashboard::DashboardPreparedExchange& exchange,
    const std::uint16_t status,
    const std::optional<std::string_view> publicCode = std::nullopt)
{
    REQUIRE(wireStatus(exchange) == status);
    const auto document = wireJson(exchange);
    REQUIRE(document.is_object());
    if (publicCode) {
        REQUIRE(document.at("ok") == false);
        REQUIRE(document.at("code") == std::string{*publicCode});
        const auto body = wireBody(exchange);
        REQUIRE(body.find("private dependency detail") == std::string::npos);
        REQUIRE(body.find("token=secret") == std::string::npos);
        REQUIRE(body.find("private-evidence") == std::string::npos);
    }
}

void exposesClosedApplicationBoundary()
{
    static_assert(std::is_final_v<Application::DashboardConnectionApplication>);
    static_assert(std::is_base_of_v<
                  Dashboard::IDashboardConnectionApplication,
                  Application::DashboardConnectionApplication>);
    static_assert(!std::is_copy_constructible_v<
                  Application::DashboardConnectionApplication>);
    static_assert(noexcept(Application::DashboardConnectionApplication(
        std::declval<Dashboard::DashboardRequestPolicy>(),
        std::declval<Domain::ResourceBudgets>(),
        std::declval<Dashboard::DashboardApplicationIdentity>(),
        std::declval<std::string>(),
        std::declval<std::uint16_t>(),
        std::declval<Dashboard::IDashboardAssetStore&>(),
        std::declval<Dashboard::IDashboardTelemetrySource&>(),
        std::declval<Dashboard::IDashboardOperationalService&>(),
        std::declval<Contracts::IManagerClient&>())));
    static_assert(noexcept(
        std::declval<Application::DashboardConnectionApplication&>().prepare(
            std::declval<Dashboard::DashboardHttpRequest>(),
            true,
            std::declval<Domain::OperationContext>())));
    static_assert(noexcept(
        std::declval<Application::DashboardConnectionApplication&>()
            .executePostDelivery(
                Dashboard::DashboardPostDeliveryAction::None,
                std::declval<Domain::OperationContext>())));
}

enum class ExpectedDependency {
    None,
    Shell,
    Static,
    Health,
    Latest,
    StatusComposite,
    Doctor,
    Agents,
    Sessions,
    Audit,
    Diagnostics,
    Prune,
    Close,
    ManagerStatus,
    ManagerSettings,
    ManagerUpdate,
    ManagerStart,
    ManagerStop,
    RestartAction,
    ShutdownAction,
};

struct RouteCase final {
    std::string_view method;
    std::string_view target;
    bool authorized;
    bool mutation;
    std::string_view body;
    ExpectedDependency dependency;
};

[[nodiscard]] std::vector<RouteCase> releasedRoutes()
{
    constexpr std::string_view CloseBody =
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":"operator closed"})json";
    constexpr std::string_view SettingsBody =
        R"json({"apply":false,"manager":{"auto_restart":false}})json";
    return {
        {"GET", "/", false, false, {}, ExpectedDependency::Shell},
        {"GET", "/index.html", false, false, {}, ExpectedDependency::Shell},
        {"GET", "/control", false, false, {}, ExpectedDependency::Shell},
        {"GET", "/manager", false, false, {}, ExpectedDependency::Shell},
        {"GET", "/static/app.js?v=alpha", false, false, {},
         ExpectedDependency::Static},
        {"GET", "/api/health", true, false, {}, ExpectedDependency::Health},
        {"GET", "/api/live", true, false, {}, ExpectedDependency::Latest},
        {"GET", "/api/frame", true, false, {}, ExpectedDependency::Latest},
        {"GET", "/api/snapshot", true, false, {}, ExpectedDependency::Latest},
        {"GET", "/api/system", true, false, {}, ExpectedDependency::Latest},
        {"GET", "/api/forge", true, false, {}, ExpectedDependency::Latest},
        {"GET", "/ping", false, false, {}, ExpectedDependency::None},
        {"GET", "/api/status", true, false, {},
         ExpectedDependency::StatusComposite},
        {"GET", "/api/doctor", true, false, {}, ExpectedDependency::Doctor},
        {"GET", "/api/agents", true, false, {}, ExpectedDependency::Agents},
        {"GET", "/api/sessions", true, false, {}, ExpectedDependency::Sessions},
        {"GET", "/api/audit", true, false, {}, ExpectedDependency::Audit},
        {"GET", "/api/diagnostics", true, false, {},
         ExpectedDependency::Diagnostics},
        {"POST", "/api/sessions/prune", true, true, "{}",
         ExpectedDependency::Prune},
        {"POST", "/api/sessions/close", true, true, CloseBody,
         ExpectedDependency::Close},
        {"GET", "/api/manager/status", true, false, {},
         ExpectedDependency::ManagerStatus},
        {"GET", "/api/manager/settings", true, false, {},
         ExpectedDependency::ManagerSettings},
        {"POST", "/api/manager/settings", true, true, SettingsBody,
         ExpectedDependency::ManagerUpdate},
        {"PUT", "/api/manager/settings", true, true, SettingsBody,
         ExpectedDependency::ManagerUpdate},
        {"POST", "/api/manager/start", true, true, "{}",
         ExpectedDependency::ManagerStart},
        {"POST", "/api/manager/stop", true, true, "{}",
         ExpectedDependency::ManagerStop},
        {"POST", "/api/manager/restart", true, true, "{}",
         ExpectedDependency::RestartAction},
        {"POST", "/api/manager/shutdown", true, true, "{}",
         ExpectedDependency::ShutdownAction},
    };
}

void requireContext(
    const std::optional<Domain::OperationContext>& actual,
    const Domain::OperationContext& expected)
{
    REQUIRE(actual.has_value());
    REQUIRE(sameContext(*actual, expected));
}

void requireRouteDependency(
    Fixture& fixture,
    const ExpectedDependency expected,
    const Domain::OperationContext& operation)
{
    using OperationalOperation = OperationalServiceFake::Operation;
    using ManagerOperation = ManagerClientFake::Operation;

    switch (expected) {
    case ExpectedDependency::None:
        REQUIRE(fixture.dependencyCalls() == 0U);
        break;
    case ExpectedDependency::Shell:
        REQUIRE(fixture.assets.shellCalls == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Static:
        REQUIRE(fixture.assets.staticCalls == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Health:
        REQUIRE(fixture.telemetry.healthCalls == 1U);
        requireContext(fixture.telemetry.lastHealthContext, operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Latest:
        REQUIRE(fixture.telemetry.latestCalls == 1U);
        requireContext(fixture.telemetry.lastLatestContext, operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::StatusComposite:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Status) == 1U);
        REQUIRE(fixture.telemetry.healthCalls == 1U);
        REQUIRE(fixture.manager.callCount(ManagerOperation::Status) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Status),
            operation);
        requireContext(fixture.telemetry.lastHealthContext, operation);
        requireContext(
            fixture.manager.lastContext(ManagerOperation::Status), operation);
        REQUIRE(fixture.dependencyCalls() == 3U);
        break;
    case ExpectedDependency::Doctor:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Doctor) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Doctor),
            operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Agents:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Agents) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Agents),
            operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Sessions:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Sessions) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Sessions),
            operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Audit:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Audit) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Audit),
            operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Diagnostics:
        REQUIRE(fixture.operational.callCount(
                    OperationalOperation::Diagnostics) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Diagnostics),
            operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Prune:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Prune) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Prune),
            operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::Close:
        REQUIRE(fixture.operational.callCount(OperationalOperation::Close) == 1U);
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Close),
            operation);
        REQUIRE(fixture.operational.closedSessionId == SessionUuid);
        REQUIRE(fixture.operational.closedSummary == "operator closed");
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::ManagerStatus:
        REQUIRE(fixture.manager.callCount(ManagerOperation::Status) == 1U);
        requireContext(
            fixture.manager.lastContext(ManagerOperation::Status), operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::ManagerSettings:
        REQUIRE(fixture.manager.callCount(ManagerOperation::Settings) == 1U);
        requireContext(
            fixture.manager.lastContext(ManagerOperation::Settings), operation);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::ManagerUpdate:
        REQUIRE(fixture.manager.callCount(ManagerOperation::UpdateSettings) == 1U);
        requireContext(
            fixture.manager.lastContext(ManagerOperation::UpdateSettings),
            operation);
        REQUIRE(fixture.manager.lastApply == false);
        REQUIRE(fixture.manager.lastPatch.has_value());
        REQUIRE(fixture.manager.lastPatch->autoRestart == false);
        REQUIRE(fixture.dependencyCalls() == 1U);
        break;
    case ExpectedDependency::ManagerStart:
    case ExpectedDependency::ManagerStop:
        REQUIRE(fixture.manager.callCount(ManagerOperation::Control) == 1U);
        requireContext(
            fixture.manager.lastContext(ManagerOperation::Control), operation);
        REQUIRE(fixture.manager.lastControl.has_value());
        REQUIRE(fixture.dependencyCalls() == 1U);
        if (expected == ExpectedDependency::ManagerStart) {
            REQUIRE(*fixture.manager.lastControl == Domain::ManagerControlAction::Start);
        } else {
            REQUIRE(*fixture.manager.lastControl == Domain::ManagerControlAction::Stop);
        }
        break;
    case ExpectedDependency::RestartAction:
        REQUIRE(fixture.manager.callCount(ManagerOperation::Restart) == 0U);
        REQUIRE(fixture.dependencyCalls() == 0U);
        break;
    case ExpectedDependency::ShutdownAction:
        REQUIRE(fixture.manager.callCount(ManagerOperation::Shutdown) == 0U);
        REQUIRE(fixture.dependencyCalls() == 0U);
        break;
    }
}

void dispatchesEveryReleasedNonStreamingRoute()
{
    for (const auto& route : releasedRoutes()) {
        Fixture fixture;
        const auto operation = context();
        auto exchange = fixture.prepare(
            request(
                std::string{route.method},
                std::string{route.target},
                route.authorized,
                route.mutation,
                std::string{route.body}),
            true,
            operation);
        const auto actualStatus = wireStatus(exchange);
        if (actualStatus != 200U) {
            throw std::runtime_error{
                "route " + std::string{route.method} + " " +
                std::string{route.target} + " returned " +
                std::to_string(actualStatus) + ": " + wireBody(exchange)};
        }
        REQUIRE(actualStatus == 200U);
        REQUIRE(!wireBody(exchange).empty());
        requireRouteDependency(fixture, route.dependency, operation);

        auto* complete = exchange.completeExchange();
        REQUIRE(complete != nullptr);
        const auto action = complete->takePostDeliveryAction();
        if (route.dependency == ExpectedDependency::RestartAction) {
            REQUIRE(action ==
                    Dashboard::DashboardPostDeliveryAction::RequestManagerRestart);
            const auto acknowledgement = wireJson(exchange);
            REQUIRE(acknowledgement.at("ok") == true);
            REQUIRE(acknowledgement.at("message") ==
                    "Manager restart accepted");
            REQUIRE(acknowledgement.at("state") == "restarting");
        } else if (route.dependency == ExpectedDependency::ShutdownAction) {
            REQUIRE(action ==
                    Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
            const auto acknowledgement = wireJson(exchange);
            REQUIRE(acknowledgement.at("ok") == true);
            REQUIRE(acknowledgement.at("message") ==
                    "Manager shutting down");
            REQUIRE(acknowledgement.at("state") == "stopping");
        } else {
            REQUIRE(action == Dashboard::DashboardPostDeliveryAction::None);
        }
        REQUIRE(complete->takePostDeliveryAction() ==
                Dashboard::DashboardPostDeliveryAction::None);
    }
}

void pingReportsOnlyManagerDashboardReachability()
{
    Fixture fixture;
    const auto response = fixture.prepare(request("GET", "/ping"));
    REQUIRE(wireStatus(response) == 200U);
    const auto body = wireBody(response);
    REQUIRE(body.find("Forge Conductor Manager is reachable") !=
            std::string::npos);
    REQUIRE(body.find("dashboard endpoint ready") != std::string::npos);
    REQUIRE(body.find("Forge Telemetry OK") == std::string::npos);
    REQUIRE(body.find("Telemetry is reachable") == std::string::npos);
    REQUIRE(body.find("continuous native collectors") == std::string::npos);
    REQUIRE(body.find("SSE realtime") == std::string::npos);
    REQUIRE(fixture.dependencyCalls() == 0U);
}

void emitsHeadOnlyRejectionsForEveryRoute()
{
    auto routes = releasedRoutes();
    routes.push_back(
        {"HEAD", "/api/stream?hz=1", true, false, {},
         ExpectedDependency::None});
    routes.push_back(
        {"HEAD", "/api/tools/call", true, false, {},
         ExpectedDependency::None});

    for (const auto& route : routes) {
        Fixture fixture;
        auto input = request(
            "HEAD",
            std::string{route.target},
            route.authorized,
            false);
        const auto planned = Dashboard::DashboardRequestPlanner::plan(
            policy(), input, true, budgets());
        REQUIRE(planned.rejection() != nullptr);
        const auto fullRejection = take(
            Dashboard::DashboardResponseComposer::rejection(
                *planned.rejection()));
        const auto expectedRepresentationLength =
            wireBody(fullRejection).size();
        auto exchange = fixture.prepare(std::move(input));
        const auto expectedStatus =
            route.target == "/api/tools/call" ? 404U : 405U;
        REQUIRE(wireStatus(exchange) == expectedStatus);
        REQUIRE(exchange.completeExchange() != nullptr);
        REQUIRE(
            exchange.completeExchange()->encodedResponse().kind() ==
            Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead);
        const auto wire = wireText(exchange);
        REQUIRE(wire.ends_with("\r\n\r\n"));
        REQUIRE(contentLength(wire) == expectedRepresentationLength);
        REQUIRE(exchange.completeExchange()->takePostDeliveryAction() ==
                Dashboard::DashboardPostDeliveryAction::None);
        REQUIRE(fixture.dependencyCalls() == 0U);
    }

    Fixture stopped;
    auto stoppedInput = request("HEAD", "/api/doctor");
    const auto stoppedPlan = Dashboard::DashboardRequestPlanner::plan(
        policy(), stoppedInput, false, budgets());
    REQUIRE(stoppedPlan.rejection() != nullptr);
    const auto stoppedRepresentation = take(
        Dashboard::DashboardResponseComposer::rejection(
            *stoppedPlan.rejection()));
    auto stoppedHead = stopped.prepare(std::move(stoppedInput), false);
    REQUIRE(wireStatus(stoppedHead) == 503U);
    REQUIRE(
        stoppedHead.completeExchange()->encodedResponse().kind() ==
        Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead);
    REQUIRE(wireText(stoppedHead).ends_with("\r\n\r\n"));
    REQUIRE(contentLength(wireText(stoppedHead)) ==
            wireBody(stoppedRepresentation).size());
    REQUIRE(stopped.dependencyCalls() == 0U);
}

void rejectsBeforeDispatchWithoutTouchingDependencies()
{
    const auto requireRejected = [](
        Fixture& fixture,
        Dashboard::DashboardHttpRequest input,
        const std::uint16_t status,
        const std::string_view code,
        const bool operationalServiceActive = true) {
        auto exchange = fixture.prepare(
            std::move(input), operationalServiceActive);
        requireJsonResponse(exchange, status, code);
        REQUIRE(fixture.dependencyCalls() == 0U);
    };

    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("GET", "/api/status", false),
            401U,
            "unauthorized");
        REQUIRE(wireText(fixture.prepare(request("GET", "/api/status", false)))
                    .find("WWW-Authenticate: Bearer\r\n") != std::string::npos);
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            Dashboard::DashboardHttpRequest{
                "GET",
                "/",
                {{"host", "localhost:47820"}},
                {}},
            403U,
            "forbidden");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("POST", "/api/manager/start", true, false, "{}"),
            415U,
            "unsupported_media_type");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("GET", "/not-a-route"),
            404U,
            "not_found");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("GET", "/api/sessions/prune"),
            405U,
            "method_not_allowed");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("GET", "/api/doctor"),
            503U,
            "service_stopped",
            false);
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("GET", "/api/stream?hz="),
            400U,
            "invalid_query");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("GET", "/static/%2e%2e/app.js", false),
            404U,
            "not_found");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("POST", "/api/tools/call", true, true, "{}"),
            404U,
            "not_found");
    }
    {
        Fixture fixture;
        requireRejected(
            fixture,
            request("OPTIONS", "/"),
            405U,
            "method_not_allowed");
    }

    Fixture unknownHead;
    auto head = unknownHead.prepare(request("HEAD", "/not-a-route"));
    REQUIRE(wireStatus(head) == 404U);
    REQUIRE(
        head.completeExchange()->encodedResponse().kind() ==
        Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead);
    REQUIRE(wireText(head).ends_with("\r\n\r\n"));
    REQUIRE(unknownHead.dependencyCalls() == 0U);
}

void mapsTypedDependencyFailuresToStablePublicErrors()
{
    struct Mapping final {
        std::string_view sourceCode;
        std::uint16_t status;
        std::string_view publicCode;
    };
    const std::vector<Mapping> mappings{
        {Domain::ErrorCodes::InvalidRequest, 400U, "invalid_request"},
        {Domain::ErrorCodes::Unauthorized, 403U, "forbidden"},
        {Domain::ErrorCodes::SessionNotFound, 404U, "not_found"},
        {Domain::ErrorCodes::Conflict, 409U, "conflict"},
        {Domain::ErrorCodes::PayloadTooLarge, 413U, "payload_too_large"},
        {Domain::ErrorCodes::UnsupportedVersion, 422U, "unsupported_version"},
        {Domain::ErrorCodes::LimitExceeded, 429U, "rate_limited"},
        {Domain::ErrorCodes::DeadlineExceeded, 503U, "service_unavailable"},
        {Domain::ErrorCodes::IntegrityFailure, 500U, "internal_failure"},
        {"untrusted_unknown_code", 500U, "internal_failure"},
    };
    for (const auto& mapping : mappings) {
        Fixture fixture;
        fixture.manager.setFailure(
            ManagerClientFake::Operation::Status,
            dependencyError(mapping.sourceCode));
        auto exchange = fixture.prepare(
            request("GET", "/api/manager/status"));
        requireJsonResponse(exchange, mapping.status, mapping.publicCode);
        REQUIRE(fixture.manager.callCount(
                    ManagerClientFake::Operation::Status) == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
    }
}

void mapsDecoderAndResponseEncodingFailuresWithoutCallingMutations()
{
    {
        Fixture fixture;
        auto response = fixture.prepare(request(
            "POST",
            "/api/sessions/close",
            true,
            true,
            R"json({"session_id":"12345678-1234-4234-8234-123456789abc"})json"));
        REQUIRE(wireStatus(response) == 200U);
        REQUIRE(fixture.operational.closedSummary ==
                Dashboard::DashboardSessionCloseRequest::DefaultSummary);
        REQUIRE(fixture.operational.callCount(
                    OperationalServiceFake::Operation::Close) == 1U);
    }
    {
        Fixture fixture;
        auto response = fixture.prepare(request(
            "POST", "/api/sessions/close", true, true, "{}"));
        REQUIRE(wireStatus(response) == 400U);
        const auto document = wireJson(response);
        REQUIRE(document.size() == 2U);
        REQUIRE(document.at("ok") == false);
        REQUIRE(document.at("message") == "session_id required");
        REQUIRE(!document.contains("code"));
        REQUIRE(fixture.operational.callCount(
                    OperationalServiceFake::Operation::Close) == 0U);
        REQUIRE(fixture.dependencyCalls() == 0U);
    }
    {
        Fixture fixture;
        auto response = fixture.prepare(request(
            "POST",
            "/api/sessions/close",
            true,
            true,
            "{not-json"));
        requireJsonResponse(response, 400U, "invalid_request");
        REQUIRE(fixture.operational.callCount(
                    OperationalServiceFake::Operation::Close) == 0U);
        REQUIRE(fixture.dependencyCalls() == 0U);
    }
    {
        Fixture fixture;
        const std::string oversized(
            Dashboard::DashboardSessionCloseRequestDecoder::
                    MaximumRequestBytes +
                1U,
            'x');
        auto response = fixture.prepare(request(
            "POST",
            "/api/sessions/close",
            true,
            true,
            oversized));
        requireJsonResponse(response, 413U, "payload_too_large");
        REQUIRE(fixture.operational.callCount(
                    OperationalServiceFake::Operation::Close) == 0U);
    }
    {
        Fixture fixture;
        auto response = fixture.prepare(request(
            "POST",
            "/api/manager/settings",
            true,
            true,
            R"json({"apply":true,"apply":false})json"));
        requireJsonResponse(response, 400U, "invalid_request");
        REQUIRE(fixture.manager.callCount(
                    ManagerClientFake::Operation::UpdateSettings) == 0U);
        REQUIRE(fixture.dependencyCalls() == 0U);
    }
    {
        Fixture fixture;
        fixture.manager.statusValue.version.assign(
            Dashboard::DashboardResponseComposer::MaximumResponseBodyBytes,
            'v');
        auto response = fixture.prepare(
            request("GET", "/api/manager/status"));
        requireJsonResponse(response, 500U, "response_too_large");
        REQUIRE(fixture.manager.callCount(
                    ManagerClientFake::Operation::Status) == 1U);
    }
}

void handlesAssetAbsenceByRouteSemantics()
{
    {
        Fixture fixture;
        auto missing = fixture.prepare(
            request("GET", "/static/missing.js", false));
        REQUIRE(wireStatus(missing) == 404U);
        REQUIRE(wireBody(missing) == "Not Found");
        REQUIRE(fixture.assets.staticCalls == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
    }
    {
        Fixture fixture;
        fixture.assets.failure = dependencyError(
            Domain::ErrorCodes::RecordNotFound);
        auto missingShell = fixture.prepare(request("GET", "/", false));
        requireJsonResponse(missingShell, 500U, "internal_failure");
        REQUIRE(fixture.assets.shellCalls == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
    }
    {
        Fixture fixture;
        fixture.assets.returnNull = true;
        auto nullAsset = fixture.prepare(request("GET", "/", false));
        requireJsonResponse(nullAsset, 500U, "internal_failure");
        REQUIRE(fixture.assets.shellCalls == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
    }
}

void telemetryAliasesShareOneOwnedObservationShape()
{
    std::vector<std::string> bodies;
    for (const auto target : {
             "/api/live", "/api/frame", "/api/snapshot"}) {
        Fixture fixture;
        auto response = fixture.prepare(request("GET", target));
        REQUIRE(fixture.telemetry.latestCalls == 1U);
        bodies.push_back(wireBody(response));
    }
    REQUIRE(bodies.size() == 3U);
    REQUIRE(bodies[0] == bodies[1]);
    REQUIRE(bodies[1] == bodies[2]);
}

void ownsSseSubscriptionAndMapsCapacityFailures()
{
    {
        Fixture fixture;
        const auto operation = context();
        {
            auto exchange = fixture.prepare(
                request("GET", "/api/stream?hz=1.5"),
                true,
                operation);
            REQUIRE(
                exchange.kind() ==
                Dashboard::DashboardPreparedExchange::Kind::ServerSentEvents);
            REQUIRE(exchange.sseExchange() != nullptr);
            REQUIRE(exchange.sseExchange()->subscription() != nullptr);
            REQUIRE(exchange.sseExchange()->subscription()->deliveryHz() == 1.5);
            REQUIRE(
                exchange.sseExchange()->encodedHead().kind() ==
                Dashboard::DashboardHttpEncodingResult::Kind::SseBootstrapHead);
            REQUIRE(
                text(exchange.sseExchange()->connectedCommentBytes()) ==
                ": connected realtime\n\n");
            REQUIRE(fixture.telemetry.subscribeCalls == 1U);
            REQUIRE(fixture.telemetry.lastRate.has_value());
            REQUIRE(fixture.telemetry.lastRate->deliveryHz == 1.5);
            requireContext(fixture.telemetry.lastSubscribeContext, operation);
            REQUIRE(fixture.telemetry.subscriptionState->closes == 0U);
            REQUIRE(fixture.telemetry.subscriptionState->destructions == 0U);
        }
        REQUIRE(fixture.telemetry.subscriptionState->closes == 1U);
        REQUIRE(fixture.telemetry.subscriptionState->destructions == 1U);
    }
    for (const auto code : {
             Domain::ErrorCodes::LimitExceeded,
             Domain::ErrorCodes::RateLimited,
             Domain::ErrorCodes::TransportClosed}) {
        Fixture fixture;
        fixture.telemetry.subscribeFailure = dependencyError(code);
        auto response = fixture.prepare(
            request("GET", "/api/stream?hz=2"));
        requireJsonResponse(response, 503U, "stream_unavailable");
        REQUIRE(fixture.telemetry.subscribeCalls == 1U);
        REQUIRE(fixture.telemetry.subscriptionState->closes == 0U);
        REQUIRE(fixture.telemetry.subscriptionState->destructions == 0U);
        REQUIRE(fixture.dependencyCalls() == 1U);
    }
    {
        Fixture fixture;
        fixture.telemetry.subscriptionDeliveryHz = 0.5;
        auto response = fixture.prepare(request("GET", "/api/stream"));
        requireJsonResponse(response, 500U, "response_too_large");
        REQUIRE(fixture.telemetry.subscribeCalls == 1U);
        REQUIRE(fixture.telemetry.subscriptionState->closes == 1U);
        REQUIRE(fixture.telemetry.subscriptionState->destructions == 1U);
        REQUIRE(fixture.dependencyCalls() == 1U);
    }
}

void rejectsNullTelemetryObservationsWithoutDereferencingThem()
{
    Fixture fixture;
    fixture.telemetry.returnNullSnapshot = true;
    auto response = fixture.prepare(request("GET", "/api/live"));
    requireJsonResponse(response, 500U, "internal_failure");
    REQUIRE(fixture.telemetry.latestCalls == 1U);
    REQUIRE(fixture.dependencyCalls() == 1U);
}

void statusUsesOneContextAndShortCircuitsInDependencyOrder()
{
    using OperationalOperation = OperationalServiceFake::Operation;
    using ManagerOperation = ManagerClientFake::Operation;

    {
        Fixture fixture;
        fixture.telemetry.healthValue.report.ok = false;
        fixture.telemetry.healthValue.report.mode = "unavailable";
        fixture.telemetry.healthValue.measuredSampleHz = 0.0;
        fixture.telemetry.healthValue.streamRunning = false;
        const auto operation = context(
            "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
            "status-success");
        auto response = fixture.prepare(
            request("GET", "/api/status"), true, operation);
        REQUIRE(wireStatus(response) == 200U);
        const auto document = wireJson(response);
        REQUIRE(document.at("ok") == true);
        REQUIRE(document.at("telemetry").at("ok") == false);
        REQUIRE(document.at("telemetry").at("mode") == "unavailable");
        REQUIRE(!document.at("telemetry").contains("sample_hz_measured"));
        REQUIRE(!document.at("telemetry").contains("stream_running"));
        requireContext(
            fixture.operational.lastContext(OperationalOperation::Status),
            operation);
        requireContext(fixture.telemetry.lastHealthContext, operation);
        requireContext(
            fixture.manager.lastContext(ManagerOperation::Status), operation);
    }
    {
        Fixture fixture;
        fixture.operational.setFailure(
            OperationalOperation::Status,
            dependencyError(Domain::ErrorCodes::DatabaseBusy));
        auto response = fixture.prepare(request("GET", "/api/status"));
        requireJsonResponse(response, 503U, "service_unavailable");
        REQUIRE(fixture.operational.callCount(OperationalOperation::Status) == 1U);
        REQUIRE(fixture.telemetry.healthCalls == 0U);
        REQUIRE(fixture.manager.callCount(ManagerOperation::Status) == 0U);
    }
    {
        Fixture fixture;
        fixture.telemetry.healthFailure = dependencyError(
            Domain::ErrorCodes::DatabaseBusy);
        auto response = fixture.prepare(request("GET", "/api/status"));
        requireJsonResponse(response, 503U, "service_unavailable");
        REQUIRE(fixture.operational.callCount(OperationalOperation::Status) == 1U);
        REQUIRE(fixture.telemetry.healthCalls == 1U);
        REQUIRE(fixture.manager.callCount(ManagerOperation::Status) == 0U);
    }
    {
        Fixture fixture;
        fixture.manager.setFailure(
            ManagerOperation::Status,
            dependencyError(Domain::ErrorCodes::DatabaseBusy));
        auto response = fixture.prepare(request("GET", "/api/status"));
        requireJsonResponse(response, 503U, "service_unavailable");
        REQUIRE(fixture.operational.callCount(OperationalOperation::Status) == 1U);
        REQUIRE(fixture.telemetry.healthCalls == 1U);
        REQUIRE(fixture.manager.callCount(ManagerOperation::Status) == 1U);
    }
}

void executesManagerLifecycleRequestsOnlyAfterDelivery()
{
    Fixture fixture;
    auto restartResponse = fixture.prepare(request(
        "POST", "/api/manager/restart", true, true, "{}"));
    REQUIRE(wireStatus(restartResponse) == 200U);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Restart) == 0U);
    auto* restartComplete = restartResponse.completeExchange();
    REQUIRE(restartComplete != nullptr);
    const auto restartAction = restartComplete->takePostDeliveryAction();
    REQUIRE(restartAction ==
            Dashboard::DashboardPostDeliveryAction::RequestManagerRestart);

    const auto restartContext = context(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "restart-post-delivery");
    const auto restartExecuted = fixture.application.executePostDelivery(
        restartAction, restartContext);
    REQUIRE(restartExecuted);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Restart) == 1U);
    requireContext(
        fixture.manager.lastContext(ManagerClientFake::Operation::Restart),
        restartContext);

    auto response = fixture.prepare(request(
        "POST", "/api/manager/shutdown", true, true, "{}"));
    REQUIRE(wireStatus(response) == 200U);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Shutdown) == 0U);
    auto* complete = response.completeExchange();
    REQUIRE(complete != nullptr);
    const auto action = complete->takePostDeliveryAction();
    REQUIRE(action ==
            Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(complete->takePostDeliveryAction() ==
            Dashboard::DashboardPostDeliveryAction::None);

    const auto postContext = context(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
        "post-delivery");
    const auto executed = fixture.application.executePostDelivery(
        action, postContext);
    REQUIRE(executed);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Shutdown) == 1U);
    requireContext(
        fixture.manager.lastContext(ManagerClientFake::Operation::Shutdown),
        postContext);

    const auto none = fixture.application.executePostDelivery(
        Dashboard::DashboardPostDeliveryAction::None,
        context(
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            "no-action"));
    REQUIRE(none);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Shutdown) == 1U);

    const auto invalid = fixture.application.executePostDelivery(
        static_cast<Dashboard::DashboardPostDeliveryAction>(255U),
        context(
            "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
            "invalid-action"));
    REQUIRE(!invalid);
    REQUIRE(invalid.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Shutdown) == 1U);

    fixture.manager.setFailure(
        ManagerClientFake::Operation::Shutdown,
        dependencyError(Domain::ErrorCodes::TransportClosed));
    const auto failed = fixture.application.executePostDelivery(
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown,
        context(
            "ffffffff-ffff-4fff-8fff-ffffffffffff",
            "failed-shutdown"));
    REQUIRE(!failed);
    REQUIRE(failed.error().code == Domain::ErrorCodes::TransportClosed);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Shutdown) == 2U);

    fixture.manager.setFailure(
        ManagerClientFake::Operation::Restart,
        dependencyError(Domain::ErrorCodes::TransportClosed));
    const auto failedRestart = fixture.application.executePostDelivery(
        Dashboard::DashboardPostDeliveryAction::RequestManagerRestart,
        context(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            "failed-restart"));
    REQUIRE(!failedRestart);
    REQUIRE(failedRestart.error().code == Domain::ErrorCodes::TransportClosed);
    REQUIRE(fixture.manager.callCount(
                ManagerClientFake::Operation::Restart) == 2U);
}

} // namespace

int main()
{
    try {
        exposesClosedApplicationBoundary();
        dispatchesEveryReleasedNonStreamingRoute();
        pingReportsOnlyManagerDashboardReachability();
        emitsHeadOnlyRejectionsForEveryRoute();
        rejectsBeforeDispatchWithoutTouchingDependencies();
        mapsTypedDependencyFailuresToStablePublicErrors();
        mapsDecoderAndResponseEncodingFailuresWithoutCallingMutations();
        handlesAssetAbsenceByRouteSemantics();
        telemetryAliasesShareOneOwnedObservationShape();
        ownsSseSubscriptionAndMapsCapacityFailures();
        rejectsNullTelemetryObservationsWithoutDereferencingThem();
        statusUsesOneContextAndShortCircuitsInDependencyOrder();
        executesManagerLifecycleRequestsOnlyAfterDelivery();
        std::cout << "Dashboard connection application tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard connection application tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard connection application tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
