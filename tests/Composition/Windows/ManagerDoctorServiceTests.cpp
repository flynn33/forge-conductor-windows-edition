#include <Windows.h>

#include "ManagerDoctorService.h"
#include "UnavailableTelemetryService.h"

#include "Fakes/ApplicationServiceFakes.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/TelemetryFakes.h"
#include "ForgeConductor/Application/AgentCatalog.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Domain/DiagnosticsModels.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Composition = ForgeConductor::Composition::Windows;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Composition::ManagerDoctorService>);
static_assert(std::is_base_of_v<
              Contracts::IDoctorService,
              Composition::ManagerDoctorService>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerDoctorService>);
static_assert(!std::is_move_constructible_v<
              Composition::ManagerDoctorService>);
static_assert(std::is_final_v<Composition::WindowsManagerDoctorPlatformProbe>);
static_assert(std::is_base_of_v<
              Composition::IManagerDoctorPlatformProbe,
              Composition::WindowsManagerDoctorPlatformProbe>);
static_assert(
    Composition::ManagerDoctorService::RequiredAgentCount == 10U);

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

class Clock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utcValue{Domain::UtcTimePoint{} + 100s};
    Domain::MonotonicTimePoint monotonicValue{
        Domain::MonotonicTimePoint{} + 10s};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utcValue;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonicValue;
    }
};

[[nodiscard]] Domain::OperationContext context(
    const Domain::MonotonicTimePoint deadline =
        Domain::MonotonicTimePoint{} + 20s,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        deadline,
        cancellation,
        parse<Domain::CorrelationId>("manager-doctor-test")};
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

[[nodiscard]] Domain::AgentSpec agent(const std::string_view id)
{
    return Domain::AgentSpec{
        parse<Domain::AgentId>(id),
        "Doctor test agent",
        "Provides deterministic catalog evidence.",
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        "",
        "builtin"};
}

[[nodiscard]] std::vector<Domain::AgentSpec> agents(
    const std::size_t count)
{
    std::vector<Domain::AgentSpec> values;
    values.reserve(count);
    for (std::size_t index{}; index < count; ++index) {
        if (index < ForgeConductor::Application::AgentCatalog::MandatoryIds.size()) {
            values.push_back(agent(
                ForgeConductor::Application::AgentCatalog::MandatoryIds[index]));
        } else {
            values.push_back(agent(
                "doctor-extra-agent-" + std::to_string(index)));
        }
    }
    return values;
}

[[nodiscard]] Domain::TelemetryHealthReport healthyTelemetry()
{
    return Domain::TelemetryHealthReport{
        true,
        "forge-telemetry",
        "windows-native",
        false,
        "continuous-native",
        "Windows native collectors",
        "WinUI 3",
        false};
}

[[nodiscard]] Domain::TelemetrySnapshot telemetrySnapshot(
    const double cpuPercent = 20.0)
{
    Domain::CpuMetrics cpu;
    cpu.percent = Domain::makeAvailableTelemetryMetric(
        cpuPercent, Domain::UtcTimePoint{} + 100s, "test.fixture");
    cpu.idlePercent = Domain::makeAvailableTelemetryMetric(
        100.0 - cpuPercent,
        Domain::UtcTimePoint{} + 100s,
        "test.fixture");
    cpu.logicalProcessorCount = Domain::makeAvailableTelemetryMetric<std::uint32_t>(
        1U, Domain::UtcTimePoint{} + 100s, "test.fixture");
    cpu.physicalCoreCount = Domain::makeAvailableTelemetryMetric<std::uint32_t>(
        1U, Domain::UtcTimePoint{} + 100s, "test.fixture");
    cpu.perLogicalProcessor = Domain::makeAvailableTelemetryMetric(
        std::vector<double>{cpuPercent},
        Domain::UtcTimePoint{} + 100s,
        "test.fixture");

    Domain::RamMetrics ram;
    ram.percent = Domain::makeAvailableTelemetryMetric(
        30.0, Domain::UtcTimePoint{} + 100s, "test.fixture");
    ram.pressurePercent = Domain::makeAvailableTelemetryMetric(
        30.0, Domain::UtcTimePoint{} + 100s, "test.fixture");

    Domain::SystemMetrics system{
        Domain::UtcTimePoint{} + 100s,
        "doctor-host",
        "windows",
        "x64",
        std::move(cpu),
        std::move(ram),
        {},
        Domain::DiskIoMetrics{},
        {},
        {},
        Domain::PowerMetrics{}};
    Domain::ForgeSnapshot forge{
        Domain::UtcTimePoint{} + 100s,
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
        Domain::UtcTimePoint{} + 101s,
        {},
        "windows-native"};
}

