#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"
#include "ForgeConductor/Application/ProjectMemoryService.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "../Fakes/DiagnosticsFakes.h"
#include "../Fakes/PlatformPathFakes.h"
#include "../Persistence/PersistenceTestSupport.h"
#include "../Fakes/ProjectRepositoryFakes.h"
#include "../Fakes/RecordingProjectMemoryService.h"
#include "../Fakes/RecordingContinuityCoordinator.h"
#include "../Fakes/DeterministicWorkspaceAuthority.h"
#include "../Fakes/ExternalServiceFakes.h"
#include "../Fakes/ToolServiceFakes.h"
#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
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
        if (failSettings_) {
            return Domain::Result<Domain::ManagerSettings>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "injected settings failure",
                    true));
        }
        return Domain::Result<Domain::ManagerSettings>::success(currentSettings);
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
        auto settings = currentSettings;
        if (patch.dashboardPort) {
            settings.dashboardPort = *patch.dashboardPort;
        }
        currentSettings = settings;
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
    bool failSettings_{};
    bool failRequestShutdown_{};
    std::atomic_size_t statusCalls_{};
    std::atomic_size_t settingsCalls_{};
    std::atomic_size_t controlCalls_{};
    std::atomic_size_t updateCalls_{};
    Domain::ManagerControlAction lastControlAction_{};
    Domain::ManagerSettingsPatch lastPatch_;
    bool lastApplyImmediately_{};
    Domain::ManagerSettings currentSettings{settingsValue()};

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

class FixedHasher final : public Contracts::IHasher {
public:
    explicit FixedHasher(Domain::Sha256Digest digest)
        : digest_{std::move(digest)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        const std::span<const std::byte> bytes) noexcept override
    {
        ++calls;
        lastByteCount = bytes.size();
        return Domain::Result<Domain::Sha256Digest>::success(digest_);
    }

    std::size_t calls{};
    std::size_t lastByteCount{};

private:
    Domain::Sha256Digest digest_;
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

class FakeNativeCheckToolRouter final : public Contracts::IToolRouter {
public:
    int exitCode{};
    std::size_t calls{};
    std::string lastCommand;
    std::string lastArguments;
    std::vector<Domain::PathText> lastRoots;
    std::vector<Domain::FileAccess> lastGrants;
    std::vector<Domain::FileAccess> lastDenials;
    bool lastShell{};
    std::uint64_t lastGeneration{};
    Domain::FileAccess lastIntent{Domain::FileAccess::Read};
    Domain::ProjectId lastProject = Domain::ProjectId::parse(uuidText(1U)).value();

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& call,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext&) noexcept override
    {
        ++calls;
        lastProject = authority.projectId();
        lastArguments = call.canonicalArguments;
        lastRoots = authority.trustedRoots();
        lastGrants = authority.grants();
        lastDenials = authority.denials();
        lastShell = authority.shellEnabled();
        lastGeneration = authority.generation();
        lastIntent = authority.intent();
        lastCommand = call.canonicalArguments.find("Write-Output OK") !=
            std::string::npos ? "Write-Output OK" : "exit 1";
        const auto payload = std::string{"{\"ok\":"} +
            (exitCode == 0 ? "true" : "false") +
            ",\"command\":\"" + lastCommand +
            "\",\"exit_code\":" + std::to_string(exitCode) +
            ",\"stdout\":\"native stdout\",\"stderr\":\"" +
            (exitCode == 0 ? "" : "native stderr") +
            "\",\"timed_out\":false,\"cancelled\":false," +
            "\"termination_confirmed\":true,\"elapsed_ms\":13}";
        return Domain::Result<Domain::ToolCallOutcome>::success(
            Domain::ToolCallOutcome{
                Domain::ToolExecutionReceipt{
                    call.metadata.requestId, call.toolName, exitCode == 0,
                    std::nullopt, 13ms},
                payload, std::nullopt, std::nullopt});
    }
    void cancel(const Domain::OperationId&) noexcept override {}
    void shutdown() noexcept override {}
};

class FakeOperationalSessions final : public Dashboard::IDashboardOperationalService {
public:
    Dashboard::DashboardSessionListing listing;
    std::size_t sessionCalls{};
    bool allowStatus{};
    bool allowDoctor{};
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
        if (allowDoctor) {
            const auto root = Domain::PathText::create("D:\\DoctorFixture").value();
            return Domain::Result<Domain::DoctorReport>::success(
                Domain::DoctorReport{true, "1.3.6", root,
                    {Domain::DoctorCheck{"manager_ipc", true, "connected", true}},
                    {}, true, root});
        }
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

class FakeProjectPolicy final : public Contracts::IProjectPolicyService {
public:
    [[nodiscard]] Domain::Result<void> check(
        const Domain::ToolAuthorizationRequest&,
        const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<std::string> execute(
        const Contracts::ProjectPolicyRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        if (request.action == Contracts::ProjectPolicyAction::ListFindings) {
            return Domain::Result<std::string>::success(nlohmann::json{
                {"revision", std::string(64U, 'e')},
                {"findings", nlohmann::json::array()},
                {"notifications", nlohmann::json::array()},
                {"activity", nlohmann::json::array({{
                    {"kind", "finding_resolved"},
                    {"finding_id", "finding-fixture"},
                    {"correlation_id", "clu-correlation"},
                    {"timestamp_utc_ms", 1'700'000'000'000LL}}})}}.dump());
        }
        if (request.action == Contracts::ProjectPolicyAction::ExportLog) {
            return Domain::Result<std::string>::success(nlohmann::json{
                {"schema", "forge-clu-governance-log-v1"},
                {"findings", nlohmann::json::array()},
                {"notifications", nlohmann::json::array()},
                {"history", nlohmann::json::array()}}.dump());
        }
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "unexpected policy action"));
    }
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

void testWorkerScopeBrokerPreservesOnlyCurrentCallerGrants()
{
    using Json = nlohmann::json;
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const auto project = Domain::ProjectId::parse(uuidText(900U)).value();
    const auto first = Domain::PathText::create("D:\\WorkerFirst").value();
    const auto second = Domain::PathText::create("D:\\WorkerSecond").value();
    TestFakes::DeterministicWorkspaceAuthority issuer{
        Domain::AuthorityId::parse(uuidText(901U)).value(), Domain::ClientId::parse("worker-scope-fixture").value(),
        {first, second}, Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create, Domain::FileAccess::Delete, Domain::FileAccess::Execute},
        {}, true, 7U};
    FakeNativeCheckToolRouter router;
    Manager::ManagerTelemetrySources sources;
    sources.projectWorkspaceAuthority = &issuer; sources.toolRouter = &router;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock, Manager::ManagerTransportLimits{}, {}, sources};
    unsigned sequence = 902U;
    const auto invoke = [&](std::string_view tool, Json input) {
        return dispatcher.dispatch(request(*clock, sequence++, Manager::ManagerToolInvokeRequest{project, std::string{tool}, input.dump()}));
    };
    Json scope{{"trusted_roots", Json::array({first.value()})}, {"grants", Json::array({"read"})},
        {"denials", Json::array()}, {"shell_enabled", false}};
    for (const auto tool : {"agent_spawn", "agent_poll", "agent_cancel", "schedule_create", "schedule_list", "schedule_cancel", "schedule_run_now"}) {
        const auto response = invoke(tool, Json{{"marker", "preserve public args"}, {"_forge_worker_scope", scope}});
        require(responseValue<Manager::ManagerToolOutcomeSnapshot>(response) != nullptr,
            "Worker tool scope could not be admitted by its broker.");
        require(router.lastRoots == std::vector<Domain::PathText>{first} && router.lastGrants == std::vector<Domain::FileAccess>{Domain::FileAccess::Read} &&
            router.lastIntent == Domain::FileAccess::Read && !router.lastShell && router.lastGeneration == 8U,
            "Read-only worker scope restored removed roots, Write/Execute grants, shell, or the original intent.");
        const auto arguments = Json::parse(router.lastArguments);
        require(!arguments.contains("_forge_worker_scope") && arguments.at("marker") == "preserve public args",
            "Broker metadata leaked through public tool schema validation or erased public arguments.");
    }
    auto denied = scope;
    denied["grants"] = Json::array({"read", "write", "create", "delete", "execute"});
    denied["denials"] = Json::array({"write", "execute"}); denied["shell_enabled"] = true;
    require(responseValue<Manager::ManagerToolOutcomeSnapshot>(invoke("agent_poll", Json{{"_forge_worker_scope", denied}})) != nullptr,
        "Restrictive worker denials were not admitted.");
    require(std::find(router.lastGrants.begin(), router.lastGrants.end(), Domain::FileAccess::Write) == router.lastGrants.end() &&
        std::find(router.lastGrants.begin(), router.lastGrants.end(), Domain::FileAccess::Execute) == router.lastGrants.end() && !router.lastShell,
        "Caller denial did not remove effective Write/Execute grants and shell access.");
    auto full = scope;
    full["trusted_roots"] = Json::array({first.value(), second.value(), "Z:\\Unconfigured"});
    full["grants"] = Json::array({"read", "write", "create", "delete", "execute"}); full["shell_enabled"] = false;
    require(responseValue<Manager::ManagerToolOutcomeSnapshot>(invoke("schedule_list", Json{{"_forge_worker_scope", full}})) != nullptr &&
        router.lastRoots == std::vector<Domain::PathText>{first, second} && !router.lastShell,
        "Worker scope added an unconfigured root or restored caller-disabled shell.");
    const auto invalid = [&](Json metadata, std::string_view code) {
        const auto calls = router.calls;
        requireError(invoke("agent_poll", Json{{"_forge_worker_scope", std::move(metadata)}}), code, "Invalid worker scope");
        require(router.calls == calls, "Invalid worker metadata reached the tool router.");
    };
    auto bad = scope; bad["trusted_roots"] = Json::array({"Z:\\Unconfigured"}); invalid(bad, Domain::ErrorCodes::Unauthorized);
    bad = scope; bad["trusted_roots"] = Json::array({first.value(), first.value()}); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["trusted_roots"] = Json::array(); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["trusted_roots"] = Json::array({17}); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["grants"] = Json::array({"read", "read"}); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["grants"] = Json::array({"admin"}); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["grants"] = Json::array(); invalid(bad, Domain::ErrorCodes::Unauthorized);
    bad = scope; bad["denials"] = Json::array({"read"}); invalid(bad, Domain::ErrorCodes::Unauthorized);
    bad = scope; bad["shell_enabled"] = "false"; invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["unknown"] = false; invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["denials"] = Json::array({"execute", "execute"}); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["denials"] = Json::array({"elevate"}); invalid(bad, Domain::ErrorCodes::InvalidRequest);
    bad = scope; bad["trusted_roots"] = Json::array();
    for (unsigned index = 0; index < 33U; ++index) bad["trusted_roots"].push_back("D:\\ExcessRoot" + std::to_string(index));
    invalid(bad, Domain::ErrorCodes::InvalidRequest);
    const auto previousCalls = router.calls;
    const auto duplicateArguments = std::string{"{\"_forge_worker_scope\":"} + scope.dump() + ",\"_forge_worker_scope\":" + scope.dump() + "}";
    requireError(dispatcher.dispatch(request(*clock, sequence++, Manager::ManagerToolInvokeRequest{project, "agent_poll", duplicateArguments})),
        Domain::ErrorCodes::InvalidRequest, "Duplicate broker scope field was accepted");
    require(router.calls == previousCalls, "Duplicate broker fields reached the router.");
    requireError(invoke("process_launch", Json{{"_forge_worker_scope", scope}}), Domain::ErrorCodes::InvalidRequest,
        "Unrelated tool accepted private worker metadata");
    require(router.calls == previousCalls, "Unrelated worker metadata reached the router.");
    require(responseValue<Manager::ManagerToolOutcomeSnapshot>(invoke("agent_poll", Json{{"no_scope", true}})) != nullptr &&
        router.lastRoots == std::vector<Domain::PathText>{first, second} && router.lastShell && router.lastIntent == Domain::FileAccess::Write && router.lastGeneration == 7U,
        "Native owner invocation without forwarded metadata did not retain its baseline.");

    const std::vector<Domain::FileAccess> currentDenials{Domain::FileAccess::Write, Domain::FileAccess::Create, Domain::FileAccess::Delete, Domain::FileAccess::Execute};
    TestFakes::DeterministicWorkspaceAuthority restrictedIssuer{
        Domain::AuthorityId::parse(uuidText(950U)).value(), Domain::ClientId::parse("worker-restricted-fixture").value(),
        {first}, Domain::FileAccess::Read, {Domain::FileAccess::Read}, currentDenials, false, 10U};
    sources.projectWorkspaceAuthority = &restrictedIssuer;
    Manager::ManagerRequestDispatcher restrictedDispatcher{controller, clock, Manager::ManagerTransportLimits{}, {}, sources};
    full["shell_enabled"] = true;
    const auto restrictedResponse = restrictedDispatcher.dispatch(request(*clock, sequence++,
        Manager::ManagerToolInvokeRequest{project, "agent_spawn", Json{{"_forge_worker_scope", full}}.dump()}));
    require(responseValue<Manager::ManagerToolOutcomeSnapshot>(restrictedResponse) != nullptr &&
        router.lastRoots == std::vector<Domain::PathText>{first} && router.lastGrants == std::vector<Domain::FileAccess>{Domain::FileAccess::Read} &&
        router.lastDenials == currentDenials && !router.lastShell && router.lastIntent == Domain::FileAccess::Read && router.lastGeneration == 11U,
        "Forwarded full scope widened the current issuer's revoked root, grants, denials, or shell restriction.");
}

