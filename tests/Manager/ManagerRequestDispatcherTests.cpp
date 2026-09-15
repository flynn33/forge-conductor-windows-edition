#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"
#include "../Fakes/ProjectRepositoryFakes.h"
#include "../Fakes/RecordingProjectMemoryService.h"
#include "../Fakes/RecordingContinuityCoordinator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Manager;
namespace TestFakes = ForgeConductor::Tests::Fakes;

using namespace std::chrono_literals;

class FakeClock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utc{std::chrono::seconds{1'700'000'000}};
    Domain::MonotonicTimePoint monotonic{std::chrono::seconds{1'000}};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utc;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic;
    }
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error{message};
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] std::string uuidText(const std::uint32_t suffix)
{
    constexpr char Digits[] = "0123456789abcdef";
    std::string value{"00000000-0000-4000-8000-000000000000"};
    auto remaining = suffix;
    for (std::size_t index{}; index < 8U; ++index) {
        value[value.size() - 1U - index] = Digits[remaining & 0xFU];
        remaining >>= 4U;
    }
    return value;
}

[[nodiscard]] Domain::OperationId operationId(const std::uint32_t suffix)
{
    return Domain::OperationId::parse(uuidText(suffix)).value();
}

[[nodiscard]] Domain::RequestId requestId(const std::uint32_t suffix)
{
    return Domain::RequestId::parse(uuidText(suffix)).value();
}

[[nodiscard]] Domain::CorrelationId correlationId(const std::uint32_t suffix)
{
    return Domain::CorrelationId::parse(
               "manager-dispatch-" + std::to_string(suffix))
        .value();
}

[[nodiscard]] Domain::Sha256Digest nonce()
{
    return Domain::Sha256Digest::parse(std::string(64U, '0')).value();
}

[[nodiscard]] std::int64_t futureWireDeadline(
    const FakeClock& clock,
    const std::chrono::milliseconds offset = 1min)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock.utc.time_since_epoch())
               .count() +
        offset.count();
}

[[nodiscard]] Manager::ManagerRequest request(
    const FakeClock& clock,
    const std::uint32_t suffix,
    Manager::ManagerRequestPayload payload)
{
    return Manager::ManagerRequest{
        Manager::ManagerProtocolVersion,
        requestId(suffix),
        correlationId(suffix),
        futureWireDeadline(clock),
        nonce(),
        std::move(payload)};
}

[[nodiscard]] const Domain::Error* responseError(
    const Manager::ManagerResponse& response)
{
    return std::get_if<Domain::Error>(&response.body);
}

template <typename T>
[[nodiscard]] const T* responseValue(const Manager::ManagerResponse& response)
{
    const auto* result = std::get_if<Manager::ManagerResult>(&response.body);
    return result ? std::get_if<T>(result) : nullptr;
}

void requireError(
    const Manager::ManagerResponse& response,
    const std::string_view code,
    const std::string& message)
{
    const auto* failure = responseError(response);
    require(failure != nullptr, message + " unexpectedly succeeded");
    require(failure->code == code, message + " returned the wrong error");
}

class FakeController final : public Contracts::IManagerController {
public:
    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ManagerStatus>::success(statusValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            Domain::ManagerControllerSnapshot{statusValue(), false});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept override
    {
        if (const auto blocked = blockIfRequested("status", context); blocked) {
            return Domain::Result<Domain::ManagerStatus>::failure(*blocked);
        }
        if (failStatus_) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "injected status failure",
                    true));
        }
        ++statusCalls_;
        return Domain::Result<Domain::ManagerStatus>::success(statusValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        ++settingsCalls_;
        return Domain::Result<Domain::ManagerSettings>::success(settingsValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& requestValue,
        const Domain::OperationContext&) noexcept override
    {
        ++controlCalls_;
        lastControlAction_ = requestValue.action;
        return Domain::Result<Domain::ManagerStatus>::success(statusValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext&) noexcept override
    {
        ++updateCalls_;
        lastPatch_ = patch;
        lastApplyImmediately_ = applyImmediately;
        auto settings = settingsValue();
        if (patch.dashboardPort) {
            settings.dashboardPort = *patch.dashboardPort;
        }
        auto status = statusValue();
        status.dashboardPort = settings.dashboardPort;
        return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::success({
            std::move(settings),
            applyImmediately,
            patch.dashboardPort.has_value(),
            std::move(status)});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        events_.push_back("request_shutdown");
        ++requestShutdownCalls_;
        if (failRequestShutdown_) {
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "injected shutdown failure"));
        }
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            Domain::ManagerControllerSnapshot{statusValue(), true});
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        events_.push_back("controller_close");
        ++closeCalls_;
    }

    void setBlocking(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        blocking_ = value;
        condition_.notify_all();
    }

    void setIgnoreCancellation(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        ignoreCancellation_ = value;
    }

    [[nodiscard]] bool waitForActive(
        const std::size_t expected,
        const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout, [&] { return active_ >= expected; });
    }

    [[nodiscard]] std::size_t closeCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return closeCalls_;
    }

    [[nodiscard]] std::size_t requestShutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return requestShutdownCalls_;
    }

    [[nodiscard]] std::vector<std::string> events() const
    {
        std::lock_guard lock{mutex_};
        return events_;
    }

    bool failStatus_{};
    bool failRequestShutdown_{};
    std::atomic_size_t statusCalls_{};
    std::atomic_size_t settingsCalls_{};
    std::atomic_size_t controlCalls_{};
    std::atomic_size_t updateCalls_{};
    Domain::ManagerControlAction lastControlAction_{};
    Domain::ManagerSettingsPatch lastPatch_;
    bool lastApplyImmediately_{};