[[nodiscard]] Contracts::WorkspaceAuthority readAuthority()
{
    Fakes::DeterministicWorkspaceAuthority issuer{
        parse<Domain::AuthorityId>(
            "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        parse<Domain::ClientId>("manager-doctor"),
        {path("C:\\Forge")},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        false,
        7U};
    issuer.setNow(Domain::MonotonicTimePoint{} + 10s);
    return take(issuer.authorityFor(
        parse<Domain::ProjectId>(
            "cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        context()));
}

[[nodiscard]] Domain::LMStudioPluginStatus lmStudioStatus(
    const bool primary = true,
    const bool fallback = true,
    const bool registered = true,
    const bool executable = true)
{
    return Domain::LMStudioPluginStatus{
        primary,
        fallback,
        registered,
        path("C:\\Forge\\bin\\forge.exe"),
        executable,
        true,
        path("C:\\Forge\\lmstudio\\primary"),
        path("C:\\Forge\\lmstudio\\fallback"),
        path("C:\\Forge\\lmstudio\\mcp.json"),
        std::nullopt,
        "ready"};
}

class RecordingPlatformProbe final
    : public Composition::IManagerDoctorPlatformProbe {
public:
    RecordingPlatformProbe()
        : snapshot_{Composition::ManagerDoctorPlatformSnapshot{
              true,
              true,
              path("C:\\Program Files\\Git\\cmd\\git.exe"),
              true,
              {}}}
    {
    }

    [[nodiscard]] Domain::Result<Composition::ManagerDoctorPlatformSnapshot>
    inspect(
        const Domain::PathText& dataRoot,
        const Domain::PathText& centralStorePath,
        const Domain::PathText& installedBinaryPath,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++calls_;
            lastDataRoot_ = dataRoot;
            lastCentralStorePath_ = centralStorePath;
            lastInstalledBinaryPath_ = installedBinaryPath;
            lastContext_ = operationContext;
            inspectEntered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this]() noexcept { return !blockInspect_; });
            if (error_) {
                return Domain::Result<
                    Composition::ManagerDoctorPlatformSnapshot>::failure(
                    *error_);
            }
            return Domain::Result<
                Composition::ManagerDoctorPlatformSnapshot>::success(
                snapshot_);
        } catch (...) {
            return Domain::Result<
                Composition::ManagerDoctorPlatformSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording Doctor platform probe failed."));
        }
    }

    void setSnapshot(Composition::ManagerDoctorPlatformSnapshot snapshot)
    {
        std::lock_guard lock{mutex_};
        snapshot_ = std::move(snapshot);
        error_.reset();
    }

    void setError(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        error_ = std::move(error);
    }

    void blockNextInspect()
    {
        std::lock_guard lock{mutex_};
        blockInspect_ = true;
        inspectEntered_ = false;
    }

    void waitUntilInspectBlocked()
    {
        std::unique_lock lock{mutex_};
        require(
            changed_.wait_for(
                lock,
                2s,
                [this]() noexcept {
                    return blockInspect_ && inspectEntered_;
                }),
            "Doctor platform probe did not block");
    }

    void releaseInspect()
    {
        std::lock_guard lock{mutex_};
        blockInspect_ = false;
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t calls() const
    {
        std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::optional<Domain::OperationContext> lastContext() const
    {
        std::lock_guard lock{mutex_};
        return lastContext_;
    }

    [[nodiscard]] std::optional<Domain::PathText> lastDataRoot() const
    {
        std::lock_guard lock{mutex_};
        return lastDataRoot_;
    }

    [[nodiscard]] std::optional<Domain::PathText> lastCentralStorePath() const
    {
        std::lock_guard lock{mutex_};
        return lastCentralStorePath_;
    }

    [[nodiscard]] std::optional<Domain::PathText> lastInstalledBinaryPath() const
    {
        std::lock_guard lock{mutex_};
        return lastInstalledBinaryPath_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Composition::ManagerDoctorPlatformSnapshot snapshot_;
    std::optional<Domain::Error> error_;
    std::optional<Domain::PathText> lastDataRoot_;
    std::optional<Domain::PathText> lastCentralStorePath_;
    std::optional<Domain::PathText> lastInstalledBinaryPath_;
    std::optional<Domain::OperationContext> lastContext_;
    std::size_t calls_{};
    bool blockInspect_{};
    bool inspectEntered_{};
};

class RecordingLMStudioService final
    : public Contracts::ILMStudioDeploymentService {
public:
    void setStatus(Domain::LMStudioPluginStatus status)
    {
        status_ = std::move(status);
        error_.reset();
    }

    void setError(Domain::Error error)
    {
        error_ = std::move(error);
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioPluginStatus> status(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            ++statusCalls_;
            lastRequest_ = request;
            lastAuthority_.emplace(readAuthority);
            lastContext_ = operationContext;
            if (shutdown_) {
                return Domain::Result<Domain::LMStudioPluginStatus>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The recording LM Studio service is shut down."));
            }
            if (error_) {
                return Domain::Result<Domain::LMStudioPluginStatus>::failure(
                    *error_);
            }
            if (!status_) {
                return Domain::Result<Domain::LMStudioPluginStatus>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "No recording LM Studio status was configured."));
            }
            return Domain::Result<Domain::LMStudioPluginStatus>::success(
                *status_);
        } catch (...) {
            return Domain::Result<Domain::LMStudioPluginStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording LM Studio status failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::LMStudioDeploymentRequest&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::LMStudioInstallResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio deployment is not expected in Doctor tests."));
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioHostActivationRequest&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio activation is not expected in Doctor tests."));
    }

    void cancel(const Domain::OperationId&) noexcept override
    {
        ++cancelCalls_;
    }

    void shutdown() noexcept override
    {
        ++shutdownCalls_;
        shutdown_ = true;
    }

    [[nodiscard]] std::size_t statusCalls() const noexcept
    {
        return statusCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        return shutdownCalls_;
    }

    [[nodiscard]] const std::optional<Domain::LMStudioDeploymentRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastAuthority() const noexcept
    {
        return lastAuthority_;
    }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return lastContext_;
    }

private:
    std::optional<Domain::LMStudioPluginStatus> status_;
    std::optional<Domain::Error> error_;
    std::optional<Domain::LMStudioDeploymentRequest> lastRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastAuthority_;
    std::optional<Domain::OperationContext> lastContext_;
    std::size_t statusCalls_{};
    std::size_t cancelCalls_{};
    std::size_t shutdownCalls_{};
    bool shutdown_{};
};

struct Fixture final {
    Clock clock;
    RecordingPlatformProbe probe;
    Fakes::RecordingAgentCatalogFake catalog;
    Fakes::RecordingAgentSessionRepositoryFake repository;
    Fakes::RecordingTelemetryService telemetry;
    RecordingLMStudioService lmStudio;
    Domain::PathText dataRoot{path("C:\\Forge")};
    Domain::PathText centralStorePath{path("C:\\Forge\\store.sqlite")};
    Domain::PathText installedBinaryPath{
        path("C:\\Forge\\bin\\forge.exe")};
    Domain::ResourceBudgets resourceBudgets{
        Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB)};
    Contracts::WorkspaceAuthority authority{readAuthority()};

    explicit Fixture(
        const std::size_t catalogOutputItemsMaximum =
            Fakes::DefaultBoundaryCaptureItemsMaximum,
        const std::size_t catalogTextBytesMaximum =
            Fakes::DefaultBoundaryCaptureTextBytesMaximum)
        : catalog{
              catalogOutputItemsMaximum,
              catalogTextBytesMaximum,
              clock.monotonicValue}
    {
        repository.setNow(clock.monotonicValue);
        telemetry.setNow(clock.monotonicValue);
        catalog.allResult.set(
            Domain::Result<std::vector<Domain::AgentSpec>>::success(
                agents(Composition::ManagerDoctorService::RequiredAgentCount)));
        repository.quickCheckResult.set(Domain::Result<void>::success());
        telemetry.startResult.set(Domain::Result<void>::success());
        telemetry.healthResult.set(
            Domain::Result<Domain::TelemetryHealthReport>::success(
                healthyTelemetry()));
        telemetry.sampleResult.set(
            Domain::Result<Contracts::ITelemetryService::Snapshot>::success(
                std::make_shared<const Domain::TelemetrySnapshot>(
                    telemetrySnapshot())));
        lmStudio.setStatus(lmStudioStatus());
        take(telemetry.start(context()));
    }

    [[nodiscard]] std::unique_ptr<Composition::ManagerDoctorService> create()
    {
        return createWithTelemetry(telemetry);
    }

    [[nodiscard]] std::unique_ptr<Composition::ManagerDoctorService>
    createWithTelemetry(Contracts::ITelemetryService& telemetryService)
    {
        return take(createConfigured(configuration(), telemetryService));
    }

    [[nodiscard]] Composition::ManagerDoctorServiceConfiguration
    configuration() const
    {
        return Composition::ManagerDoctorServiceConfiguration{
            "0.9.0",
            dataRoot,
            centralStorePath,
            installedBinaryPath,
            resourceBudgets,
            authority};
    }

    [[nodiscard]] Domain::Result<
        std::unique_ptr<Composition::ManagerDoctorService>>
    createConfigured(
        Composition::ManagerDoctorServiceConfiguration configuration,
        Contracts::ITelemetryService& telemetryService)
    {
        return Composition::ManagerDoctorService::create(
            std::move(configuration),
            probe,
            catalog,
            repository,
            telemetryService,
            lmStudio,
            clock);
    }

    [[nodiscard]] Domain::Result<
        std::unique_ptr<Composition::ManagerDoctorService>>
    createConfigured(
        Composition::ManagerDoctorServiceConfiguration configuration)
    {
        return createConfigured(std::move(configuration), telemetry);
    }
};

[[nodiscard]] const Domain::DoctorCheck& check(
    const Domain::DoctorReport& report,
    const std::string_view name)
{
    const auto found = std::find_if(
        report.checks.begin(),
        report.checks.end(),
        [name](const Domain::DoctorCheck& candidate) noexcept {
            return candidate.name == name;
        });
    require(found != report.checks.end(), "Doctor check was missing");
    return *found;
}

void exactContractOrderAndForwarding()
{
    Fixture fixture;
    auto service = fixture.create();
    const auto operationContext = context();
    const auto report = take(service->run(operationContext));

    const std::vector<std::string_view> expectedNames{
        "home_layout",
        "sqlite_store",
        "agent_catalog",
        "sqlite_query",
        "git_available",
        "telemetry_native",
        "telemetry_runtime",
        "telemetry_snapshot",
        "windows_binary_install",
        "no_legacy_forge_serve",
        "lm_studio_native_stdio",
        "lm_studio_mcp_plugin"};
    const std::vector<bool> expectedHardness{
        true,
        true,
        true,
        true,
        true,
        true,
        true,
        true,
        false,
        true,
        false,
        false};

    require(report.ok, "all healthy hard checks did not pass");
    require(report.version == "0.9.0", "Doctor version changed");
    require(report.home == fixture.dataRoot, "Doctor home changed");
    require(report.binaryInstalled, "installed binary was not projected");
    require(
        report.binaryPath == fixture.installedBinaryPath,
        "installed binary path changed");
    require(
        report.checks.size() == expectedNames.size(),
        "Doctor check count changed");
    for (std::size_t index{}; index < expectedNames.size(); ++index) {
        require(
            report.checks[index].name == expectedNames[index],
            "Doctor check order changed");
        require(
            report.checks[index].hard == expectedHardness[index],
            "Doctor check hardness changed");
        require(report.checks[index].ok, "healthy Doctor check failed");
    }

    require(fixture.probe.calls() == 1U, "platform probe call count changed");
    require(
        fixture.probe.lastDataRoot() == fixture.dataRoot &&
            fixture.probe.lastCentralStorePath() == fixture.centralStorePath &&
            fixture.probe.lastInstalledBinaryPath() ==
                fixture.installedBinaryPath,
        "configured paths were not forwarded to the platform probe");
    require(
        fixture.probe.lastContext() &&
            sameContext(*fixture.probe.lastContext(), operationContext),
        "operation context was not forwarded to the platform probe");
    require(
        fixture.catalog.lastContext() &&
            sameContext(*fixture.catalog.lastContext(), operationContext),
        "operation context was not forwarded to the agent catalog");
    require(
        fixture.repository.lastContext() &&
            sameContext(*fixture.repository.lastContext(), operationContext),
        "operation context was not forwarded to the session repository");
    require(
        fixture.lmStudio.lastRequest() &&
            fixture.lmStudio.lastRequest()->preferredBinary &&
            *fixture.lmStudio.lastRequest()->preferredBinary ==
                fixture.installedBinaryPath &&
            fixture.lmStudio.lastRequest()->preserveForeignEntries,
        "LM Studio status request changed its binary or preservation policy");
    require(
        fixture.lmStudio.lastAuthority() &&
            fixture.lmStudio.lastAuthority()->authorityId() ==
                fixture.authority.authorityId() &&
            fixture.lmStudio.lastAuthority()->projectId() ==
                fixture.authority.projectId() &&
            fixture.lmStudio.lastAuthority()->intent() ==
                Domain::FileAccess::Read &&
            fixture.lmStudio.lastAuthority()->generation() ==
                fixture.authority.generation(),
        "LM Studio read authority was not forwarded unchanged");
    require(
        fixture.lmStudio.lastContext() &&
            sameContext(*fixture.lmStudio.lastContext(), operationContext),
        "operation context was not forwarded to LM Studio status");
    require(
        fixture.telemetry.healthCalls() == 1U &&
            fixture.telemetry.sampleCalls() == 1U &&
            fixture.telemetry.lastForceForgeComposition(),
        "Doctor telemetry evidence calls changed");
}

void hardFailuresFailTheReportAndSoftFailuresDoNot()
{
    {
        Fixture fixture;
        fixture.probe.setSnapshot(
            Composition::ManagerDoctorPlatformSnapshot{
                false,
                true,
                path("C:\\Git\\git.exe"),
                true,
                {}});
        const auto report = take(fixture.create()->run(context()));
        require(!check(report, "home_layout").ok, "missing home passed");
        require(!report.ok, "missing hard home did not fail the report");
    }
    {
        Fixture fixture;
        fixture.probe.setSnapshot(
            Composition::ManagerDoctorPlatformSnapshot{
                true,
                false,
                path("C:\\Git\\git.exe"),
                true,
                {}});
        const auto report = take(fixture.create()->run(context()));
        require(!check(report, "sqlite_store").ok, "missing store passed");
        require(!report.ok, "missing hard store did not fail the report");
    }
    {
        Fixture fixture;
        fixture.catalog.allResult.set(
            Domain::Result<std::vector<Domain::AgentSpec>>::success(agents(9U)));
        const auto report = take(fixture.create()->run(context()));
        require(!check(report, "agent_catalog").ok, "short catalog passed");
        require(!report.ok, "short hard catalog did not fail the report");
    }
    {
        Fixture fixture;
        fixture.repository.quickCheckResult.set(
            Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DatabaseBusy,
                "busy")));
        const auto report = take(fixture.create()->run(context()));
        require(!check(report, "sqlite_query").ok, "failed query passed");
        require(!report.ok, "failed hard query did not fail the report");
    }
    {
        Fixture fixture;
        fixture.probe.setSnapshot(
            Composition::ManagerDoctorPlatformSnapshot{
                true,
                true,
                std::nullopt,
                true,
                {}});
        const auto report = take(fixture.create()->run(context()));
        require(!check(report, "git_available").ok, "missing Git passed");
        require(!report.ok, "missing hard Git did not fail the report");
    }
    {
        Fixture fixture;
        auto health = healthyTelemetry();
        health.ok = false;
        fixture.telemetry.healthResult.set(
            Domain::Result<Domain::TelemetryHealthReport>::success(
                std::move(health)));
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "telemetry_native").ok,
            "unhealthy native telemetry passed");
        require(!report.ok, "hard native telemetry failure was ignored");
    }
    {
        Fixture fixture;
        auto health = healthyTelemetry();
        health.runtime = "node";
        fixture.telemetry.healthResult.set(
            Domain::Result<Domain::TelemetryHealthReport>::success(
                std::move(health)));
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "telemetry_runtime").ok,
            "non-native telemetry runtime passed");
        require(!report.ok, "hard telemetry runtime failure was ignored");
    }
    {
        Fixture fixture;
        fixture.telemetry.sampleResult.set(
            Domain::Result<Contracts::ITelemetryService::Snapshot>::success(
                std::make_shared<const Domain::TelemetrySnapshot>(
                    telemetrySnapshot(101.0))));
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "telemetry_snapshot").ok,
            "invalid telemetry snapshot passed");
        require(!report.ok, "hard telemetry snapshot failure was ignored");
    }
    {
        Fixture fixture;
        fixture.probe.setSnapshot(
            Composition::ManagerDoctorPlatformSnapshot{
                true,
                true,
                path("C:\\Git\\git.exe"),
                true,
                {"forge-serve.cmd"}});
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "no_legacy_forge_serve").ok,
            "legacy launcher passed");
        require(!report.ok, "hard legacy-launcher failure was ignored");
    }
    {
        Fixture fixture;
        fixture.probe.setSnapshot(
            Composition::ManagerDoctorPlatformSnapshot{
                true,
                true,
                path("C:\\Git\\git.exe"),
                false,
                {}});
        fixture.lmStudio.setStatus(
            lmStudioStatus(false, false, false, false));
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "windows_binary_install").ok &&
                !check(report, "lm_studio_native_stdio").ok &&
                !check(report, "lm_studio_mcp_plugin").ok,
            "soft failures were not represented");
        require(report.ok, "soft failures changed overall Doctor success");
    }
}