void testTelemetryCannotReadRemovedManagedRuns()
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
        *clock, 76U, Manager::ManagerTelemetryRequest{std::nullopt}));
    const auto* snapshot = responseValue<Domain::ManagerTelemetrySnapshot>(response);
    require(snapshot != nullptr, "manager telemetry result");
    require(snapshot->resources.cpuPercent.value == 33.5,
            "manager telemetry reuses native CPU sample");
    require(snapshot->resources.history.size() == 1U,
            "manager telemetry reuses bounded resource history");
    require(!snapshot->selectedRun && !snapshot->context.retainedTokens &&
        !snapshot->context.authoritative && !snapshot->provider.responseId &&
        !snapshot->continuity.runId, "resource telemetry does not project a Managed Run or invent chat usage");
    requireError(dispatcher.dispatch(request(*clock, 78U,
        Manager::ManagerTelemetryRequest{runId})), Domain::ErrorCodes::InvalidRequest,
        "removed run cannot be read through telemetry");
    require(!snapshot->storeHealthy.value &&
                snapshot->storeHealthy.availability ==
                    Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
            "missing optional store source stays explicitly unavailable");
    require(telemetry.sampleCalls == 2U && managedRuns->statusCalls == 0U,
            "resource telemetry never invokes Managed Run");

    Manager::ManagerRequestDispatcher unavailable{controller, clock};
    requireError(
        unavailable.dispatch(request(
            *clock, 77U, Manager::ManagerTelemetryRequest{std::nullopt})),
        Domain::ErrorCodes::InvalidRequest,
        "manager telemetry unavailable composition");
}

void testManagedRunEndpointsAreRemoved()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    auto managedRuns = std::make_shared<FakeManagedRuns>();
    TestFakes::RecordingProjectMemoryService memory;
    const auto projectId = Domain::ProjectId::parse(uuidText(701U)).value();
    memory.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
        Domain::MemoryPage{
            projectId, {}, std::nullopt, false, 0U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    Manager::ManagerTelemetrySources sources;
    sources.projectMemory = &memory;
    Manager::ManagerRequestDispatcher dispatcher{
        controller,
        clock,
        Manager::ManagerTransportLimits{},
        managedRuns,
        sources};

    const auto runId = Domain::SessionId::parse(uuidText(700U)).value();
    const auto clientId = Domain::ClientId::parse(uuidText(702U)).value();
    const auto started = dispatcher.dispatch(request(
        *clock,
        70U,
        Manager::ManagedRunStartRequest{
            runId, projectId, clientId, 12U, "Inspect this project."}));
    requireError(started, Domain::ErrorCodes::InvalidRequest, "removed run start rejected");
    requireError(dispatcher.dispatch(request(*clock, 71U, Manager::ManagedRunStatusRequest{runId})), Domain::ErrorCodes::InvalidRequest, "removed run status rejected");
    requireError(dispatcher.dispatch(request(*clock, 72U, Manager::ManagedRunPauseRequest{runId})), Domain::ErrorCodes::InvalidRequest, "removed run pause rejected");
    requireError(dispatcher.dispatch(request(*clock, 73U, Manager::ManagedRunResumeRequest{runId})), Domain::ErrorCodes::InvalidRequest, "removed run resume rejected");
    requireError(dispatcher.dispatch(request(*clock, 74U, Manager::ManagedRunCancelRequest{runId})), Domain::ErrorCodes::InvalidRequest, "removed run cancel rejected");
    require(managedRuns->startCalls == 0U && managedRuns->statusCalls == 0U && managedRuns->pauseCalls == 0U && managedRuns->resumeCalls == 0U && managedRuns->cancelCalls == 0U, "removed endpoints never reach run service");
}

void testAutomaticContinuityPreferenceIsProjectProviderScopedAndDurable()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    auto managedRuns = std::make_shared<FakeManagedRuns>();
    TestFakes::RecordingProjectMemoryService memory;
    const auto project = Domain::ProjectId::parse(uuidText(703U)).value();
    const auto digest = Domain::Sha256Digest::parse(std::string(64U, 'c')).value();
    const auto firstRecordId = Domain::MemoryRecordId::parse(uuidText(704U)).value();
    const auto secondRecordId = Domain::MemoryRecordId::parse(uuidText(705U)).value();
    const auto writeOutcome = [&](const Domain::MemoryRecordId& id) {
        return Domain::MemoryWriteOutcome{
            project, id, 1U, Domain::MemoryWriteDisposition::Inserted,
            digest, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion};
    };
    memory.rememberResult.set(
        Domain::Result<Domain::MemoryWriteOutcome>::success(
            writeOutcome(firstRecordId)));
    Manager::ManagerTelemetrySources sources;
    sources.projectMemory = &memory;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{},
        managedRuns, sources};

    controller->currentSettings.localModelName = "provider-a";
    const auto savedA = dispatcher.dispatch(request(*clock, 760U,
        Manager::ManagerAutomaticContinuityPreferenceRequest{project, false}));
    const auto* preferenceA =
        responseValue<Domain::AutomaticContinuityPreference>(savedA);
    require(preferenceA != nullptr && !preferenceA->enabled &&
        preferenceA->providerId.find("provider-a") != std::string::npos,
        "provider A continuity preference is saved off");
    require(memory.lastRememberRequest() &&
        memory.lastRememberRequest()->write.body,
        "provider A preference is persisted through project memory");
    const auto bodyA = *memory.lastRememberRequest()->write.body;

    controller->currentSettings.localModelName = "provider-b";
    memory.rememberResult.set(
        Domain::Result<Domain::MemoryWriteOutcome>::success(
            writeOutcome(secondRecordId)));
    const auto savedB = dispatcher.dispatch(request(*clock, 761U,
        Manager::ManagerAutomaticContinuityPreferenceRequest{project, true}));
    const auto* preferenceB =
        responseValue<Domain::AutomaticContinuityPreference>(savedB);
    require(preferenceB != nullptr && preferenceB->enabled &&
        preferenceB->providerId.find("provider-b") != std::string::npos,
        "provider B continuity preference is independently saved on");
    const auto bodyB = *memory.lastRememberRequest()->write.body;

    const auto now = clock->utc;
    const auto stored = [&](const Domain::MemoryRecordId& id,
                            std::string provider,
                            std::string body) {
        return Domain::ProjectMemoryRecord{
            id, project, 1U, "automatic_continuity_preference",
            std::move(provider), "automatic continuity preference",
            std::move(body), {"automatic-continuity", "provider-binding"},
            1.0, 1.0, "manager_automatic_continuity", std::nullopt,
            std::nullopt, now, now, now, std::nullopt, digest, false,
            Domain::ProjectMemorySchemaVersion};
    };
    auto recordB = stored(secondRecordId, preferenceB->providerId, bodyB);
    auto recordA = stored(firstRecordId, preferenceA->providerId, bodyA);
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {{recordB, 1.0}, {recordA, 1.0}}, std::nullopt,
            false, 2'048U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));

    Manager::ManagerRequestDispatcher restarted{
        controller, clock, Manager::ManagerTransportLimits{},
        managedRuns, sources};
    controller->currentSettings.localModelName = "provider-a";
    const auto readA = restarted.dispatch(request(*clock, 762U,
        Manager::ManagerAutomaticContinuityPreferenceRequest{
            project, std::nullopt}));
    const auto* durableA =
        responseValue<Domain::AutomaticContinuityPreference>(readA);
    require(durableA != nullptr && !durableA->enabled,
        "provider A preference survives dispatcher restart/readback");

    const auto runId = Domain::SessionId::parse(uuidText(706U)).value();
    const auto clientId = Domain::ClientId::parse(uuidText(707U)).value();
    const auto started = restarted.dispatch(request(*clock, 763U,
        Manager::ManagedRunStartRequest{
            runId, project, clientId, 0U, "Run with exact preference.",
            true, true}));
    requireError(started, Domain::ErrorCodes::InvalidRequest, "saved preference cannot restore removed run entry point");
    require(managedRuns->startCalls == 0U, "preference does not create a run");

    controller->currentSettings.localModelName = "provider-b";
    const auto readB = restarted.dispatch(request(*clock, 764U,
        Manager::ManagerAutomaticContinuityPreferenceRequest{
            project, std::nullopt}));
    const auto* durableB =
        responseValue<Domain::AutomaticContinuityPreference>(readB);
    require(durableB != nullptr && durableB->enabled,
        "provider B preference remains independent after restart/readback");
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
    requireError(aResponse, Domain::ErrorCodes::InvalidRequest, "removed run history rejected");
    requireError(dispatcher.dispatch(request(*clock, 92U,
        Manager::ManagerOperationalRequest{Manager::ManagerOperationalArea::Runs,
            Manager::ManagerOperationalAction::Inspect, std::nullopt, {}, projectB})),
        Domain::ErrorCodes::InvalidRequest, "removed run history rejected for every project");
    require(operational.sessionCalls == 0U, "removed readback never lists sessions");
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
    require(runtimeText.find("Sessions are LM Studio chats") != std::string::npos,
        "runtime inventory identifies the actual session host");
    require(runtimeText.find(aOpen.value()) == std::string::npos &&
        runtimeText.find(aRecent.value()) == std::string::npos &&
        runtimeText.find(bRecent.value()) == std::string::npos,
        "runtime inventory does not expose removed run readback");
    require(operational.sessionCalls == 0U && managedRuns->statusCalls == 0U,
        "runtime inventory never reads Managed Run results");
}