private:
    [[nodiscard]] static Domain::ManagerStatus statusValue()
    {
        return Domain::ManagerStatus{
            true,
            true,
            Domain::ManagerServiceState::Running,
            true,
            true,
            true,
            42U,
            std::nullopt,
            std::nullopt,
            0U,
            std::nullopt,
            true,
            3s,
            false,
            "127.0.0.1",
            7788U,
            8s,
            Domain::PathText::create("C:\\ManagerDispatcherTest").value(),
            "test"};
    }

    [[nodiscard]] static Domain::ManagerSettings settingsValue()
    {
        Domain::ManagerSettings value;
        value.dashboardPort = 7788;
        return value;
    }

    [[nodiscard]] std::optional<Domain::Error> blockIfRequested(
        const std::string_view name,
        const Domain::OperationContext& context) noexcept
    {
        std::unique_lock lock{mutex_};
        if (!blocking_) {
            return std::nullopt;
        }
        ++active_;
        events_.push_back(std::string{name} + "_enter");
        condition_.notify_all();
        const std::stop_callback cancellation{
            context.cancellation, [this] { condition_.notify_all(); }};
        condition_.wait(lock, [&] {
            return !blocking_ ||
                (!ignoreCancellation_ && context.isCancellationRequested());
        });
        --active_;
        events_.push_back(std::string{name} + "_exit");
        condition_.notify_all();
        if (context.isCancellationRequested()) {
            return Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "fake controller observed cancellation");
        }
        return std::nullopt;
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool blocking_{};
    bool ignoreCancellation_{};
    std::size_t active_{};
    std::size_t closeCalls_{};
    std::size_t requestShutdownCalls_{};
    std::vector<std::string> events_;
};

class FakeManagedRuns final : public Contracts::IManagedRunService {
public:
    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> start(
        const Domain::ManagedRunStartRequest& requestValue,
        const Domain::OperationContext& context) noexcept override
    {
        ++startCalls;
        lastStart = requestValue;
        lastContextOperation = context.operationId;
        return Domain::Result<Domain::ManagedRunSnapshot>::success(
            snapshot(requestValue, Domain::ManagedRunState::Running));
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        ++statusCalls;
        const auto state = stateByRun.find(runId.value());
        auto value = snapshotFor(runId, state == stateByRun.end()
            ? Domain::ManagedRunState::Completed : state->second);
        if (const auto found = projectByRun.find(runId.value());
            found != projectByRun.end()) {
            value.record.projectId = found->second;
        }
        value.record.providerResponseId =
            Domain::ProviderSessionId::parse("response-authoritative-1").value();
        value.record.inputTokens = 101U;
        value.record.outputTokens = 37U;
        value.record.retainedContextTokens = 4'096U;
        if (const auto found = outputByRun.find(runId.value());
            found != outputByRun.end()) value.record.outputText = found->second;
        return Domain::Result<Domain::ManagedRunSnapshot>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        ++cancelCalls;
        auto value = snapshotFor(runId, Domain::ManagedRunState::Cancelling);
        value.cancellationRequested = true;
        return Domain::Result<Domain::ManagedRunSnapshot>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> pause(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        ++pauseCalls;
        auto value = snapshotFor(runId, Domain::ManagedRunState::Paused);
        value.pauseRequested = true;
        return Domain::Result<Domain::ManagedRunSnapshot>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> resume(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        ++resumeCalls;
        return Domain::Result<Domain::ManagedRunSnapshot>::success(
            snapshotFor(runId, Domain::ManagedRunState::Running));
    }

    void shutdown() noexcept override { ++shutdownCalls; }

    std::optional<Domain::ManagedRunStartRequest> lastStart;
    std::map<std::string, Domain::ProjectId> projectByRun;
    std::map<std::string, Domain::ManagedRunState> stateByRun;
    std::map<std::string, std::string> outputByRun;
    std::optional<Domain::OperationId> lastContextOperation;
    std::atomic_size_t startCalls{};
    std::atomic_size_t statusCalls{};
    std::atomic_size_t cancelCalls{};
    std::atomic_size_t pauseCalls{};
    std::atomic_size_t resumeCalls{};
    std::atomic_size_t shutdownCalls{};

private:
    [[nodiscard]] static Domain::ManagedRunSnapshot snapshot(
        const Domain::ManagedRunStartRequest& requestValue,
        const Domain::ManagedRunState state)
    {
        const auto time = Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}};
        return Domain::ManagedRunSnapshot{
            Domain::ManagedRunRecord{
                requestValue.runId,
                requestValue.projectId,
                requestValue.clientId,
                requestValue.task,
                requestValue.authorityGeneration,
                state,
                std::nullopt,
                0U,
                0U,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                {},
                time,
                time},
            true,
            false};
    }

    [[nodiscard]] static Domain::ManagedRunSnapshot snapshotFor(
        const Domain::SessionId& runId,
        const Domain::ManagedRunState state)
    {
        return snapshot(
            Domain::ManagedRunStartRequest{
                runId,
                Domain::ProjectId::parse(uuidText(701U)).value(),
                Domain::ClientId::parse(uuidText(702U)).value(),
                operationId(703U),
                correlationId(703U),
                1U,
                "fixture managed task"},
            state);
    }
};

class FakeDurableManagedRunStore final : public Contracts::IManagedRunStore {
public:
    std::map<std::string, Domain::ManagedRunRecord> records;

    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        const auto found = records.find(runId.value());
        return Domain::Result<std::optional<Domain::ManagedRunRecord>>::success(
            found == records.end()
                ? std::optional<Domain::ManagedRunRecord>{}
                : std::optional<Domain::ManagedRunRecord>{found->second});
    }

    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext&) noexcept override
    {
        records.insert_or_assign(record.runId.value(), record);
        return Domain::Result<void>::success();
    }
};

class FakeEvidenceHasher final : public Contracts::IHasher {
public:
    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        std::span<const std::byte>) noexcept override
    {
        return Domain::Sha256Digest::parse(std::string(64U, 'a'));
    }
};