void catalogRequiresExactMandatoryIdentitiesAndUniqueness()
{
    {
        Fixture fixture;
        auto substituted = agents(
            Composition::ManagerDoctorService::RequiredAgentCount);
        substituted.back() = agent("custom-substitute");
        fixture.catalog.allResult.set(
            Domain::Result<std::vector<Domain::AgentSpec>>::success(
                std::move(substituted)));
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "agent_catalog").ok,
            "catalog with a substituted mandatory identity passed");
        require(
            !report.ok,
            "missing mandatory identity did not fail the Doctor report");
    }
    {
        Fixture fixture;
        auto duplicated = agents(
            Composition::ManagerDoctorService::RequiredAgentCount);
        duplicated.push_back(agent(
            ForgeConductor::Application::AgentCatalog::MandatoryIds.front()));
        fixture.catalog.allResult.set(
            Domain::Result<std::vector<Domain::AgentSpec>>::success(
                std::move(duplicated)));
        const auto report = take(fixture.create()->run(context()));
        require(
            !check(report, "agent_catalog").ok,
            "catalog with a duplicate identity passed");
        require(
            !report.ok,
            "duplicate catalog identity did not fail the Doctor report");
    }
    {
        Fixture fixture;
        fixture.catalog.allResult.set(
            Domain::Result<std::vector<Domain::AgentSpec>>::success(
                agents(
                    Composition::ManagerDoctorService::RequiredAgentCount +
                    1U)));
        const auto report = take(fixture.create()->run(context()));
        require(
            check(report, "agent_catalog").ok,
            "unique custom agent invalidated the mandatory catalog");
        require(report.ok, "valid extended catalog failed the Doctor report");
    }
}