void testActivityAndDoctorProjectSimplifiedWorkflowState()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    FakeOperationalSessions operational;
    operational.allowDoctor = true;
    const auto project = Domain::ProjectId::parse(uuidText(714U)).value();
    const auto rowRecordId = Domain::MemoryRecordId::parse(uuidText(715U)).value();
    const auto revision = Domain::Sha256Digest::parse(std::string(64U, 'e')).value();
    FixedHasher hasher{revision};
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(
        Domain::ProjectMemoryDescriptor{project, "Activity project", std::nullopt,
            {Domain::PathText::create("D:\\ActivityFixture").value()}})),
        "seed Activity project");
    const auto queueBody = nlohmann::json{
        {"schema", "forge-instruction-package-queue-v2"},
        {"project_id", project.value()}, {"queue_row_id", "queue-fixture"},
        {"package_id", "package-fixture"}, {"package_name", "fixture"},
        {"package_path", "D:\\ActivityFixture\\instructions"},
        {"revision", revision.value()}, {"order", 1024U}, {"state", "active"},
        {"cursor", {{"entry", 1U}, {"byte_offset", 0U}}},
        {"entry_count", 2U}, {"content_bytes", 20U},
        {"coverage_gap_count", 0U}, {"attempts", 1U},
        {"correlation_id", "package-correlation"}, {"last_error", nullptr}}.dump();
    const auto now = clock->utc;
    Domain::ProjectMemoryRecord queueRecord{
        rowRecordId, project, 1U, "instruction_package_queue", "fixture",
        "active fixture", queueBody, {"instruction-package-queue"}, 1.0, 1.0,
        "manager_instruction_package", std::nullopt, std::nullopt,
        now, now, now, std::nullopt, revision, false,
        Domain::ProjectMemorySchemaVersion};
    TestFakes::RecordingProjectMemoryService memory;
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {{queueRecord, 1.0}}, std::nullopt, false, 2048U,
            256U * 1024U, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    memory.statusResult.set(
        Domain::Result<Domain::ProjectMemoryStatus>::success(
            Domain::ProjectMemoryStatus{project,
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion, 1U}));
    FakeProjectPolicy policy;
    Manager::ManagerTelemetrySources sources;
    sources.operational = &operational;
    sources.projects = &registry;
    sources.projectMemory = &memory;
    sources.evidenceHasher = &hasher;
    sources.projectPolicy = &policy;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    const auto feedResponse = dispatcher.dispatch(request(*clock, 716U,
        Manager::ManagerOperationalRequest{Manager::ManagerOperationalArea::Feed,
            Manager::ManagerOperationalAction::Inspect, std::nullopt, {}, project}));
    const auto* feed = responseValue<Manager::ManagerOperationalSnapshot>(feedResponse);
    require(feed != nullptr, "project Activity feed succeeds");
    std::string feedText;
    for (const auto& line : feed->lines) feedText += line + "\n";
    require(feedText.find("package.queue") != std::string::npos &&
        feedText.find("queue-fixture") != std::string::npos &&
        feedText.find("package-correlation") != std::string::npos,
        "Activity includes package queue/cursor event correlation");
    require(feedText.find("clu.finding_resolved") != std::string::npos &&
        feedText.find("finding-fixture") != std::string::npos &&
        feedText.find("clu-correlation") != std::string::npos,
        "Activity includes CLU finding/resolution event correlation");

    const auto doctorResponse = dispatcher.dispatch(request(*clock, 717U,
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Diagnostics,
            Manager::ManagerOperationalAction::Inspect, std::nullopt, {}, project}));
    const auto* doctor = responseValue<Manager::ManagerOperationalSnapshot>(doctorResponse);
    require(doctor != nullptr, "project Doctor succeeds");
    std::string doctorText;
    for (const auto& line : doctor->lines) doctorText += line + "\n";
    require(doctorText.find("PASS package_queue_cursor_integrity") != std::string::npos &&
        doctorText.find("PASS clu_repository_notification_export") != std::string::npos &&
        doctorText.find("PASS continuity_preference_provider_binding") != std::string::npos &&
        doctorText.find("PASS simplified_workflow_schema_alignment") != std::string::npos,
        "Doctor checks package, CLU, continuity binding, migration, export, and schema alignment");
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
    requireError(responseA, Domain::ErrorCodes::InvalidRequest, "removed evidence readback rejected");
    require(operational.sessionCalls == 0U, "removed evidence does not read sessions");
}

void testNativeTaskCheckRequiresExactVerifiedRunAndPersistsReceipt()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    FakeOperationalSessions operational;
    FakeDurableManagedRunStore durable;
    FakeEvidenceHasher hasher;
    FakeNativeCheckToolRouter nativeTool;
    const auto projectA = Domain::ProjectId::parse(uuidText(731U)).value();
    const auto projectB = Domain::ProjectId::parse(uuidText(732U)).value();
    const auto runA = Domain::SessionId::parse(uuidText(733U)).value();
    const auto time = Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}};
    operational.listing.recent.push_back({
        runA, Domain::AgentId::parse("forge-managed-run").value(),
        std::nullopt, Domain::SessionStatus::Completed,
        std::nullopt, time, time});
    Domain::ManagedRunRecord record{
        runA, projectA, Domain::ClientId::parse("native-check-fixture").value(),
        "private mission", 0U, Domain::ManagedRunState::Completed,
        std::nullopt, 3U, 2U, std::nullopt, "private model output",
        std::nullopt, {}, time, time, false};
    record.evidenceIntegrity = Domain::ManagedRunEvidenceIntegrity::Verified;
    record.evidenceSeal = Domain::Sha256Digest::parse(std::string(64U, 'b')).value();
    durable.records.emplace(runA.value(), record);
    TestFakes::DeterministicWorkspaceAuthority authority{
        Domain::AuthorityId::parse(uuidText(734U)).value(),
        Domain::ClientId::parse("native-check-fixture").value(),
        {Domain::PathText::create("D:\\NativeFixture").value()},
        Domain::FileAccess::Execute, {Domain::FileAccess::Execute}, {}, true, 0U};
    Manager::ManagerTelemetrySources sources;
    sources.operational = &operational;
    sources.durableManagedRunStore = &durable;
    sources.evidenceHasher = &hasher;
    sources.projectWorkspaceAuthority = &authority;
    sources.toolRouter = &nativeTool;
    sources.shellEnabled = true;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    for (const auto& project : {projectA, projectB}) {
        requireError(dispatcher.dispatch(request(*clock, 97U,
            Manager::ManagerOperationalRequest{Manager::ManagerOperationalArea::Evidence,
                Manager::ManagerOperationalAction::VerifyTask, runA, "Write-Output OK", project})),
            Domain::ErrorCodes::InvalidRequest, "removed native run verification rejected");
    }
    require(nativeTool.calls == 0U && durable.records.at(runA.value()).nativeTaskChecks.empty(),
        "removed verification cannot invoke tools or persist receipts");
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
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(pageFor(projectB)));
    const std::string pageCursor{"djE6MjA="};
    const auto selectedB = dispatcher.dispatch(request(
        *clock,
        81U,
        Manager::ManagerProjectMemoryRequest{
            projectB, "decision", 20U, pageCursor}));
    const auto* workspaceB =
        responseValue<Manager::ManagerProjectWorkspaceSnapshot>(selectedB);
    require(workspaceB != nullptr && workspaceB->project.id == projectB,
            "project B selection returns project B");
    require(memory.lastProjectId() == projectB,
            "project B memory is routed with project B identity");
    require(!memory.searchRequests().empty() &&
            memory.searchRequests().back().cursor == pageCursor &&
            memory.searchRequests().back().limit == 20U,
            "project-memory search forwards the continuation cursor");

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
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(pageFor(projectA)));
    const auto listRequestCount = memory.listRecentRequests().size();
    const auto selectedA = dispatcher.dispatch(request(
        *clock,
        83U,
        Manager::ManagerProjectMemoryRequest{
            projectA, {}, 20U, pageCursor}));
    const auto* workspaceA =
        responseValue<Manager::ManagerProjectWorkspaceSnapshot>(selectedA);
    require(workspaceA != nullptr && workspaceA->project.id == projectA,
            "project A selection remains isolated from project B");
    require(memory.listRecentRequests().size() == listRequestCount + 2U &&
            memory.listRecentRequests()[listRequestCount].kinds.empty() &&
            memory.listRecentRequests()[listRequestCount].cursor == pageCursor &&
            memory.listRecentRequests()[listRequestCount].limit == 20U,
            "recent project-memory reads forward the continuation cursor");
}