class FakeOperationalSessions final : public Dashboard::IDashboardOperationalService {
public:
    Dashboard::DashboardSessionListing listing;
    std::size_t sessionCalls{};
    bool allowStatus{};
    [[nodiscard]] Domain::Result<Dashboard::DashboardStatusData> status(
        const Domain::OperationContext&) noexcept override
    {
        if (allowStatus) return Domain::Result<Dashboard::DashboardStatusData>::success({});
        return Domain::Result<Dashboard::DashboardStatusData>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "Unexpected status in run-history test."));
    }
    [[nodiscard]] Domain::Result<Domain::DoctorReport> doctor(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::DoctorReport>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "Unexpected doctor in run-history test."));
    }
    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> agents(
        const Domain::OperationContext&) noexcept override
    { return Domain::Result<std::vector<Domain::AgentSpec>>::success({}); }
    [[nodiscard]] Domain::Result<Dashboard::DashboardSessionListing> sessions(
        const Domain::OperationContext&) noexcept override
    {
        ++sessionCalls;
        return Domain::Result<Dashboard::DashboardSessionListing>::success(listing);
    }
    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> audit(
        const Domain::OperationContext&) noexcept override
    { return Domain::Result<std::vector<Domain::AuditEvent>>::success({}); }
    [[nodiscard]] Domain::Result<std::vector<std::string>> diagnosticLines(
        const Domain::OperationContext&) noexcept override
    { return Domain::Result<std::vector<std::string>>::success({}); }
    [[nodiscard]] Domain::Result<std::size_t> pruneSessions(
        const Domain::OperationContext&) noexcept override
    { return Domain::Result<std::size_t>::success(0U); }
    [[nodiscard]] Domain::Result<Domain::AgentSession> closeSession(
        const Dashboard::DashboardSessionCloseRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::AgentSession>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "Unexpected close in run-history test."));
    }
    void shutdown() noexcept override {}
};