void acceptsRealtimeRuntimeAndProjectsUnavailableTelemetryTruthfully()
{
    {
        Fixture fixture;
        auto health = healthyTelemetry();
        health.runtime = "windows-native-realtime";
        fixture.telemetry.healthResult.set(
            Domain::Result<Domain::TelemetryHealthReport>::success(
                std::move(health)));
        const auto report = take(fixture.create()->run(context()));
        require(
            check(report, "telemetry_runtime").ok,
            "future native realtime runtime was rejected");
    }

    Fixture fixture;
    Composition::UnavailableTelemetryService unavailable{fixture.clock};
    auto service = fixture.createWithTelemetry(unavailable);
    const auto report = take(service->run(context()));
    require(!report.ok, "unavailable telemetry produced a healthy report");
    require(
        !check(report, "telemetry_native").ok,
        "unavailable telemetry passed the native health check");
    require(
        check(report, "telemetry_runtime").ok,
        "truthful unavailable native runtime was rejected");
    require(
        !check(report, "telemetry_snapshot").ok,
        "unavailable telemetry fabricated a snapshot");
    require(
        report.telemetry.runtime == "windows-native" &&
            report.telemetry.mode == "unavailable" &&
            !report.telemetry.ok,
        "unavailable telemetry health was not preserved");

    service->shutdown();
    require(
        take(unavailable.health(context())).mode == "unavailable",
        "Doctor shutdown owned the unavailable telemetry dependency");
}