template <typename T>
[[nodiscard]] T requireQueueResult(Domain::Result<T> result)
{
    if (!result) fail(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

struct PersistentInstructionQueueFixture final {
    ForgeConductor::Tests::PersistenceSupport::ScopedTestDirectory directory{
        L"instruction-queue"};
    Domain::ProjectId project{Domain::ProjectId::parse(uuidText(9'000U)).value()};
    std::shared_ptr<FakeClock> clock{std::make_shared<FakeClock>()};
    TestFakes::ProjectRegistryRepositoryFake registry{
        8U, std::chrono::steady_clock::now()};
    std::shared_ptr<TestFakes::RecordingApplicationPathsFake> paths{
        std::make_shared<TestFakes::RecordingApplicationPathsFake>()};
    std::shared_ptr<TestFakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<ForgeConductor::Infrastructure::Windows::BCryptSha256Hasher>
        hasher{std::make_shared<ForgeConductor::Infrastructure::Windows::BCryptSha256Hasher>()};
    std::shared_ptr<ForgeConductor::Infrastructure::Windows::SecretRedactor>
        redactor{std::make_shared<ForgeConductor::Infrastructure::Windows::SecretRedactor>()};
    std::shared_ptr<TestFakes::SequenceUuidGenerator> uuids;
    std::shared_ptr<ForgeConductor::Persistence::Windows::WindowsProjectMemoryRepository>
        repository;
    std::unique_ptr<TestFakes::ProjectMemoryRepositoryFactoryFake> factory;
    std::unique_ptr<ForgeConductor::Application::ProjectMemoryService> memory;
    std::unique_ptr<Manager::ManagerRequestDispatcher> dispatcher;
    std::uint32_t nextRequest{10'000U};
    bool advanceClock{true};

    PersistentInstructionQueueFixture()
    {
        clock->monotonic = std::chrono::steady_clock::now();
        clock->utc = std::chrono::time_point_cast<std::chrono::seconds>(
            std::chrono::system_clock::now());
        diagnostics = std::make_shared<TestFakes::RuntimeDiagnosticsFake>(
            clock->monotonic);
        paths->setNow(clock->monotonic);
        paths->projectRootResult.set(Domain::Result<Domain::PathText>::success(
            ForgeConductor::Tests::PersistenceSupport::pathText(directory.path() / L"memory")));
        require(static_cast<bool>(registry.seedDescriptor(
            Domain::ProjectMemoryDescriptor{project, "Durable queue", std::nullopt,
                {ForgeConductor::Tests::PersistenceSupport::pathText(directory.path())}})),
            "seed durable queue project");
        std::vector<Domain::Uuid> sequence;
        for (std::uint32_t index = 20'000U; index < 20'512U; ++index) {
            sequence.push_back(Domain::Uuid::parse(uuidText(index)).value());
        }
        uuids = std::make_shared<TestFakes::SequenceUuidGenerator>(std::move(sequence));
        reopen();
    }

    [[nodiscard]] Domain::OperationContext context()
    {
        const auto suffix = nextRequest++;
        return Domain::OperationContext{operationId(suffix),
            std::chrono::steady_clock::now() + 30s, {}, correlationId(suffix)};
    }

    void reopen()
    {
        dispatcher.reset();
        memory.reset();
        factory.reset();
        repository.reset();
        repository = requireQueueResult(
            ForgeConductor::Persistence::Windows::WindowsProjectMemoryRepository::open(
                project, paths, diagnostics, redactor, hasher, uuids, clock, {}, context()));
        factory = std::make_unique<TestFakes::ProjectMemoryRepositoryFactoryFake>(
            1U, 1U, clock->monotonic);
        require(static_cast<bool>(factory->addRepository(repository)),
            "register durable queue repository");
        memory = std::make_unique<ForgeConductor::Application::ProjectMemoryService>(
            registry, *factory, *redactor, Domain::ProjectMemoryLimits{});
        Manager::ManagerTelemetrySources sources;
        sources.projects = &registry;
        sources.projectMemory = memory.get();
        sources.evidenceHasher = hasher.get();
        dispatcher = std::make_unique<Manager::ManagerRequestDispatcher>(
            std::make_shared<FakeController>(), clock,
            Manager::ManagerTransportLimits{}, std::shared_ptr<Contracts::IManagedRunService>{}, sources);
    }

    [[nodiscard]] Manager::ManagerResponse dispatch(Manager::ManagerRequestPayload payload)
    {
        if (advanceClock) clock->utc += 1s;
        return dispatcher->dispatch(request(*clock, nextRequest++, std::move(payload)));
    }

    [[nodiscard]] Manager::ManagerInstructionPackageQueueSnapshot queue(
        Manager::ManagerInstructionPackageQueueAction action =
            Manager::ManagerInstructionPackageQueueAction::List,
        std::optional<std::string> rowId = {}, std::optional<std::size_t> target = {})
    {
        const auto response = dispatch(Manager::ManagerInstructionPackageQueueRequest{
            project, action, std::move(rowId), target});
        const auto* value = responseValue<Manager::ManagerInstructionPackageQueueSnapshot>(response);
        require(value != nullptr, std::string{"durable queue operation: "} +
            (responseError(response) ? responseError(response)->message : "wrong response type"));
        return *value;
    }

    [[nodiscard]] Domain::MemoryRecordId remember(
        std::string kind, std::string title, nlohmann::json body)
    {
        Domain::ProjectMemoryWrite write;
        write.kind = std::move(kind);
        write.title = std::move(title);
        write.summary = "durable instruction queue fixture";
        write.body = body.dump();
        write.sourceKind = "instruction_queue_test";
        return requireQueueResult(memory->remember(
            Domain::RememberProjectMemoryRequest{project, std::move(write)}, context())).recordId;
    }

    [[nodiscard]] Domain::MemoryRecordId seedRow(
        const std::string& id, const std::string& state, std::uint64_t order)
    {
        return remember("instruction_package_queue", id, nlohmann::json{
            {"schema", "forge-instruction-package-queue-v2"},
            {"project_id", project.value()}, {"queue_row_id", id},
            {"package_id", "package-" + id}, {"package_name", id},
            {"package_path", (directory.path() / id).string()},
            {"revision", std::string(64U, 'a')}, {"order", order}, {"state", state},
            {"cursor", {{"entry", 3U}, {"byte_offset", 9U}}},
            {"entry_count", 4U}, {"content_bytes", 64U},
            {"coverage_gap_count", 1U}, {"attempts", 2U},
            {"last_error", state == "failed" || state == "interrupted"
                ? nlohmann::json("previous interruption") : nlohmann::json(nullptr)}});
    }
};

void testInstructionQueueRemovalIsImmediateForEveryState()
{
    PersistentInstructionQueueFixture fixture;
    for (const auto* state : {"active", "ready", "failed", "interrupted"}) {
        const auto id = fixture.seedRow(state, state, 1024U);
        const auto removed = fixture.queue(
            Manager::ManagerInstructionPackageQueueAction::Remove, std::string{state});
        require(removed.rows.empty(), std::string{state} + " package removes immediately");
        fixture.reopen();
        require(fixture.queue().rows.empty(), std::string{state} + " removal survives reopen");
        const auto oldRecord = fixture.memory->get(
            Domain::GetProjectMemoryRequest{fixture.project, {id}, true}, fixture.context());
        require(oldRecord && oldRecord.value().records.empty(),
            "removed queue record is tombstoned");
    }
    static_cast<void>(fixture.seedRow("only", "active", 1024U));
    requireError(fixture.dispatch(Manager::ManagerInstructionPackageQueueRequest{
        fixture.project, Manager::ManagerInstructionPackageQueueAction::Move,
        std::string{"only"}, 1U}), Domain::ErrorCodes::InvalidRequest,
        "out-of-range package move");
    requireError(fixture.dispatch(Manager::ManagerInstructionPackageQueueRequest{
        fixture.project, Manager::ManagerInstructionPackageQueueAction::Remove,
        std::string{"missing"}}), Domain::ErrorCodes::RecordNotFound,
        "missing package removal");
    requireError(fixture.dispatch(Manager::ManagerInstructionPackageQueueRequest{
        Domain::ProjectId::parse(uuidText(9'001U)).value(),
        Manager::ManagerInstructionPackageQueueAction::Remove, std::string{"only"}}),
        Domain::ErrorCodes::ProjectNotFound, "unregistered project removal");
    require(fixture.queue().rows.size() == 1U,
        "invalid mutations preserve the selected project's queue");
}

void testInstructionQueueRepeatedOrderSurvivesRepositoryReopen()
{
    PersistentInstructionQueueFixture fixture;
    static_cast<void>(fixture.seedRow("A", "active", 1024U));
    static_cast<void>(fixture.seedRow("B", "ready", 2048U));
    const auto seedOrder = [&](const std::string& first, const std::string& second) {
        fixture.clock->utc += 1s;
        Domain::ProjectMemoryWrite write;
        write.kind = "instruction_package_queue_order";
        write.title = "Instruction package execution order";
        write.summary = "2 ordered package rows";
        write.body = nlohmann::json{{"schema", "forge-instruction-package-order-v1"},
            {"project_id", fixture.project.value()},
            {"rows", nlohmann::json::array({first, second})}}.dump();
        write.tags = {"instruction-package-queue-order"};
        write.importance = 1.0;
        write.confidence = 1.0;
        write.sourceKind = "manager_instruction_package";
        const auto identity = first + "\n" + second + "\n";
        const auto digest = requireQueueResult(fixture.hasher->sha256(
            std::as_bytes(std::span{identity.data(), identity.size()})));
        write.idempotencyKey = Domain::IdempotencyKey::create(
            "instruction-order:" + digest.value()).value();
        static_cast<void>(requireQueueResult(fixture.memory->remember(
            Domain::RememberProjectMemoryRequest{fixture.project, std::move(write)},
            fixture.context())));
    };
    seedOrder("A", "B");
    seedOrder("B", "A");
    const auto requireOrder = [&](const std::string& first, const std::string& second) {
        const auto listed = fixture.queue();
        require(listed.rows.size() == 2U && listed.rows[0].queueRowId == first &&
            listed.rows[1].queueRowId == second, "durable queue has requested order " + first + second);
    };
    fixture.reopen();
    requireOrder("B", "A");
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Move,
        std::string{"A"}, 0U));
    fixture.reopen();
    requireOrder("A", "B");
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Move,
        std::string{"B"}, 0U));
    fixture.reopen();
    requireOrder("B", "A");
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Move,
        std::string{"A"}, 0U));
    fixture.reopen();
    requireOrder("A", "B");
    fixture.advanceClock = false;
    const auto sameSecond = fixture.clock->utc;
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Move,
        std::string{"B"}, 0U));
    fixture.reopen();
    requireOrder("B", "A");
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Move,
        std::string{"A"}, 0U));
    fixture.reopen();
    requireOrder("A", "B");
    require(fixture.clock->utc == sameSecond,
        "opposite order mutations and readback occurred within one persisted second");
    const auto removed = fixture.queue(Manager::ManagerInstructionPackageQueueAction::Remove,
        std::string{"A"});
    require(removed.rows.size() == 1U && removed.rows.front().queueRowId == "B" && removed.rows.front().order == 1024U,
        "removing active row preserves its neighbor");
    fixture.reopen();
    const auto remaining = fixture.queue();
    require(remaining.rows.size() == 1U && remaining.rows.front().queueRowId == "B" &&
        remaining.rows.front().order == 1024U, "remaining compacted order survives reopen");
}