class FakeTelemetryService final : public Contracts::ITelemetryService {
public:
    FakeTelemetryService()
    {
        const auto time = Domain::UtcTimePoint{
            std::chrono::seconds{1'700'000'000}};
        Domain::SystemMetrics system;
        system.timestamp = time;
        system.host = "dispatcher-host";
        system.platform = "Windows 11";
        system.architecture = "x64";
        system.cpu.percent = Domain::makeAvailableTelemetryMetric<double>(
            33.5, time, "GetSystemTimes");
        system.ram.percent = Domain::makeAvailableTelemetryMetric<double>(
            62.0, time, "GlobalMemoryStatusEx");
        system.ram.usedBytes = Domain::makeAvailableTelemetryMetric<std::uint64_t>(
            62U, time, "GlobalMemoryStatusEx");
        system.ram.totalBytes = Domain::makeAvailableTelemetryMetric<std::uint64_t>(
            100U, time, "GlobalMemoryStatusEx");
        system.ram.availableBytes =
            Domain::makeAvailableTelemetryMetric<std::uint64_t>(
                38U, time, "GlobalMemoryStatusEx");
        system.processes.push_back(Domain::ProcessMetrics{
            42U, "Manager", 2.5, 1'024U, 768U, 4U, 16U,
            "GetProcessTimes"});
        snapshot_ = std::make_shared<const Domain::TelemetrySnapshot>(
            Domain::TelemetrySnapshot{
                std::move(system),
                Domain::ForgeSnapshot{
                    time,
                    Domain::PathText::create("C:\\TelemetryTest").value(),
                    "windows-manager",
                    0U,
                    0U,
                    {},
                    {},
                    0U,
                    Domain::TelemetryHealth::Ok},
                time,
                {Domain::HistoryPoint{
                    time, 33.5, 62.0, std::nullopt, 0.0, 0U,
                    Domain::TelemetryHealth::Ok}},
                "windows-native"});
    }

    [[nodiscard]] Domain::Result<void> start(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<Snapshot> sample(
        bool,
        const Domain::OperationContext&) noexcept override
    {
        ++sampleCalls;
        return Domain::Result<Snapshot>::success(snapshot_);
    }

    [[nodiscard]] Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::TelemetryHealthReport>::success(
            Domain::TelemetryHealthReport{
                true, "telemetry", "windows-native", false, "continuous",
                "fixture", "native", false});
    }

    [[nodiscard]] Domain::Result<void> setConsumer(Consumer consumer) noexcept override
    {
        consumer_ = std::move(consumer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Snapshot latest() const noexcept override { return snapshot_; }
    [[nodiscard]] std::size_t pendingCount() const noexcept override { return 0U; }
    void stop() noexcept override {}

    std::atomic_size_t sampleCalls{};

private:
    Snapshot snapshot_;
    Consumer consumer_;
};

void testTelemetrySnapshotUsesManagerOwnedRunValues()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    auto managedRuns = std::make_shared<FakeManagedRuns>();
    FakeTelemetryService telemetry;
    Manager::ManagerRequestDispatcher dispatcher{
        controller,
        clock,
        Manager::ManagerTransportLimits{},
        managedRuns,
        Manager::ManagerTelemetrySources{
            &telemetry, nullptr, nullptr, nullptr, nullptr}};

    const auto runId = Domain::SessionId::parse(uuidText(700U)).value();
    const auto response = dispatcher.dispatch(request(
        *clock, 76U, Manager::ManagerTelemetryRequest{runId}));
    const auto* snapshot = responseValue<Domain::ManagerTelemetrySnapshot>(response);
    require(snapshot != nullptr, "manager telemetry result");
    require(snapshot->resources.cpuPercent.value == 33.5,
            "manager telemetry reuses native CPU sample");
    require(snapshot->resources.history.size() == 1U,
            "manager telemetry reuses bounded resource history");
    require(snapshot->selectedRun.has_value(),
            "manager telemetry includes selected run");
    require(snapshot->selectedRun->record.runId == runId,
            "manager telemetry preserves run identity");
    require(snapshot->context.retainedTokens == 4'096U,
            "manager telemetry preserves authoritative retained tokens");
    require(snapshot->context.inputTokens == 101U &&
                snapshot->context.outputTokens == 37U,
            "manager telemetry preserves provider token counts");
    require(snapshot->context.headroomTokens == 18'432U,
            "manager computes context headroom once");
    require(snapshot->context.authoritative,
            "retained context is marked authoritative");
    require(snapshot->provider.responseId ==
                snapshot->selectedRun->record.providerResponseId,
            "provider response identity comes from selected run");
    require(snapshot->continuity.canonicalResponseId ==
                snapshot->selectedRun->record.providerResponseId,
            "continuity uses the canonical response identity");
    require(!snapshot->storeHealthy.value &&
                snapshot->storeHealthy.availability ==
                    Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
            "missing optional store source stays explicitly unavailable");
    require(telemetry.sampleCalls == 1U && managedRuns->statusCalls == 1U,
            "telemetry and selected run are sampled once");

    Manager::ManagerRequestDispatcher unavailable{controller, clock};
    requireError(
        unavailable.dispatch(request(
            *clock, 77U, Manager::ManagerTelemetryRequest{std::nullopt})),
        Domain::ErrorCodes::InvalidRequest,
        "manager telemetry unavailable composition");
}

void testManagedRunDispatchAndIdentity()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    auto managedRuns = std::make_shared<FakeManagedRuns>();
    Manager::ManagerRequestDispatcher dispatcher{
        controller,
        clock,
        Manager::ManagerTransportLimits{},
        managedRuns};

    const auto runId = Domain::SessionId::parse(uuidText(700U)).value();
    const auto projectId = Domain::ProjectId::parse(uuidText(701U)).value();
    const auto clientId = Domain::ClientId::parse(uuidText(702U)).value();
    const auto started = dispatcher.dispatch(request(
        *clock,
        70U,
        Manager::ManagedRunStartRequest{
            runId, projectId, clientId, 12U, "Inspect this project."}));
    const auto* startedRun = responseValue<Domain::ManagedRunSnapshot>(started);
    require(startedRun != nullptr, "managed run start result");
    require(startedRun->record.runId == runId, "managed run start identity");
    require(managedRuns->lastStart.has_value(), "managed start forwarding");
    require(managedRuns->lastStart->projectId == projectId, "managed project forwarding");
    require(managedRuns->lastStart->clientId == clientId, "managed client forwarding");
    require(managedRuns->lastStart->authorityGeneration == 12U,
            "managed authority generation forwarding");
    require(managedRuns->lastStart->operationId == operationId(70U),
            "managed operation derives from request id");
    require(managedRuns->lastStart->correlationId == correlationId(70U),
            "managed correlation forwarding");
    require(managedRuns->lastContextOperation == operationId(70U),
            "managed context operation forwarding");

    const auto status = dispatcher.dispatch(request(
        *clock, 71U, Manager::ManagedRunStatusRequest{runId}));
    require(responseValue<Domain::ManagedRunSnapshot>(status) != nullptr,
            "managed run status result");
    const auto paused = dispatcher.dispatch(request(
        *clock, 72U, Manager::ManagedRunPauseRequest{runId}));
    const auto* pausedRun = responseValue<Domain::ManagedRunSnapshot>(paused);
    require(pausedRun != nullptr && pausedRun->pauseRequested,
            "managed run pause result");
    const auto resumed = dispatcher.dispatch(request(
        *clock, 73U, Manager::ManagedRunResumeRequest{runId}));
    const auto* resumedRun = responseValue<Domain::ManagedRunSnapshot>(resumed);
    require(resumedRun != nullptr &&
            resumedRun->record.state == Domain::ManagedRunState::Running,
            "managed run resume result");
    const auto cancelled = dispatcher.dispatch(request(
        *clock, 74U, Manager::ManagedRunCancelRequest{runId}));
    const auto* cancelledRun = responseValue<Domain::ManagedRunSnapshot>(cancelled);
    require(cancelledRun != nullptr && cancelledRun->cancellationRequested,
            "managed run cancellation result");
    require(managedRuns->startCalls == 1U && managedRuns->statusCalls == 1U &&
            managedRuns->pauseCalls == 1U && managedRuns->resumeCalls == 1U &&
            managedRuns->cancelCalls == 1U,
            "managed run method routing");

    Manager::ManagerRequestDispatcher unavailable{controller, clock};
    requireError(
        unavailable.dispatch(request(
            *clock, 75U, Manager::ManagedRunStatusRequest{runId})),
        Domain::ErrorCodes::InvalidRequest,
        "managed run unavailable composition");
}

void testRunHistoryIsBoundToSelectedProject()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    auto managedRuns = std::make_shared<FakeManagedRuns>();
    FakeOperationalSessions operational;
    const auto projectA = Domain::ProjectId::parse(uuidText(701U)).value();
    const auto projectB = Domain::ProjectId::parse(uuidText(702U)).value();
    const auto managedAgent = Domain::AgentId::parse("forge-managed-run").value();
    const auto otherAgent = Domain::AgentId::parse("other-agent").value();
    const auto time = Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}};
    const auto aOpen = Domain::SessionId::parse(uuidText(710U)).value();
    const auto bRecent = Domain::SessionId::parse(uuidText(711U)).value();
    const auto aRecent = Domain::SessionId::parse(uuidText(712U)).value();
    operational.listing.open.push_back({
        aOpen, managedAgent, std::nullopt, Domain::SessionStatus::Open,
        std::nullopt, time, time});
    operational.listing.recent.push_back({
        bRecent, managedAgent, std::nullopt, Domain::SessionStatus::Completed,
        std::nullopt, time, time});
    operational.listing.recent.push_back({
        aRecent, managedAgent, std::nullopt, Domain::SessionStatus::Completed,
        std::nullopt, time, time});
    operational.listing.recent.push_back({
        Domain::SessionId::parse(uuidText(713U)).value(), otherAgent,
        std::nullopt, Domain::SessionStatus::Completed, std::nullopt, time, time});
    managedRuns->projectByRun.emplace(bRecent.value(), projectB);
    Manager::ManagerTelemetrySources sources;
    sources.operational = &operational;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, managedRuns, sources};

    const auto aResponse = dispatcher.dispatch(request(*clock, 91U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Runs,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, projectA}));
    const auto* aHistory = responseValue<Manager::ManagerOperationalSnapshot>(aResponse);
    require(aHistory != nullptr && aHistory->lines.size() == 2U,
        "run history includes only selected project A");
    require(aHistory->lines[0].find(aOpen.value()) != std::string::npos &&
            aHistory->lines[1].find(aRecent.value()) != std::string::npos,
        "project A run identities are preserved");
    require(aHistory->lines[0].find(bRecent.value()) == std::string::npos &&
            aHistory->lines[1].find(bRecent.value()) == std::string::npos,
        "project B run identity never leaks into A");

    const auto bResponse = dispatcher.dispatch(request(*clock, 92U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Runs,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, projectB}));
    const auto* bHistory = responseValue<Manager::ManagerOperationalSnapshot>(bResponse);
    require(bHistory != nullptr && bHistory->lines.size() == 1U &&
            bHistory->lines[0].find(bRecent.value()) != std::string::npos,
        "project B history retains only its exact run");
    require(operational.sessionCalls == 2U,
        "one bounded session listing is read per authorized inspection");
    requireError(dispatcher.dispatch(request(*clock, 93U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Runs,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, std::nullopt})),
        Domain::ErrorCodes::InvalidRequest, "unbound run history");
    require(operational.sessionCalls == 2U,
        "unbound inspection does not read sessions");
    operational.allowStatus = true;
    managedRuns->stateByRun.emplace(aOpen.value(), Domain::ManagedRunState::Running);
    managedRuns->outputByRun.emplace(aRecent.value(), "OK from project A");
    managedRuns->outputByRun.emplace(bRecent.value(), "private project B result");
    const auto runtimeResponse = dispatcher.dispatch(request(*clock, 94U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Runtimes,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, projectA}));
    const auto* runtime = responseValue<Manager::ManagerOperationalSnapshot>(runtimeResponse);
    require(runtime != nullptr, "project-bound runtime inventory succeeds");
    const auto runtimeText = [&] {
        std::string text;
        for (const auto& line : runtime->lines) text += line + "\n";
        return text;
    }();
    require(runtimeText.find("Job inventory: 2 recent selected-project runs · 1 active · 1 completed") !=
            std::string::npos,
        "runtime jobs use exact persisted run states");
    require(runtimeText.find(aOpen.value()) != std::string::npos &&
            runtimeText.find(aRecent.value()) != std::string::npos &&
            runtimeText.find("Result · OK from project A") != std::string::npos,
        "runtime inventory projects selected-project identities and outcomes");
    require(runtimeText.find(bRecent.value()) == std::string::npos &&
            runtimeText.find("private project B result") == std::string::npos,
        "runtime inventory never projects another project's run or result");
    require(operational.sessionCalls == 3U,
        "runtime jobs read one bounded persisted session window");
}