void dependencyErrorsAreProjectedSafelyAndAuthorityErrorsPropagate()
{
    constexpr std::string_view secret = "credential-material-must-not-escape";
    const std::string sensitiveMessage{
        std::string{secret} + std::string(16U * 1024U, 'x')};

    Fixture fixture;
    fixture.probe.setError(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        sensitiveMessage,
        true,
        "private-platform-evidence"));
    fixture.catalog.allResult.set(
        Domain::Result<std::vector<Domain::AgentSpec>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                sensitiveMessage,
                false,
                "private-catalog-evidence")));
    fixture.repository.quickCheckResult.set(
        Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DatabaseBusy,
            sensitiveMessage,
            true,
            "private-store-evidence")));
    fixture.telemetry.healthResult.set(
        Domain::Result<Domain::TelemetryHealthReport>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                sensitiveMessage,
                true,
                "private-telemetry-evidence")));
    fixture.telemetry.sampleResult.set(
        Domain::Result<Contracts::ITelemetryService::Snapshot>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                sensitiveMessage,
                true,
                "private-snapshot-evidence")));
    fixture.lmStudio.setError(Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        sensitiveMessage,
        true,
        "private-lm-evidence"));

    const auto report = take(fixture.create()->run(context()));
    require(report.checks.size() == 12U, "error report lost Doctor rows");
    for (const auto& item : report.checks) {
        require(
            item.detail.size() <=
                Domain::MaximumDiagnosticFlattenedFieldBytes,
            "Doctor detail exceeded its diagnostic projection bound");
        require(
            item.detail.find(secret) == std::string::npos,
            "Doctor detail exposed a dependency error message");
        require(
            item.detail.find("private-") == std::string::npos,
            "Doctor detail exposed a dependency evidence identifier");
    }

    Fixture authorityFixture;
    authorityFixture.lmStudio.setError(Domain::makeError(
        Domain::ErrorCodes::Unauthorized,
        "The read authority was rejected."));
    requireError(
        authorityFixture.create()->run(context()),
        Domain::ErrorCodes::Unauthorized,
        "LM Studio authority failure was disguised as availability");
}