void testInstructionQueueLegacyDeletionAndReadoptionAreDurable()
{
    PersistentInstructionQueueFixture fixture;
    const auto folder = fixture.directory.path() / L"source";
    std::filesystem::create_directories(folder);
    {
        std::ofstream output{folder / L"START-HERE.md", std::ios::binary};
        output << "Follow this project contract.\n";
    }
    const auto legacyFile = fixture.remember("instruction_package_file", "START-HERE.md",
        nlohmann::json("legacy text"));
    static_cast<void>(fixture.remember("instruction_package", "Legacy package", nlohmann::json{
        {"schema", "forge-instruction-package-v1"}, {"package_name", "Legacy package"},
        {"package_path", folder.string()}, {"revision", std::string(64U, 'd')},
        {"files", nlohmann::json::array({{{"path", "START-HERE.md"},
            {"record_id", legacyFile.value()}}})}}));
    const auto migrated = fixture.queue();
    require(migrated.rows.size() == 1U && migrated.rows.front().state == "active",
        "legacy fixture migrates active queue row");
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Remove,
        migrated.rows.front().queueRowId));
    fixture.reopen();
    require(fixture.queue().rows.empty(), "last removed legacy row does not remigrate");
    require(fixture.queue().rows.empty(), "repeated empty listing does not recreate legacy row");
    const auto path = ForgeConductor::Tests::PersistenceSupport::pathText(folder);
    const auto preview = fixture.dispatch(Manager::ManagerInstructionPackageRequest{
        fixture.project, path, false, std::nullopt});
    const auto* inspected = responseValue<Manager::ManagerInstructionPackageSnapshot>(preview);
    require(inspected != nullptr, "source folder remains available after removal");
    const auto revision = inspected->revision;
    const auto add = [&] {
        const auto response = fixture.dispatch(Manager::ManagerInstructionPackageRequest{
            fixture.project, path, true, revision});
        const auto* added = responseValue<Manager::ManagerInstructionPackageSnapshot>(response);
        require(added != nullptr && added->activated && added->queueRowId,
            "same source revision can be adopted");
        return *added;
    };
    const auto firstId = *add().queueRowId;
    require(add().queueRowId == firstId && fixture.queue().rows.size() == 1U,
        "repeated live add does not duplicate queue row");
    static_cast<void>(fixture.seedRow("neighbor", "ready", 2048U));
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Move,
        firstId, 1U));
    const auto duplicate = add();
    require(duplicate.queueRowId == firstId && duplicate.queueOrder == 2048U &&
        fixture.queue().rows.size() == 2U,
        "duplicate live adoption returns effective moved order without duplicating rows");
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Remove,
        std::string{"neighbor"}));
    static_cast<void>(fixture.queue(Manager::ManagerInstructionPackageQueueAction::Remove, firstId));
    fixture.reopen();
    require(fixture.queue().rows.empty(), "new row removal survives reopen");
    const auto nextId = *add().queueRowId;
    fixture.reopen();
    const auto readopted = fixture.queue();
    require(readopted.rows.size() == 1U && readopted.rows.front().queueRowId == nextId,
        "removed source revision can be re-added as a visible durable queue row");
    require(ForgeConductor::Tests::PersistenceSupport::readFixture(folder / L"START-HERE.md") ==
        "Follow this project contract.\n", "queue removal never deletes source files");
}
void testInstructionQueueMutationFailuresRemainVisible()
{
    auto clock = std::make_shared<FakeClock>();
    const auto project = Domain::ProjectId::parse(uuidText(9'100U)).value();
    const auto rowId = Domain::MemoryRecordId::parse(uuidText(9'101U)).value();
    const auto orderId = Domain::MemoryRecordId::parse(uuidText(9'102U)).value();
    const auto revision = Domain::Sha256Digest::parse(std::string(64U, 'e')).value();
    FixedHasher hasher{revision};
    TestFakes::ProjectRegistryRepositoryFake registry{1U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        project, "Queue failures", std::nullopt,
        {Domain::PathText::create("D:\\QueueFailures").value()}})), "seed queue failure project");
    TestFakes::RecordingProjectMemoryService memory;
    const auto makeRecord = [&](const Domain::MemoryRecordId& id,
                                std::string kind, nlohmann::json body) {
        return Domain::ProjectMemoryRecord{id, project, 1U, std::move(kind),
            "Queue failure", "Queue failure", body.dump(), {}, 1.0, 1.0,
            "queue_failure_test", std::nullopt, std::nullopt,
            clock->utc, clock->utc, clock->utc, std::nullopt, revision, false,
            Domain::ProjectMemorySchemaVersion};
    };
    const auto row = makeRecord(rowId, "instruction_package_queue", nlohmann::json{
        {"schema", "forge-instruction-package-queue-v2"}, {"project_id", project.value()},
        {"queue_row_id", "queue-failure"}, {"package_id", "package-failure"},
        {"package_name", "Queue failure"}, {"package_path", "D:\\QueueFailures"},
        {"revision", revision.value()}, {"order", 1024U}, {"state", "active"},
        {"cursor", {{"entry", 0U}, {"byte_offset", 0U}}}});
    auto empty = Domain::MemoryPage{project, {}, std::nullopt, false, 0U,
        64U * 1024U, Domain::ProjectMemorySchemaVersion, Domain::ProjectMemoryCapabilityVersion};
    auto queuePage = empty;
    queuePage.records.push_back(Domain::MemorySearchHit{row, 1.0});
    memory.listRecentByKind["instruction_package_queue"].set(
        Domain::Result<Domain::MemoryPage>::success(queuePage));
    memory.listRecentByKind["instruction_package_queue_order"].set(
        Domain::Result<Domain::MemoryPage>::success(empty));
    memory.rememberResult.set(Domain::Result<Domain::MemoryWriteOutcome>::failure(
        Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "order write failed")));
    Manager::ManagerTelemetrySources sources;
    sources.projects = &registry;
    sources.projectMemory = &memory;
    sources.evidenceHasher = &hasher;
    Manager::ManagerRequestDispatcher dispatcher{
        std::make_shared<FakeController>(), clock, Manager::ManagerTransportLimits{}, {}, sources};
    const auto move = [&](std::uint32_t suffix) {
        return dispatcher.dispatch(request(*clock, suffix,
            Manager::ManagerInstructionPackageQueueRequest{project,
                Manager::ManagerInstructionPackageQueueAction::Move,
                std::string{"queue-failure"}, 0U}));
    };
    const auto remove = [&](std::uint32_t suffix) {
        return dispatcher.dispatch(request(*clock, suffix,
            Manager::ManagerInstructionPackageQueueRequest{project,
                Manager::ManagerInstructionPackageQueueAction::Remove,
                std::string{"queue-failure"}}));
    };
    requireError(move(9'110U), Domain::ErrorCodes::IntegrityFailure, "queue order insert failure");
    const auto failedRemoval = remove(9'111U);
    requireError(failedRemoval, Domain::ErrorCodes::IntegrityFailure, "removal order insert failure");
    require(responseError(failedRemoval)->message == "order write failed" &&
        memory.callCount(TestFakes::ProjectMemoryCall::Forget) == 0U,
        "failed removal marker is reported before forgetting a row");
    memory.rememberResult.set(Domain::Result<Domain::MemoryWriteOutcome>::success(
        Domain::MemoryWriteOutcome{project, orderId, 1U, Domain::MemoryWriteDisposition::Inserted,
            revision, Domain::ProjectMemorySchemaVersion, Domain::ProjectMemoryCapabilityVersion}));
    memory.forgetResult.set(Domain::Result<Domain::ForgetOutcome>::failure(
        Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "forget failed")));
    const auto forgetFailed = remove(9'112U);
    requireError(forgetFailed, Domain::ErrorCodes::IntegrityFailure, "queue forget failure");
    require(responseError(forgetFailed)->message == "forget failed" &&
        memory.callCount(TestFakes::ProjectMemoryCall::Forget) == 1U,
        "forget error reaches the caller");
    auto orderPage = empty;
    orderPage.records.push_back(Domain::MemorySearchHit{makeRecord(orderId,
        "instruction_package_queue_order", nlohmann::json{
            {"schema", "forge-instruction-package-order-v1"}, {"project_id", project.value()},
            {"rows", nlohmann::json::array({"queue-failure"})}}), 1.0});
    memory.listRecentByKind["instruction_package_queue_order"].set(
        Domain::Result<Domain::MemoryPage>::success(orderPage));
    memory.updateResult.set(Domain::Result<Domain::ProjectMemoryRecord>::failure(
        Domain::makeError(Domain::ErrorCodes::IntegrityFailure, "order update failed")));
    requireError(move(9'113U), Domain::ErrorCodes::IntegrityFailure, "queue order update failure");
    const auto updateFailed = remove(9'114U);
    requireError(updateFailed, Domain::ErrorCodes::IntegrityFailure, "removal order update failure");
    require(responseError(updateFailed)->message == "order update failed" &&
        memory.callCount(TestFakes::ProjectMemoryCall::Forget) == 1U,
        "failed order update leaves queue row unforgotten");
}
void testInstructionPackagePreviewAndActivationStayProjectBound()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const auto project = Domain::ProjectId::parse(uuidText(820U)).value();
    const Domain::ProjectMemoryDescriptor descriptor{
        project, "Instruction project", std::nullopt,
        {Domain::PathText::create("D:\\Projects\\Instructions").value()}};
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(descriptor)),
        "seed instruction project");
    TestFakes::RecordingProjectMemoryService memory;
    const auto revision = Domain::Sha256Digest::parse(
        std::string(64U, 'a')).value();
    FixedHasher hasher{revision};

    const auto packageRoot = std::filesystem::temp_directory_path() /
        "forge-instruction-package-dispatcher-test";
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{packageRoot};
    std::error_code ignored;
    std::filesystem::remove_all(packageRoot, ignored);
    std::filesystem::create_directories(packageRoot / "specs");
    {
        std::ofstream output{packageRoot / "START-HERE.md", std::ios::binary};
        output << "# Start here\nFollow the project contract.\n";
    }
    {
        std::ofstream output{packageRoot / "specs" / "policy.json",
            std::ios::binary};
        output << "{\"mode\":\"bounded\"}\n";
    }
    {
        std::ofstream output{packageRoot / "ignored.bin", std::ios::binary};
        const std::array<unsigned char, 6U> binary{0x00U, 0xffU, 0x10U, 0x80U, 0x00U, 0x7fU};
        output.write(reinterpret_cast<const char*>(binary.data()),
            static_cast<std::streamsize>(binary.size()));
    }
    std::filesystem::create_directories(packageRoot / "bulk" / "nested");
    for (std::size_t index{}; index < 40U; ++index) {
        std::ofstream output{
            packageRoot / "bulk" / "nested" / ("entry-" + std::to_string(index))};
        output << "extensionless entry " << index << '\n';
    }
    {
        std::ofstream output{packageRoot / "large.dat", std::ios::binary};
        std::string block(300U * 1024U, 'L');
        output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    {
        std::ofstream output{packageRoot / "sparse.bin", std::ios::binary};
        output.seekp((4U * 1024U * 1024U) - 1U);
        output.put('\0');
    }
    const auto reparseCreated = ::CreateSymbolicLinkW(
        (packageRoot / "linked-specs").c_str(),
        (packageRoot / "specs").c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2U) != FALSE;
    require(reparseCreated,
        "instruction fixture creates a directory reparse point");
    const auto transientCreated = ::CreateSymbolicLinkW(
        (packageRoot / "transient-entry").c_str(),
        (packageRoot / "already-gone.txt").c_str(),
        0x2U) != FALSE;
    require(transientCreated,
        "instruction fixture creates a transient broken entry");
    {
        std::ofstream output{packageRoot / "unreadable.txt", std::ios::binary};
        output << "temporarily locked";
    }
    const auto unreadableHandle = ::CreateFileW(
        (packageRoot / "unreadable.txt").c_str(), GENERIC_READ,
        0U, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(unreadableHandle != INVALID_HANDLE_VALUE,
        "instruction fixture locks an unreadable entry");
    struct CloseNativeHandle final {
        HANDLE value;
        ~CloseNativeHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    } closeUnreadable{unreadableHandle};
    const auto packagePath = Domain::PathText::create(
        packageRoot.string()).value();
    Manager::ManagerTelemetrySources sources;
    sources.projects = &registry;
    sources.projectMemory = &memory;
    sources.evidenceHasher = &hasher;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    const auto preview = dispatcher.dispatch(request(
        *clock, 821U, Manager::ManagerInstructionPackageRequest{
            project, packagePath, false, std::nullopt}));
    const auto* previewSnapshot =
        responseValue<Manager::ManagerInstructionPackageSnapshot>(preview);
    require(previewSnapshot != nullptr &&
        previewSnapshot->projectId == project &&
        previewSnapshot->revision == revision &&
        previewSnapshot->fileCount >= 51U &&
        previewSnapshot->ignoredFileCount == 0U &&
        previewSnapshot->contentBytes > 4U * 1024U * 1024U &&
        previewSnapshot->coverageGapCount >= 5U &&
        !previewSnapshot->activated &&
        !previewSnapshot->manifestRecordId,
        "instruction package preview accepts and inventories every entry");
    require(hasher.calls > 1U && hasher.lastByteCount > 0U,
        "instruction preview derives a bounded streaming revision");
    require(memory.callCount(TestFakes::ProjectMemoryCall::RememberBatch) == 0U &&
        memory.callCount(TestFakes::ProjectMemoryCall::Remember) == 0U,
        "instruction preview does not mutate memory");

    const auto fileA = Domain::MemoryRecordId::parse(uuidText(822U)).value();
    const auto fileB = Domain::MemoryRecordId::parse(uuidText(823U)).value();
    const auto manifest = Domain::MemoryRecordId::parse(uuidText(824U)).value();
    const auto outcome = [&](const Domain::MemoryRecordId& id) {
        return Domain::MemoryWriteOutcome{
            project, id, 1U, Domain::MemoryWriteDisposition::Inserted,
            revision, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion};
    };
    memory.rememberBatchResult.set(
        Domain::Result<Domain::MemoryBatchOutcome>::success(
            Domain::MemoryBatchOutcome{
                project, {outcome(fileA), outcome(fileB)},
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion}));
    memory.rememberResult.set(
        Domain::Result<Domain::MemoryWriteOutcome>::success(outcome(manifest)));
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {}, std::nullopt, false, 0U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));

    const auto stale = Domain::Sha256Digest::parse(
        std::string(64U, 'b')).value();
    requireError(dispatcher.dispatch(request(
        *clock, 825U, Manager::ManagerInstructionPackageRequest{
            project, packagePath, true, stale})),
        Domain::ErrorCodes::Conflict,
        "instruction activation with stale preview");
    require(memory.callCount(TestFakes::ProjectMemoryCall::RememberBatch) == 0U,
        "stale instruction activation commits nothing");

    const auto activated = dispatcher.dispatch(request(
        *clock, 826U, Manager::ManagerInstructionPackageRequest{
            project, packagePath, true, revision}));
    const auto* activatedSnapshot =
        responseValue<Manager::ManagerInstructionPackageSnapshot>(activated);
    require(activatedSnapshot != nullptr && activatedSnapshot->activated &&
        activatedSnapshot->manifestRecordId == manifest &&
        activatedSnapshot->queueRowId,
        "validated instruction revision is added with queue identity");
    require(memory.callCount(TestFakes::ProjectMemoryCall::RememberBatch) > 1U &&
        memory.callCount(TestFakes::ProjectMemoryCall::Remember) == 1U &&
        memory.lastProjectId() == project,
        "instruction files and manifest remain bound to exact project");
    bool sawReparse{};
    bool sawUnavailable{};
    std::optional<std::string> startEntryBody;
    for (const auto& batch : memory.rememberBatchRequests()) {
        for (const auto& write : batch.writes) {
            if (!write.body) continue;
            const auto entry = nlohmann::json::parse(*write.body);
            sawReparse = sawReparse ||
                entry.value("kind", std::string{}) == "reparse_point";
            sawUnavailable = sawUnavailable ||
                entry.value("interpretation", std::string{}) == "unavailable";
            if (entry.value("relative_path", std::string{}) ==
                "START-HERE.md") startEntryBody = *write.body;
        }
    }
    require(sawReparse && sawUnavailable && startEntryBody,
        "reparse, unreadable, or transient entries remain explicit persisted records");

    const auto now = clock->utc;
    const auto record = [&](const Domain::MemoryRecordId& id,
                            std::string kind,
                            std::string title,
                            std::string body) {
        return Domain::ProjectMemoryRecord{
            id, project, 1U, std::move(kind), std::move(title),
            "instruction package test", std::move(body),
            {"instruction-package"}, 1.0, 1.0,
            "manager_instruction_package", std::nullopt, std::nullopt,
            now, now, now, std::nullopt, revision, false,
            Domain::ProjectMemorySchemaVersion};
    };
    const auto queueRowId = *activatedSnapshot->queueRowId;
    const auto manifestBody = nlohmann::json{
        {"schema", "forge-instruction-package-queue-v2"},
        {"project_id", project.value()},
        {"queue_row_id", queueRowId},
        {"package_id", revision.value()},
        {"package_name", "fixture"},
        {"package_path", packagePath.value()},
        {"revision", revision.value()},
        {"order", 1024U},
        {"state", "failed"},
        {"cursor", {{"entry", 0U}, {"byte_offset", 0U}}},
        {"entry_count", 2U},
        {"content_bytes", 64U},
        {"coverage_gap_count", 0U},
        {"attempts", 1U},
        {"last_error", "transient read failure"}}.dump();
    auto manifestRecord = record(
        manifest, "instruction_package_queue", "fixture", manifestBody);
    memory.updateResult.set(
        Domain::Result<Domain::ProjectMemoryRecord>::success(manifestRecord));
    auto firstFile = record(
        fileA, "instruction_package_entry", "START-HERE.md",
        *startEntryBody);
    auto secondFile = record(
        fileB, "instruction_package_entry", "specs/policy.json",
        nlohmann::json{{"queue_row_id", queueRowId},
            {"relative_path", "specs/policy.json"}, {"kind", "file"},
            {"byte_length", 18U}, {"content_hash", revision.value()},
            {"interpretation", "interpreted"}, {"coverage_detail", nullptr},
            {"derived_text", "{\"mode\":\"bounded\"}"}}.dump());
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {{manifestRecord, 1.0}}, std::nullopt,
            false, 1'024U, 64U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    memory.statusResult.set(
        Domain::Result<Domain::ProjectMemoryStatus>::success(
            Domain::ProjectMemoryStatus{
                project, 1U, 1U, 0U, 0U, 1U, 4'096U, 128U,
                false, true, Domain::ProjectMemorySchemaVersion, {}}));
    const auto retriedResponse = dispatcher.dispatch(request(
        *clock, 8260U, Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::Retry,
            queueRowId}));
    const auto* retried = responseValue<
        Manager::ManagerInstructionPackageQueueSnapshot>(retriedResponse);
    require(retried != nullptr && !retried->rows.empty() &&
        retried->rows.front().state == "ready" &&
        !retried->rows.front().lastError && memory.lastUpdateRequest() &&
        memory.lastUpdateRequest()->body &&
        nlohmann::json::parse(*memory.lastUpdateRequest()->body)
            .at("attempts").get<std::uint64_t>() == 2U,
        "failed package row retry persists a cleared error and incremented attempt");
    const auto workspaceResponse = dispatcher.dispatch(request(
        *clock, 8261U,
        Manager::ManagerProjectMemoryRequest{project, {}, 20U}));
    const auto* workspace =
        responseValue<Manager::ManagerProjectWorkspaceSnapshot>(
            workspaceResponse);
    require(workspace != nullptr && workspace->activeInstructionManifest &&
        workspace->activeInstructionManifest->id == manifest,
        "project workspace projects the active instruction manifest outside pagination");
    memory.searchResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {{firstFile, 1.0}, {secondFile, 1.0}},
            std::nullopt, false, 2'048U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    const auto contentPage = dispatcher.dispatch(request(
        *clock, 8262U, Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::ReadContent,
            queueRowId, std::nullopt, std::nullopt, 50U,
            std::string{"START-HERE.md"}, 0U, 8U}));
    const auto* content = responseValue<
        Manager::ManagerInstructionPackageQueueSnapshot>(contentPage);
    require(content != nullptr && content->contentBase64 &&
        content->contentRevision == revision && content->contentHash &&
        content->nextContentOffset == 8U && !content->contentComplete,
        "content page is bounded and bound to immutable revision and hash");
    {
        std::ofstream changed{packageRoot / "START-HERE.md",
            std::ios::binary | std::ios::trunc};
        changed << "changed after immutable package validation\n";
    }
    requireError(dispatcher.dispatch(request(
        *clock, 8263U, Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::ReadContent,
            queueRowId, std::nullopt, std::nullopt, 50U,
            std::string{"START-HERE.md"}, 8U, 8U})),
        Domain::ErrorCodes::Conflict,
        "content change between page requests is rejected against immutable hash");
    auto managedRuns = std::make_shared<FakeManagedRuns>();
    Manager::ManagerRequestDispatcher runDispatcher{
        controller, clock, Manager::ManagerTransportLimits{},
        managedRuns, sources};
    const auto runId = Domain::SessionId::parse(uuidText(827U)).value();
    const auto clientId = Domain::ClientId::parse(uuidText(828U)).value();
    const auto started = runDispatcher.dispatch(request(
        *clock, 829U, Manager::ManagedRunStartRequest{
            runId, project, clientId, 7U, "Complete the project work."}));
    requireError(started, Domain::ErrorCodes::InvalidRequest, "package assignment cannot restore Managed Run");
    require(managedRuns->startCalls == 0U, "package queue does not create a run");
}