void testDurableEvidenceIsRedactedAndProjectBound()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    FakeOperationalSessions operational;
    FakeDurableManagedRunStore durable;
    FakeEvidenceHasher hasher;
    const auto projectA = Domain::ProjectId::parse(uuidText(721U)).value();
    const auto projectB = Domain::ProjectId::parse(uuidText(722U)).value();
    const auto runA = Domain::SessionId::parse(uuidText(723U)).value();
    const auto runB = Domain::SessionId::parse(uuidText(724U)).value();
    const auto managedAgent = Domain::AgentId::parse("forge-managed-run").value();
    const auto time = Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}};
    operational.listing.recent.push_back({
        runA, managedAgent, std::nullopt, Domain::SessionStatus::Completed,
        std::nullopt, time, time});
    operational.listing.recent.push_back({
        runB, managedAgent, std::nullopt, Domain::SessionStatus::Completed,
        std::nullopt, time, time});
    Domain::ManagedRunRecord a{
        runA, projectA, Domain::ClientId::parse("evidence-fixture").value(),
        "private project A mission", 7U, Domain::ManagedRunState::Completed,
        Domain::ProviderSessionId::parse("resp_evidence_a").value(),
        20U, 4U, std::nullopt, std::string{"private project A model output"},
        std::nullopt, {}, time, time, false};
    a.evidenceSeal = Domain::Sha256Digest::parse(std::string(64U, 'b')).value();
    a.evidenceIntegrity = Domain::ManagedRunEvidenceIntegrity::Verified;
    durable.records.emplace(runA.value(), a);
    auto b = a;
    b.runId = runB;
    b.projectId = projectB;
    b.task = "private project B mission";
    b.outputText = "private project B model output";
    durable.records.emplace(runB.value(), b);
    Manager::ManagerTelemetrySources sources;
    sources.operational = &operational;
    sources.durableManagedRunStore = &durable;
    sources.evidenceHasher = &hasher;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    const auto responseA = dispatcher.dispatch(request(*clock, 95U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Evidence,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, projectA}));
    const auto* evidenceA = responseValue<Manager::ManagerOperationalSnapshot>(
        responseA);
    require(evidenceA && evidenceA->lines.size() == 1U,
        "evidence returns one exact-project durable run");
    const auto& row = evidenceA->lines.front();
    require(row.find("\"run_id\":\"" + runA.value() + "\"") !=
                std::string::npos &&
            row.find("\"project_id\":\"" + projectA.value() + "\"") !=
                std::string::npos &&
            row.find("\"native_record_integrity\":\"verified\"") !=
                std::string::npos &&
            row.find("\"task_outcome_verification\":\"not_configured\"") !=
                std::string::npos,
        "evidence carries native provenance without claiming task success");
    require(row.find("\"task_sha256\":\"" + std::string(64U, 'a') +
                "\"") != std::string::npos &&
            row.find("\"stored_output_sha256\":\"" +
                std::string(64U, 'a') + "\"") != std::string::npos &&
            row.find("private project A") == std::string::npos &&
            row.find(runB.value()) == std::string::npos,
        "redacted evidence omits mission and model text and foreign identity");
    requireError(dispatcher.dispatch(request(*clock, 96U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Evidence,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, std::nullopt})),
        Domain::ErrorCodes::InvalidRequest, "evidence needs exact project");
    require(operational.sessionCalls == 1U,
        "unbound evidence never reads the session window");
}