void rejectsOversizedCatalogAndInvalidTelemetryBudgets()
{
    {
        Fixture fixture{
            ForgeConductor::Application::AgentCatalog::MaximumEntries + 1U,
            128U * 1024U};
        fixture.catalog.allResult.set(
            Domain::Result<std::vector<Domain::AgentSpec>>::success(agents(
                ForgeConductor::Application::AgentCatalog::MaximumEntries +
                1U)));
        requireError(
            fixture.create()->run(context()),
            Domain::ErrorCodes::IntegrityFailure,
            "oversized agent catalog was accepted as diagnostic evidence");
    }

    {
        Fixture fixture;
        auto configuration = fixture.configuration();
        configuration.resourceBudgets.historyPointsDefault = 0U;
        requireError(
            fixture.createConfigured(std::move(configuration)),
            Domain::ErrorCodes::InvalidRequest,
            "zero default telemetry history budget was accepted");
    }
    {
        Fixture fixture;
        auto configuration = fixture.configuration();
        configuration.resourceBudgets.historyPointsHardMaximum = 0U;
        requireError(
            fixture.createConfigured(std::move(configuration)),
            Domain::ErrorCodes::InvalidRequest,
            "zero hard telemetry history budget was accepted");
    }
    {
        Fixture fixture;
        auto configuration = fixture.configuration();
        configuration.resourceBudgets.historyPointsDefault = 11U;
        configuration.resourceBudgets.historyPointsHardMaximum = 10U;
        requireError(
            fixture.createConfigured(std::move(configuration)),
            Domain::ErrorCodes::InvalidRequest,
            "inverted telemetry history budgets were accepted");
    }
}

void nullTelemetrySnapshotIsAHardDiagnosticFailure()
{
    Fixture fixture;
    fixture.telemetry.sampleResult.set(
        Domain::Result<Contracts::ITelemetryService::Snapshot>::success(
            Contracts::ITelemetryService::Snapshot{}));

    const auto report = take(fixture.create()->run(context()));
    require(
        !check(report, "telemetry_snapshot").ok,
        "null telemetry snapshot passed its Doctor row");
    require(
        check(report, "telemetry_snapshot").hard,
        "null telemetry snapshot changed row hardness");
    require(!report.ok, "null telemetry snapshot did not fail the report");
}