void testLmStudioBindingUsesRegisteredAuthorizedProject()
{
    auto clock = std::make_shared<FakeClock>();
    const auto project = Domain::ProjectId::parse(uuidText(840U)).value();
    const auto root = Domain::PathText::create("D:\\Projects\\Selected").value();
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        project, "Selected", std::nullopt, {root}})), "seed project binding test");
    TestFakes::DeterministicWorkspaceAuthority access{
        Domain::AuthorityId::parse(uuidText(841U)).value(),
        Domain::ClientId::parse(uuidText(842U)).value(), {root},
        Domain::FileAccess::Read, {Domain::FileAccess::Read}, {}, false, 1U};
    const Domain::OperationContext context{operationId(843U), clock->monotonic + 1min, {}, correlationId(843U)};
    auto authority = access.authorityFor(project, context).value();
    TestFakes::DeterministicToolAuthorizerFake authorizer{
        "install-lmstudio-plugin", Domain::ToolEffect::Write, clock->monotonic};
    TestFakes::RecordingLMStudioDeploymentServiceFake deployment;
    deployment.setNow(clock->monotonic);
    deployment.statusResult.set(Domain::Result<Domain::LMStudioPluginStatus>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, "captured selected binding")));
    Manager::ManagerTelemetrySources sources;
    sources.projects = &registry;
    sources.projectWorkspaceAuthority = &access;
    sources.lmStudioDeployment = &deployment;
    sources.lmStudioReadAuthority = &authority;
    sources.lmStudioWriteAuthority = &authority;
    sources.toolAuthorizer = &authorizer;
    Manager::ManagerRequestDispatcher dispatcher{
        std::make_shared<FakeController>(), clock, Manager::ManagerTransportLimits{}, {}, sources};
    const auto selected = dispatcher.dispatch(request(*clock, 844U,
        Manager::ManagerLmStudioStatusRequest{project}));
    require(responseError(selected) && responseError(selected)->message == "captured selected binding" &&
        deployment.lastDeploymentRequest() &&
        deployment.lastDeploymentRequest()->projectId == project &&
        deployment.lastDeploymentRequest()->projectRoot == root,
        "LM Studio requests must resolve the exact registered and authorized workspace");
    const auto callsBeforeUnknown = deployment.statusCalls();
    const auto unknown = dispatcher.dispatch(request(*clock, 845U,
        Manager::ManagerLmStudioRepairRequest{Domain::ProjectId::parse(uuidText(846U)).value()}));
    require(responseError(unknown) && deployment.statusCalls() == callsBeforeUnknown &&
        deployment.deployCalls() == 0U,
        "An unregistered selected project must fail before inspection or deployment");
    const auto legacy = dispatcher.dispatch(request(*clock, 847U,
        Manager::ManagerLmStudioStatusRequest{}));
    require(responseError(legacy) && deployment.lastDeploymentRequest() &&
        !deployment.lastDeploymentRequest()->projectId &&
        !deployment.lastDeploymentRequest()->projectRoot,
        "Legacy projectless clients retain their current registration behavior");
}