void testProjectWorkflowKeepsExactProjectIdentity()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const auto projectA = Domain::ProjectId::parse(uuidText(801U)).value();
    const auto projectB = Domain::ProjectId::parse(uuidText(802U)).value();
    const Domain::ProjectMemoryDescriptor descriptorA{
        projectA, "Project A", std::nullopt,
        {Domain::PathText::create("D:\\Projects\\A").value()}};
    const Domain::ProjectMemoryDescriptor descriptorB{
        projectB, "Project B", std::nullopt,
        {Domain::PathText::create("D:\\Projects\\B").value()}};
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(descriptorA)), "seed project A");
    require(static_cast<bool>(registry.seedDescriptor(descriptorB)), "seed project B");
    TestFakes::RecordingProjectMemoryService memory;

    const auto statusFor = [](const Domain::ProjectId& projectId) {
        return Domain::ProjectMemoryStatus{
            projectId, 1U, 1U, 2U, 0U, 3U, 4'096U, 128U,
            false, true, 1U, {}};
    };
    const auto pageFor = [](const Domain::ProjectId& projectId) {
        return Domain::MemoryPage{
            projectId, {}, std::nullopt, false, 0U, 256U * 1024U, 1U, 1U};
    };

    Manager::ManagerRequestDispatcher dispatcher{
        controller,
        clock,
        Manager::ManagerTransportLimits{},
        {},
        Manager::ManagerTelemetrySources{
            nullptr, nullptr, &registry, &memory, nullptr}};

    const auto listed = dispatcher.dispatch(request(
        *clock, 80U, Manager::ManagerProjectsListRequest{10U}));
    const auto* projects = responseValue<Manager::ManagerProjectsSnapshot>(listed);
    require(projects != nullptr && projects->projects.size() == 2U,
            "project list retains both identities");

    memory.statusResult.set(
        Domain::Result<Domain::ProjectMemoryStatus>::success(statusFor(projectB)));
    memory.searchResult.set(
        Domain::Result<Domain::MemoryPage>::success(pageFor(projectB)));
    const auto selectedB = dispatcher.dispatch(request(
        *clock,
        81U,
        Manager::ManagerProjectMemoryRequest{projectB, "decision", 20U}));
    const auto* workspaceB =
        responseValue<Manager::ManagerProjectWorkspaceSnapshot>(selectedB);
    require(workspaceB != nullptr && workspaceB->project.id == projectB,
            "project B selection returns project B");
    require(memory.lastProjectId() == projectB,
            "project B memory is routed with project B identity");

    memory.searchResult.set(
        Domain::Result<Domain::MemoryPage>::success(pageFor(projectA)));
    requireError(
        dispatcher.dispatch(request(
            *clock,
            82U,
            Manager::ManagerProjectMemoryRequest{projectB, "decision", 20U})),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "cross-project memory response");

    memory.statusResult.set(
        Domain::Result<Domain::ProjectMemoryStatus>::success(statusFor(projectA)));
    memory.searchResult.set(
        Domain::Result<Domain::MemoryPage>::success(pageFor(projectA)));
    const auto selectedA = dispatcher.dispatch(request(
        *clock,
        83U,
        Manager::ManagerProjectMemoryRequest{projectA, "decision", 20U}));
    const auto* workspaceA =
        responseValue<Manager::ManagerProjectWorkspaceSnapshot>(selectedA);
    require(workspaceA != nullptr && workspaceA->project.id == projectA,
            "project A selection remains isolated from project B");
}

void testMaintenanceRequiresExactScopeAndCoordinatesStores()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const auto project = Domain::ProjectId::parse(uuidText(850U)).value();
    const Domain::ProjectMemoryDescriptor descriptor{
        project, "Maintenance project", std::nullopt,
        {Domain::PathText::create("D:\\Projects\\Maintenance").value()}};
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(descriptor)),
        "seed maintenance project");
    TestFakes::RecordingProjectMemoryService memory;
    TestFakes::RecordingContinuityCoordinator continuity;
    memory.resetProjectMemoryResult.set(
        Domain::Result<Domain::ResetReport>::success(
            Domain::ResetReport{
                "reset_project_memory", project.value(), 1U, 2U, 3U, 4U, true}));
    continuity.resetResult.set(
        Domain::Result<Domain::ContinuityResetReport>::success(
            Domain::ContinuityResetReport{
                project,
                Domain::ResetReport{
                    "reset_project_continuity", project.value(),
                    1U, 5U, 6U, 7U, true}}));

    Manager::ManagerTelemetrySources sources;
    sources.projects = &registry;
    sources.projectMemory = &memory;
    sources.continuity = &continuity;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    requireError(
        dispatcher.dispatch(request(
            *clock, 84U,
            Manager::ManagerMaintenanceRequest{
                Manager::ManagerMaintenanceScope::ProjectAllData,
                project, "wrong confirmation"})),
        Domain::ErrorCodes::Unauthorized,
        "combined reset rejects wrong confirmation");
    require(memory.callCount(TestFakes::ProjectMemoryCall::ResetProjectMemory) == 0U,
        "wrong confirmation leaves memory unchanged");
    require(continuity.callCount(
        TestFakes::ContinuityCall::ResetProjectContinuity) == 0U,
        "wrong confirmation leaves continuity unchanged");

    const auto completed = dispatcher.dispatch(request(
        *clock, 85U,
        Manager::ManagerMaintenanceRequest{
            Manager::ManagerMaintenanceScope::ProjectAllData,
            project, "RESET PROJECT DATA " + project.value()}));
    const auto* snapshot = responseValue<Manager::ManagerMaintenanceSnapshot>(completed);
    require(snapshot != nullptr && snapshot->verified,
        "combined reset returns verified snapshot");
    require(snapshot->affectedScope == project.value() &&
            snapshot->projectsAffected == 1U,
        "combined reset retains exact project scope");
    require(snapshot->recordsRemoved == 7U && snapshot->linksRemoved == 9U &&
            snapshot->eventsRemoved == 11U,
        "combined reset aggregates both store reports");
    require(memory.callCount(TestFakes::ProjectMemoryCall::ResetProjectMemory) == 1U,
        "combined reset invokes memory once");
    require(continuity.callCount(
        TestFakes::ContinuityCall::ResetProjectContinuity) == 1U,
        "combined reset invokes continuity once");
}