void terminalTransportAndLmAuthorityErrorsPropagate()
{
    {
        Fixture fixture;
        fixture.probe.setError(Domain::makeError(
            Domain::ErrorCodes::TransportClosed,
            "The platform transport closed."));
        requireError(
            fixture.create()->run(context()),
            Domain::ErrorCodes::TransportClosed,
            "transport closure was converted to a Doctor row");
    }

    const std::vector<std::string_view> authorityErrorCodes{
        Domain::ErrorCodes::PathOutsideAuthority,
        Domain::ErrorCodes::ProjectScopeMismatch};
    for (const auto errorCode : authorityErrorCodes) {
        Fixture fixture;
        fixture.lmStudio.setError(Domain::makeError(
            errorCode,
            "The LM Studio read scope was rejected."));
        requireError(
            fixture.create()->run(context()),
            errorCode,
            "LM Studio authority-scope failure was disguised as availability");
    }
}

void relativeConfiguredPathsAreRejected()
{
    for (std::size_t pathIndex{}; pathIndex < 3U; ++pathIndex) {
        Fixture fixture;
        auto configuration = fixture.configuration();
        if (pathIndex == 0U) {
            configuration.dataRoot = path("relative\\Forge");
        } else if (pathIndex == 1U) {
            configuration.centralStorePath = path("store.sqlite");
        } else {
            configuration.installedBinaryPath = path("bin\\forge.exe");
        }
        requireError(
            fixture.createConfigured(std::move(configuration)),
            Domain::ErrorCodes::InvalidRequest,
            "relative Manager Doctor path was accepted");
    }
}