void testExplicitLmStudioRepairAndActivationRemainAuthorized()
{
    auto clock = std::make_shared<FakeClock>();
    const auto project = Domain::ProjectId::parse(uuidText(850U)).value();
    const auto root = Domain::PathText::create("D:\\Projects\\ExplicitRepair").value();
    const auto binary = Domain::PathText::create("D:\\Forge\\forge-conductor.exe").value();
    const auto caller = Domain::ClientId::parse(uuidText(851U)).value();
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        project, "Explicit repair", std::nullopt, {root}})), "seed explicit repair project");
    TestFakes::DeterministicWorkspaceAuthority readIssuer{
        Domain::AuthorityId::parse(uuidText(852U)).value(), caller, {root},
        Domain::FileAccess::Read, {Domain::FileAccess::Read}, {}, false, 1U};
    TestFakes::DeterministicWorkspaceAuthority writeIssuer{
        Domain::AuthorityId::parse(uuidText(853U)).value(), caller, {root},
        Domain::FileAccess::Write, {Domain::FileAccess::Read, Domain::FileAccess::Write,
            Domain::FileAccess::Create, Domain::FileAccess::Delete, Domain::FileAccess::Execute}, {}, true, 1U};
    TestFakes::DeterministicWorkspaceAuthority executionIssuer{
        Domain::AuthorityId::parse(uuidText(854U)).value(), caller, {root},
        Domain::FileAccess::Execute, {Domain::FileAccess::Read, Domain::FileAccess::Execute}, {}, true, 1U};
    const Domain::OperationContext context{operationId(855U), clock->monotonic + 1min,
        {}, correlationId(855U)};
    auto readAuthority = readIssuer.authorityFor(project, context).value();
    auto writeAuthority = writeIssuer.authorityFor(project, context).value();
    auto executionAuthority = executionIssuer.authorityFor(project, context).value();
    const auto primary = Domain::PathText::create("D:\\LMStudio\\primary").value();
    const auto fallback = Domain::PathText::create("D:\\LMStudio\\fallback").value();
    const auto clu = Domain::PathText::create("D:\\LMStudio\\clu").value();
    const auto configuration = Domain::PathText::create("D:\\LMStudio\\mcp.json").value();
    const auto deploymentId = Domain::DeploymentId::parse("explicit-owner-deployment").value();
    TestFakes::RecordingLMStudioDeploymentServiceFake deployment;
    deployment.setNow(clock->monotonic);
    deployment.statusResult.set(Domain::Result<Domain::LMStudioPluginStatus>::success({
        true, true, true, true, binary, true, true, primary, fallback, clu,
        configuration, deploymentId, "Registered"}));
    deployment.deployResult.set(Domain::Result<Domain::LMStudioInstallResult>::success({
        true, binary, {primary, fallback, clu}, configuration, deploymentId,
        "Explicit owner repair completed"}));
    deployment.activateResult.set(Domain::Result<Domain::LMStudioHostActivationResult>::success({
        deploymentId, true, false, false, true,
        {Domain::LMStudioConnectorRole::Primary, Domain::LMStudioConnectorRole::Fallback,
            Domain::LMStudioConnectorRole::Clu}, "Explicit owner activation completed"}));
    TestFakes::DeterministicToolAuthorizerFake writeAuthorizer{
        "install-lmstudio-plugin", Domain::ToolEffect::Write, clock->monotonic};
    Manager::ManagerTelemetrySources sources;
    sources.projects = &registry;
    sources.projectWorkspaceAuthority = &readIssuer;
    sources.preferredForgeBinary = binary;
    sources.lmStudioDeployment = &deployment;
    sources.lmStudioReadAuthority = &readAuthority;
    sources.lmStudioWriteAuthority = &writeAuthority;
    sources.lmStudioActivationAuthority = &executionAuthority;
    sources.toolAuthorizer = &writeAuthorizer;
    {
        Manager::ManagerRequestDispatcher dispatcher{std::make_shared<FakeController>(), clock,
            Manager::ManagerTransportLimits{}, {}, sources};
        const auto response = dispatcher.dispatch(request(*clock, 856U,
            Manager::ManagerLmStudioRepairRequest{project}));
        const auto* repaired = responseValue<Manager::ManagerLmStudioSnapshot>(response);
        require(repaired && repaired->mcpConfigurationRegistered &&
            repaired->actionDetail == "Explicit owner repair completed" &&
            deployment.statusCalls() == 2U && deployment.deployCalls() == 1U &&
            deployment.activateCalls() == 0U,
            "explicit manual repair must still deploy and reinspect the complete registration");
        require(deployment.lastDeploymentRequest() &&
            deployment.lastDeploymentRequest()->projectId == project &&
            deployment.lastDeploymentRequest()->projectRoot == root &&
            deployment.lastDeploymentRequest()->preferredBinary == binary &&
            deployment.lastDeploymentRequest()->preserveForeignEntries &&
            writeAuthorizer.lastRequest() &&
            writeAuthorizer.lastRequest()->effect == Domain::ToolEffect::Write &&
            writeAuthorizer.lastRequest()->authority.authorityId == writeAuthority.authorityId(),
            "manual repair must retain exact project binding and Write authorization");
    }
    TestFakes::DeterministicToolAuthorizerFake executionAuthorizer{
        "install-lmstudio-plugin", Domain::ToolEffect::Execute, clock->monotonic};
    sources.toolAuthorizer = &executionAuthorizer;
    Manager::ManagerRequestDispatcher dispatcher{std::make_shared<FakeController>(), clock,
        Manager::ManagerTransportLimits{}, {}, sources};
    const auto response = dispatcher.dispatch(request(*clock, 857U,
        Manager::ManagerLmStudioActivateRequest{project}));
    const auto* activated = responseValue<Manager::ManagerLmStudioSnapshot>(response);
    require(activated && activated->connectionCheckPerformed &&
        activated->primaryConnectorReady && activated->fallbackConnectorReady &&
        activated->continuityConnectorReady &&
        activated->actionDetail == "Explicit owner activation completed" &&
        deployment.deployCalls() == 1U && deployment.activateCalls() == 1U &&
        executionAuthorizer.lastRequest() &&
        executionAuthorizer.lastRequest()->effect == Domain::ToolEffect::Execute &&
        deployment.lastAuthorization() &&
        deployment.lastAuthorization()->effect() == Domain::ToolEffect::Execute,
        "explicit manual activation must retain Execute authorization and all three role checks");
}