void testPayloadMappingAndControllerFailures()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    Manager::ManagerRequestDispatcher dispatcher{controller, clock};

    const auto status = dispatcher.dispatch(request(
        *clock, 1U, Manager::ManagerStatusRequest{}));
    require(responseValue<Domain::ManagerStatus>(status) != nullptr, "status result");

    const auto settings = dispatcher.dispatch(request(
        *clock, 2U, Manager::ManagerSettingsRequest{}));
    require(
        responseValue<Domain::ManagerSettings>(settings) != nullptr,
        "settings result");

    const auto control = dispatcher.dispatch(request(
        *clock,
        3U,
        Domain::ManagerControlRequest{Domain::ManagerControlAction::Restart}));
    require(responseValue<Domain::ManagerStatus>(control) != nullptr, "control result");
    require(
        controller->lastControlAction_ == Domain::ManagerControlAction::Restart,
        "control payload forwarding");

    Domain::ManagerSettingsPatch patch;
    patch.dashboardPort = static_cast<std::uint16_t>(8888U);
    const auto update = dispatcher.dispatch(request(
        *clock,
        4U,
        Manager::ManagerSettingsUpdateRequest{patch, true}));
    const auto* updateOutcome =
        responseValue<Domain::ManagerSettingsUpdateOutcome>(update);
    require(updateOutcome != nullptr, "settings update outcome result");
    require(
        responseValue<Domain::ManagerSettings>(update) == nullptr,
        "settings update must not collapse to the settings GET result type");
    require(updateOutcome->settings.dashboardPort == 8888U, "updated settings");
    require(updateOutcome->applied, "settings update applied metadata");
    require(updateOutcome->bindingChanged, "settings binding metadata");
    require(
        updateOutcome->status.dashboardPort == 8888U,
        "settings update status metadata");
    require(
        controller->lastPatch_.dashboardPort == 8888,
        "settings patch forwarding");
    require(controller->lastApplyImmediately_, "settings apply forwarding");

    controller->failStatus_ = true;
    requireError(
        dispatcher.dispatch(request(*clock, 5U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::DatabaseBusy,
        "controller failure mapping");
}

void testDuplicateCapacityAndCancellationBypass()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    controller->setBlocking(true);
    Manager::ManagerRequestDispatcher dispatcher{controller, clock};

    std::vector<Manager::ManagerResponse> responses;
    responses.reserve(3U);
    std::mutex responseMutex;
    std::vector<std::jthread> workers;
    workers.reserve(3U);
    for (std::uint32_t suffix = 10U; suffix < 13U; ++suffix) {
        workers.emplace_back([&, suffix] {
            auto response = dispatcher.dispatch(request(
                *clock, suffix, Manager::ManagerStatusRequest{}));
            std::lock_guard lock{responseMutex};
            responses.push_back(std::move(response));
        });
    }
    require(controller->waitForActive(3U), "three requests must become active");
    require(dispatcher.activeOperationCount() == 3U, "active count bound");

    requireError(
        dispatcher.dispatch(request(*clock, 10U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::Conflict,
        "duplicate request");
    requireError(
        dispatcher.dispatch(request(*clock, 13U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::LimitExceeded,
        "capacity request");

    const auto cancelResponse = dispatcher.dispatch(request(
        *clock, 10U, Manager::ManagerCancelRequest{operationId(10U)}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(cancelResponse) != nullptr,
        "cancel acknowledgement");
    const auto absentCancel = dispatcher.dispatch(request(
        *clock, 99U, Manager::ManagerCancelRequest{operationId(99U)}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(absentCancel) != nullptr,
        "absent cancel must be idempotent");

    controller->setBlocking(false);
    workers.clear();
    require(dispatcher.waitUntilIdle(2s), "cancelled requests must drain");
    require(responses.size() == 3U, "all admitted requests returned");
}

void testShutdownOrderingAndClosedAdmission()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    controller->setBlocking(true);
    Manager::ManagerTransportLimits limits;
    limits.shutdownDrainTimeout = 2s;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock, limits};

    std::optional<Manager::ManagerResponse> activeResponse;
    std::jthread active{[&] {
        activeResponse = dispatcher.dispatch(request(
            *clock, 20U, Manager::ManagerStatusRequest{}));
    }};
    require(controller->waitForActive(1U), "shutdown fixture active request");

    const auto shutdown = dispatcher.dispatch(request(
        *clock, 21U, Manager::ManagerShutdownRequest{}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(shutdown) != nullptr,
        "shutdown acknowledgement");
    active.join();
    require(activeResponse.has_value(), "active shutdown response");
    requireError(
        *activeResponse,
        Domain::ErrorCodes::Cancelled,
        "shutdown cancellation response");
    require(!dispatcher.isAccepting(), "shutdown closes regular admission");
    require(controller->requestShutdownCalls() == 1U, "shutdown controller call");

    const auto events = controller->events();
    const auto exit = std::find(events.begin(), events.end(), "status_exit");
    const auto requested =
        std::find(events.begin(), events.end(), "request_shutdown");
    require(exit != events.end(), "active exit event");
    require(requested != events.end(), "request shutdown event");
    require(exit < requested, "active cancellation must drain before shutdown request");

    requireError(
        dispatcher.dispatch(request(*clock, 22U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::TransportClosed,
        "regular work after shutdown");
    const auto cancel = dispatcher.dispatch(request(
        *clock, 22U, Manager::ManagerCancelRequest{operationId(22U)}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(cancel) != nullptr,
        "cancel remains available after shutdown admission closes");
}

void testShutdownFailureAndEnvelopeValidation()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    controller->failRequestShutdown_ = true;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock};

    auto unsupported = request(*clock, 30U, Manager::ManagerStatusRequest{});
    unsupported.version = 2U;
    requireError(
        dispatcher.dispatch(unsupported),
        Domain::ErrorCodes::UnsupportedVersion,
        "unsupported version");

    auto nonUuid = request(*clock, 31U, Manager::ManagerStatusRequest{});
    nonUuid.requestId = Domain::RequestId::parse("opaque-but-not-uuid").value();
    requireError(
        dispatcher.dispatch(nonUuid),
        Domain::ErrorCodes::InvalidRequest,
        "non-UUID request identifier");

    auto expired = request(*clock, 32U, Manager::ManagerStatusRequest{});
    expired.deadlineUtcMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock->utc.time_since_epoch())
            .count();
    requireError(
        dispatcher.dispatch(expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "expired envelope");

    requireError(
        dispatcher.dispatch(request(
            *clock, 33U, Manager::ManagerShutdownRequest{})),
        Domain::ErrorCodes::InternalFailure,
        "shutdown controller failure");
    require(!dispatcher.isAccepting(), "failed shutdown still closes admission");
}

void testBoundedCloseDefersControllerShutdownUntilIdle()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const std::weak_ptr<FakeClock> weakClock = clock;
    const std::weak_ptr<FakeController> weakController = controller;
    controller->setBlocking(true);
    controller->setIgnoreCancellation(true);
    Manager::ManagerTransportLimits limits;
    limits.shutdownDrainTimeout = 1ms;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock, limits};

    std::jthread active{[&] {
        static_cast<void>(dispatcher.dispatch(request(
            *clock, 40U, Manager::ManagerStatusRequest{})));
    }};
    require(controller->waitForActive(1U), "bounded close active request");
    dispatcher.shutdown();
    require(
        controller->closeCalls() == 0U,
        "controller close may not race an active callback");
    controller.reset();
    clock.reset();
    require(
        !weakController.expired() && !weakClock.expired(),
        "dispatcher must retain dependencies across a bounded shutdown");
    auto retainedController = weakController.lock();
    require(retainedController != nullptr, "retained controller lifetime");
    retainedController->setBlocking(false);
    active.join();
    require(dispatcher.waitUntilIdle(2s), "bounded close final drain");
    require(
        retainedController->closeCalls() == 1U,
        "last release must close the controller exactly once");
    dispatcher.shutdown();
    require(
        retainedController->closeCalls() == 1U,
        "close must remain idempotent");
}

void testRacingReleaseAndShutdownClosesExactlyOnce()
{
    constexpr std::size_t Iterations = 128U;
    for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
        auto clock = std::make_shared<FakeClock>();
        auto controller = std::make_shared<FakeController>();
        controller->setBlocking(true);
        controller->setIgnoreCancellation(true);
        Manager::ManagerTransportLimits limits;
        limits.shutdownDrainTimeout = 0ms;
        Manager::ManagerRequestDispatcher dispatcher{controller, clock, limits};

        std::jthread active{[&] {
            static_cast<void>(dispatcher.dispatch(request(
                *clock,
                static_cast<std::uint32_t>(100U + iteration),
                Manager::ManagerStatusRequest{})));
        }};
        require(controller->waitForActive(1U), "racing close active request");

        std::atomic_bool start{};
        std::jthread shutdown{[&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            dispatcher.shutdown();
        }};
        std::jthread release{[&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            controller->setBlocking(false);
        }};
        start.store(true, std::memory_order_release);
        shutdown.join();
        release.join();
        active.join();

        require(dispatcher.waitUntilIdle(2s), "racing close final drain");
        require(
            controller->closeCalls() == 1U,
            "a release racing the bounded shutdown must close exactly once");

        std::vector<std::jthread> repeatedShutdowns;
        repeatedShutdowns.reserve(4U);
        for (std::size_t caller{}; caller < 4U; ++caller) {
            repeatedShutdowns.emplace_back([&] { dispatcher.shutdown(); });
        }
        repeatedShutdowns.clear();
        require(
            controller->closeCalls() == 1U,
            "concurrent repeated shutdown must not close more than once");
    }
}

void testConstructionRejectsNullDependencies()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();

    bool rejectedController{};
    try {
        Manager::ManagerRequestDispatcher dispatcher{
            std::shared_ptr<Contracts::IManagerController>{}, clock};
        static_cast<void>(dispatcher.isAccepting());
    } catch (const std::invalid_argument&) {
        rejectedController = true;
    }
    require(rejectedController, "null controller construction");

    bool rejectedClock{};
    try {
        Manager::ManagerRequestDispatcher dispatcher{
            controller, std::shared_ptr<Contracts::IClock>{}};
        static_cast<void>(dispatcher.isAccepting());
    } catch (const std::invalid_argument&) {
        rejectedClock = true;
    }
    require(rejectedClock, "null clock construction");
}

} // namespace

int main()
{
    try {
        testPayloadMappingAndControllerFailures();
        testManagedRunDispatchAndIdentity();
        testRunHistoryIsBoundToSelectedProject();
        testDurableEvidenceIsRedactedAndProjectBound();
        testProjectWorkflowKeepsExactProjectIdentity();
        testMaintenanceRequiresExactScopeAndCoordinatesStores();
        testTelemetrySnapshotUsesManagerOwnedRunValues();
        testDuplicateCapacityAndCancellationBypass();
        testShutdownOrderingAndClosedAdmission();
        testShutdownFailureAndEnvelopeValidation();
        testBoundedCloseDefersControllerShutdownUntilIdle();
        testRacingReleaseAndShutdownClosesExactlyOnce();
        testConstructionRejectsNullDependencies();
        std::cout << "Manager request dispatcher tests passed: 12 groups\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Manager request dispatcher tests failed: "
                  << failure.what() << '\n';
        return 1;
    }
}