void cancellationDeadlineShutdownAndBorrowedLifetimes()
{
    {
        Fixture fixture;
        auto service = fixture.create();
        std::stop_source cancellation;
        cancellation.request_stop();
        requireError(
            service->run(context(
                Domain::MonotonicTimePoint{} + 20s,
                cancellation.get_token())),
            Domain::ErrorCodes::Cancelled,
            "Doctor ignored cancellation");
        require(fixture.probe.calls() == 0U, "cancelled run reached dependency");
        requireError(
            service->run(context(Domain::MonotonicTimePoint{} + 10s)),
            Domain::ErrorCodes::DeadlineExceeded,
            "Doctor ignored an expired deadline");
        require(fixture.probe.calls() == 0U, "expired run reached dependency");
    }
    {
        Fixture fixture;
        fixture.repository.quickCheckResult.set(
            Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The repository operation was cancelled.")));
        requireError(
            fixture.create()->run(context()),
            Domain::ErrorCodes::Cancelled,
            "dependency cancellation was converted to a Doctor row");
    }

    Fixture fixture;
    auto service = fixture.create();
    fixture.probe.blockNextInspect();
    const auto operationContext = context();
    auto runFuture = std::async(
        std::launch::async,
        [&]() { return service->run(operationContext); });
    fixture.probe.waitUntilInspectBlocked();
    auto shutdownFuture = std::async(
        std::launch::async,
        [&]() noexcept { service->shutdown(); });
    require(
        shutdownFuture.wait_for(50ms) == std::future_status::timeout,
        "Doctor shutdown did not drain admitted work");
    fixture.probe.releaseInspect();
    require(
        runFuture.get().hasValue(),
        "admitted Doctor run did not finish during shutdown");
    shutdownFuture.get();
    requireError(
        service->run(context()),
        Domain::ErrorCodes::Cancelled,
        "Doctor admitted work after shutdown");
    service.reset();

    require(
        fixture.probe.inspect(
            fixture.dataRoot,
            fixture.centralStorePath,
            fixture.installedBinaryPath,
            context()).hasValue(),
        "Doctor owned the platform probe lifetime");
    require(
        fixture.catalog.all(context()).hasValue(),
        "Doctor owned the agent catalog lifetime");
    require(
        fixture.repository.quickCheck(context()).hasValue(),
        "Doctor closed the borrowed session repository");
    require(
        fixture.telemetry.running(),
        "Doctor stopped the borrowed telemetry service");
    require(
        fixture.lmStudio.shutdownCalls() == 0U,
        "Doctor owned the LM Studio service shutdown");
    require(
        fixture.lmStudio.status(
            Domain::LMStudioDeploymentRequest{
                fixture.installedBinaryPath,
                true},
            fixture.authority,
            context()).hasValue(),
        "Doctor shut down the borrowed LM Studio service");
}

[[nodiscard]] std::string utf8(const std::filesystem::path& value)
{
    const auto& wide = value.native();
    const auto required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    require(required > 0, "filesystem path could not be encoded as UTF-8");
    std::string result(static_cast<std::size_t>(required), '\0');
    const auto written = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    require(written == required, "filesystem path UTF-8 encoding changed");
    return result;
}

[[nodiscard]] std::filesystem::path currentExecutable()
{
    std::wstring buffer(32U * 1024U, L'\0');
    const auto size = ::GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    require(
        size > 0U && size < buffer.size(),
        "current executable path was unavailable");
    buffer.resize(size);
    return std::filesystem::path{buffer};
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto base = std::filesystem::temp_directory_path();
        for (std::size_t attempt{}; attempt < 32U; ++attempt) {
            auto candidate = base /
                (L"forge-manager-doctor-tests-" +
                 std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(::GetTickCount64()) + L"-" +
                 std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                value_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error{"temporary directory could not be created"};
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code ignored;
        std::filesystem::remove_all(value_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& value() const noexcept
    {
        return value_;
    }

private:
    std::filesystem::path value_;
};

void windowsProbeUsesExactNativeEvidence()
{
    Clock clock;
    Composition::WindowsManagerDoctorPlatformProbe probe{clock};
    TemporaryDirectory temporary;
    const auto dataRoot = temporary.value() / L"Forge";
    const auto binaryDirectory = dataRoot / L"bin";
    const auto centralStore = dataRoot / L"store.sqlite";
    const auto installedBinary = binaryDirectory / L"forge.exe";
    std::filesystem::create_directories(binaryDirectory);
    {
        std::ofstream store{centralStore, std::ios::binary};
        store << "SQLite format 3";
    }
    std::filesystem::copy_file(
        currentExecutable(),
        installedBinary,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream launcher{binaryDirectory / L"forge-serve.cmd"};
        launcher << "legacy";
    }
    const auto reparseTarget = binaryDirectory / L"legacy-launcher-target";
    {
        std::ofstream launcher{reparseTarget};
        launcher << "legacy reparse target";
    }
    const auto reparseLauncher =
        binaryDirectory / L"forge-serve-fallback";
    const bool reparseLauncherCreated =
        ::CreateSymbolicLinkW(
            reparseLauncher.c_str(),
            reparseTarget.c_str(),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    if (reparseLauncherCreated) {
        const DWORD reparseAttributes =
            ::GetFileAttributesW(reparseLauncher.c_str());
        require(
            reparseAttributes != INVALID_FILE_ATTRIBUTES &&
                (reparseAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U &&
                (reparseAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U,
            "legacy launcher test fixture is not a file reparse point");
    }
    {
        std::ofstream nearMiss{binaryDirectory / L"forge-server.cmd"};
        nearMiss << "not a legacy launcher";
    }

    const auto snapshot = take(probe.inspect(
        path(utf8(dataRoot)),
        path(utf8(centralStore)),
        path(utf8(installedBinary)),
        context()));
    require(snapshot.homeDirectoryPresent, "Windows probe missed home directory");
    require(snapshot.centralStorePresent, "Windows probe missed central store");
    require(
        snapshot.installedBinaryExecutable,
        "Windows probe rejected a copied native executable");
    require(
        snapshot.legacyLauncherBasenames.size() ==
            (reparseLauncherCreated ? 2U : 1U),
        "Windows probe legacy launcher matching was not exact");
    require(
        std::find(
            snapshot.legacyLauncherBasenames.begin(),
            snapshot.legacyLauncherBasenames.end(),
            "forge-serve.cmd") != snapshot.legacyLauncherBasenames.end(),
        "Windows probe missed forge-serve.cmd");
    if (reparseLauncherCreated) {
        require(
            std::find(
                snapshot.legacyLauncherBasenames.begin(),
                snapshot.legacyLauncherBasenames.end(),
                "forge-serve-fallback") !=
                snapshot.legacyLauncherBasenames.end(),
            "Windows probe missed the forge-serve-fallback reparse point");
    }
    require(
        std::find(
            snapshot.legacyLauncherBasenames.begin(),
            snapshot.legacyLauncherBasenames.end(),
            "forge-server.cmd") == snapshot.legacyLauncherBasenames.end(),
        "Windows probe accepted a near-match legacy launcher");

    const auto missingRoot = temporary.value() / L"missing";
    const auto missing = take(probe.inspect(
        path(utf8(missingRoot)),
        path(utf8(missingRoot / L"store.sqlite")),
        path(utf8(missingRoot / L"forge.exe")),
        context()));
    require(!missing.homeDirectoryPresent, "Windows probe fabricated a home");
    require(!missing.centralStorePresent, "Windows probe fabricated a store");
    require(
        !missing.installedBinaryExecutable,
        "Windows probe fabricated an installed binary");
    require(
        missing.legacyLauncherBasenames.empty(),
        "Windows probe fabricated legacy launchers");
}

} // namespace

int main()
{
    try {
        exactContractOrderAndForwarding();
        hardFailuresFailTheReportAndSoftFailuresDoNot();
        catalogRequiresExactMandatoryIdentitiesAndUniqueness();
        acceptsRealtimeRuntimeAndProjectsUnavailableTelemetryTruthfully();
        dependencyErrorsAreProjectedSafelyAndAuthorityErrorsPropagate();
        rejectsOversizedCatalogAndInvalidTelemetryBudgets();
        nullTelemetrySnapshotIsAHardDiagnosticFailure();
        terminalTransportAndLmAuthorityErrorsPropagate();
        relativeConfiguredPathsAreRejected();
        cancellationDeadlineShutdownAndBorrowedLifetimes();
        windowsProbeUsesExactNativeEvidence();
        std::cout << "Manager Doctor service tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager Doctor service tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager Doctor service tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