void testLegacyInstructionManifestMigratesToStableQueue()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const auto project = Domain::ProjectId::parse(uuidText(830U)).value();
    const auto legacyId = Domain::MemoryRecordId::parse(uuidText(831U)).value();
    const auto legacyFileId = Domain::MemoryRecordId::parse(uuidText(832U)).value();
    const auto migratedId = Domain::MemoryRecordId::parse(uuidText(833U)).value();
    const auto revision = Domain::Sha256Digest::parse(std::string(64U, 'd')).value();
    FixedHasher hasher{revision};
    TestFakes::RecordingProjectMemoryService memory;
    TestFakes::ProjectRegistryRepositoryFake registry{8U, clock->monotonic};
    require(static_cast<bool>(registry.seedDescriptor(
        Domain::ProjectMemoryDescriptor{project, "Legacy instruction project",
            std::nullopt,
            {Domain::PathText::create("D:\\Legacy").value()}})),
        "seed legacy instruction project");
    const auto now = clock->utc;
    const auto record = [&](const Domain::MemoryRecordId& id,
                            std::string kind, std::string body) {
        return Domain::ProjectMemoryRecord{
            id, project, 1U, std::move(kind), "legacy", "legacy", std::move(body),
            {"instruction-package"}, 1.0, 1.0, "legacy", std::nullopt,
            std::nullopt, now, now, now, std::nullopt, revision, false,
            Domain::ProjectMemorySchemaVersion};
    };
    const auto legacyBody = nlohmann::json{
        {"schema", "forge-instruction-package-v1"},
        {"package_name", "Legacy package"},
        {"package_path", "D:\\Legacy\\Instructions"},
        {"revision", revision.value()},
        {"files", nlohmann::json::array({{
            {"path", "START-HERE.md"},
            {"record_id", legacyFileId.value()}}})}}.dump();
    auto legacyRecord = record(legacyId, "instruction_package", legacyBody);
    auto legacyFile = record(legacyFileId, "instruction_package_file",
        "Follow the migrated project contract.");
    memory.listRecentResult.set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {{legacyRecord, 1.0}}, std::nullopt, false, 1024U,
            256U * 1024U, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    memory.getResult.set(
        Domain::Result<Domain::MemoryRecords>::success(Domain::MemoryRecords{
            project, {legacyFile}, 1024U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    const auto outcome = [&](const Domain::MemoryRecordId& id) {
        return Domain::MemoryWriteOutcome{
            project, id, 1U, Domain::MemoryWriteDisposition::Inserted,
            revision, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion};
    };
    memory.rememberBatchResult.set(
        Domain::Result<Domain::MemoryBatchOutcome>::success(
            Domain::MemoryBatchOutcome{project, {outcome(legacyFileId)},
                Domain::ProjectMemorySchemaVersion,
                Domain::ProjectMemoryCapabilityVersion}));
    memory.rememberResult.set(
        Domain::Result<Domain::MemoryWriteOutcome>::success(outcome(migratedId)));
    Manager::ManagerTelemetrySources sources;
    sources.projects = &registry;
    sources.projectMemory = &memory;
    sources.evidenceHasher = &hasher;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    const auto first = dispatcher.dispatch(request(*clock, 834U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* firstQueue = responseValue<
        Manager::ManagerInstructionPackageQueueSnapshot>(first);
    require(firstQueue != nullptr,
        std::string{"legacy manifest migration returns a queue snapshot: "} +
        (responseError(first) ? responseError(first)->message : "wrong result type"));
    require(firstQueue->rows.size() == 1U, "legacy manifest migration returns one row");
    require(firstQueue->rows.front().state == "active",
        "legacy manifest migration preserves the active state");
    require(firstQueue->rows.front().cursorEntry == 0U,
        "legacy manifest migration resets the cursor conservatively");
    require(firstQueue->rows.front().lastError &&
        firstQueue->rows.front().lastError->find("uncertain") != std::string::npos,
        "legacy manifest migration records cursor uncertainty");
    const auto stableRowId = firstQueue->rows.front().queueRowId;
    const auto firstKey = memory.lastRememberRequest()->write.idempotencyKey.value();

    const auto second = dispatcher.dispatch(request(*clock, 835U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* secondQueue = responseValue<
        Manager::ManagerInstructionPackageQueueSnapshot>(second);
    require(secondQueue != nullptr && secondQueue->rows.size() == 1U &&
        secondQueue->rows.front().queueRowId == stableRowId &&
        memory.lastRememberRequest()->write.idempotencyKey.value() == firstKey,
        "legacy migration is idempotent and preserves a stable queue identity");

    memory.getResult.set(Domain::Result<Domain::MemoryRecords>::success(
        Domain::MemoryRecords{project, {}, 0U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    const auto missingFile = dispatcher.dispatch(request(*clock, 836U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* incomplete = responseValue<Manager::ManagerInstructionPackageQueueSnapshot>(missingFile);
    require(incomplete && incomplete->rows.size() == 1U &&
        incomplete->rows.front().state == "needs_attention" &&
        incomplete->rows.front().coverageGapCount == 1U,
        "a missing pinned legacy file remains an explicit coverage gap");
    const auto missingEntry = nlohmann::json::parse(
        *memory.rememberBatchRequests().back().writes.front().body);
    require(missingEntry.at("interpretation") == "unreadable" &&
        missingEntry.at("derived_text").is_null() &&
        missingEntry.at("content_hash").is_null(),
        "migration does not fabricate interpreted empty text for a missing file");

    auto emptyOrder = record(migratedId, "instruction_package_queue_order",
        nlohmann::json{{"schema", "forge-instruction-package-order-v1"},
            {"project_id", project.value()}, {"rows", nlohmann::json::array()}}.dump());
    memory.listRecentByKind["instruction_package_queue_order"].set(
        Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
            project, {{emptyOrder, 1.0}}, std::nullopt, false, 1024U,
            256U * 1024U, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    const auto writesBeforeRemoval = memory.callCount(TestFakes::ProjectMemoryCall::Remember);
    const auto removed = dispatcher.dispatch(request(*clock, 837U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* removedQueue = responseValue<Manager::ManagerInstructionPackageQueueSnapshot>(removed);
    require(removedQueue && removedQueue->rows.empty() &&
        memory.callCount(TestFakes::ProjectMemoryCall::Remember) == writesBeforeRemoval,
        "a saved empty queue order suppresses resurrection of removed legacy instructions");

    memory.listRecentByKind.erase("instruction_package_queue_order");
    memory.rememberBatchResult.set(Domain::Result<Domain::MemoryBatchOutcome>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, "fixture migration write failure")));
    const auto failed = dispatcher.dispatch(request(*clock, 838U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    require(responseError(failed) && responseError(failed)->message == "fixture migration write failure",
        "migration persistence failures are surfaced instead of reporting no package");

    auto largeManifest = nlohmann::json::parse(legacyBody);
    largeManifest["files"] = nlohmann::json::array();
    for (std::size_t index{}; index < 101U; ++index) {
        largeManifest["files"].push_back({
            {"path", "instruction-" + std::to_string(index) + ".md"},
            {"record_id", legacyFileId.value()}});
    }
    legacyRecord.body = largeManifest.dump();
    memory.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
        Domain::MemoryPage{project, {{legacyRecord, 1.0}}, std::nullopt,
            false, 1024U, 256U * 1024U, Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    memory.getResult.set(Domain::Result<Domain::MemoryRecords>::success(
        Domain::MemoryRecords{project, {legacyFile}, 1024U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    memory.rememberBatchResult.set(Domain::Result<Domain::MemoryBatchOutcome>::success(
        Domain::MemoryBatchOutcome{project, {outcome(legacyFileId)},
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion}));
    const auto readsBeforeLarge = memory.callCount(TestFakes::ProjectMemoryCall::Get);
    const auto batchesBeforeLarge = memory.rememberBatchRequests().size();
    const auto large = dispatcher.dispatch(request(*clock, 839U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* largeQueue = responseValue<Manager::ManagerInstructionPackageQueueSnapshot>(large);
    require(largeQueue && largeQueue->rows.size() == 1U &&
        largeQueue->rows.front().entryCount == 101U &&
        memory.callCount(TestFakes::ProjectMemoryCall::Get) - readsBeforeLarge == 101U,
        "legacy reads stay below the project-memory ID and aggregate response limits");
    require(memory.rememberBatchRequests().size() - batchesBeforeLarge == 21U,
        "legacy migration persists entries in bounded batches");
    for (auto index = batchesBeforeLarge; index < memory.rememberBatchRequests().size(); ++index) {
        require(memory.rememberBatchRequests()[index].writes.size() <= 5U,
            "each migration batch stays inside the persistence page bound");
    }

    auto updatedBody = nlohmann::json::parse(*memory.lastRememberRequest()->write.body);
    updatedBody["cursor"] = {{"entry", 7U}, {"byte_offset", 99U}};
    updatedBody["state"] = "paused";
    updatedBody["order"] = 2048U;
    updatedBody["last_error"] = nullptr;
    auto updatedRecord = record(migratedId, "instruction_package_queue", updatedBody.dump());
    auto deduplicated = outcome(migratedId);
    deduplicated.disposition = Domain::MemoryWriteDisposition::Deduplicated;
    memory.rememberResult.set(Domain::Result<Domain::MemoryWriteOutcome>::success(deduplicated));
    memory.getById[migratedId.value()].set(Domain::Result<Domain::MemoryRecords>::success(
        Domain::MemoryRecords{project, {updatedRecord}, 1024U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion, Domain::ProjectMemoryCapabilityVersion}));
    const auto concurrent = dispatcher.dispatch(request(*clock, 840U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* persistedQueue = responseValue<Manager::ManagerInstructionPackageQueueSnapshot>(concurrent);
    require(persistedQueue && persistedQueue->rows.size() == 1U &&
        persistedQueue->rows.front().cursorEntry == 7U &&
        persistedQueue->rows.front().cursorByteOffset == 99U &&
        persistedQueue->rows.front().order == 2048U &&
        persistedQueue->rows.front().state == "paused",
        "A concurrent deduplicated migration reads the persisted cursor and queue state");
    memory.getById[migratedId.value()].set(Domain::Result<Domain::MemoryRecords>::success(
        Domain::MemoryRecords{project, {}, 0U, 256U * 1024U,
            Domain::ProjectMemorySchemaVersion, Domain::ProjectMemoryCapabilityVersion}));
    const auto tombstoned = dispatcher.dispatch(request(*clock, 841U,
        Manager::ManagerInstructionPackageQueueRequest{
            project, Manager::ManagerInstructionPackageQueueAction::List}));
    const auto* tombstonedQueue = responseValue<Manager::ManagerInstructionPackageQueueSnapshot>(tombstoned);
    require(tombstonedQueue && tombstonedQueue->rows.empty(),
        "A concurrently tombstoned deduplicated queue is never fabricated as active");
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

    for (const auto& confirmation : {std::string{"wrong confirmation"}, "RESET PROJECT DATA " + project.value()}) {
        requireError(dispatcher.dispatch(request(*clock, 84U,
            Manager::ManagerMaintenanceRequest{Manager::ManagerMaintenanceScope::ProjectAllData,
                project, confirmation})), Domain::ErrorCodes::InvalidRequest, "removed scope reset rejected");
    }
    require(memory.callCount(TestFakes::ProjectMemoryCall::ResetProjectMemory) == 0U &&
        continuity.callCount(TestFakes::ContinuityCall::ResetProjectContinuity) == 0U,
        "removed reset never changes stores");
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

void testToolsSnapshotReadsLiveSettingsAndPropagatesFailure()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    TestFakes::BoundedToolCatalogFake catalog{{Domain::McpToolDescriptor{
        Domain::ToolDescriptor{
            "shell", "Native shell", "shell", Domain::ToolEffect::Execute,
            Domain::ToolAvailability::Available, true, true},
        R"({"type":"object"})"}}};
    Manager::ManagerTelemetrySources sources;
    sources.tools = &catalog;
    sources.shellEnabled = true;
    Manager::ManagerRequestDispatcher dispatcher{
        controller, clock, Manager::ManagerTransportLimits{}, {}, sources};

    controller->currentSettings.shellEnabled = false;
    const auto disabled = dispatcher.dispatch(request(
        *clock, 6U, Manager::ManagerToolsRequest{}));
    const auto* disabledTools = responseValue<Manager::ManagerToolsSnapshot>(disabled);
    require(disabledTools != nullptr && !disabledTools->shellEnabled &&
        disabledTools->tools.size() == 1U &&
        disabledTools->tools.front().name == "shell",
        "tools snapshot uses current disabled policy rather than startup telemetry");

    controller->currentSettings.shellEnabled = true;
    const auto enabled = dispatcher.dispatch(request(
        *clock, 7U, Manager::ManagerToolsRequest{}));
    const auto* enabledTools = responseValue<Manager::ManagerToolsSnapshot>(enabled);
    require(enabledTools != nullptr && enabledTools->shellEnabled,
        "tools snapshot reflects shell access enabled after construction");

    controller->currentSettings.shellEnabled = false;
    const auto disabledAgain = dispatcher.dispatch(request(
        *clock, 8U, Manager::ManagerToolsRequest{}));
    const auto* disabledAgainTools =
        responseValue<Manager::ManagerToolsSnapshot>(disabledAgain);
    require(disabledAgainTools != nullptr && !disabledAgainTools->shellEnabled,
        "tools snapshot reflects shell access revoked after construction");

    controller->failSettings_ = true;
    requireError(dispatcher.dispatch(request(
        *clock, 9U, Manager::ManagerToolsRequest{})),
        Domain::ErrorCodes::DatabaseBusy, "tools snapshot settings failure");
    require(controller->settingsCalls_.load() == 4U,
        "each tools snapshot reads live settings exactly once");
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
        testToolsSnapshotReadsLiveSettingsAndPropagatesFailure();
        testWorkerScopeBrokerPreservesOnlyCurrentCallerGrants();
        testManagedRunEndpointsAreRemoved();
        testAutomaticContinuityPreferenceIsProjectProviderScopedAndDurable();
        testRunHistoryIsBoundToSelectedProject();
        testActivityAndDoctorProjectSimplifiedWorkflowState();
        testDurableEvidenceIsRedactedAndProjectBound();
        testNativeTaskCheckRequiresExactVerifiedRunAndPersistsReceipt();
        testProjectWorkflowKeepsExactProjectIdentity();
        testInstructionQueueRepeatedOrderSurvivesRepositoryReopen();
        testInstructionQueueRemovalIsImmediateForEveryState();
        testInstructionQueueLegacyDeletionAndReadoptionAreDurable();
        testInstructionQueueMutationFailuresRemainVisible();
        testInstructionPackagePreviewAndActivationStayProjectBound();
        testLegacyInstructionManifestMigratesToStableQueue();
        testLmStudioBindingUsesRegisteredAuthorizedProject();
        testExplicitLmStudioRepairAndActivationRemainAuthorized();
        testMaintenanceRequiresExactScopeAndCoordinatesStores();
        testTelemetryCannotReadRemovedManagedRuns();
        testDuplicateCapacityAndCancellationBypass();
        testShutdownOrderingAndClosedAdmission();
        testShutdownFailureAndEnvelopeValidation();
        testBoundedCloseDefersControllerShutdownUntilIdle();
        testRacingReleaseAndShutdownClosesExactlyOnce();
        testConstructionRejectsNullDependencies();
        std::cout << "Manager request dispatcher tests passed: 25 groups\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Manager request dispatcher tests failed: "
                  << failure.what() << '\n';
        return 1;
    }
}
