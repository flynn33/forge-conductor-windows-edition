#include "ForgeConductor/Manager/ManagerProtocolCodec.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Manager;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
}

template <typename Identifier>
[[nodiscard]] Identifier identifier(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] std::vector<std::byte> frameFromText(const std::string_view text)
{
    if (text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::runtime_error{"Test frame exceeds the protocol prefix."};
    }
    const auto length = static_cast<std::uint32_t>(text.size());
    std::vector<std::byte> frame(text.size() + 4U);
    frame[0] = static_cast<std::byte>(length & 0xffU);
    frame[1] = static_cast<std::byte>((length >> 8U) & 0xffU);
    frame[2] = static_cast<std::byte>((length >> 16U) & 0xffU);
    frame[3] = static_cast<std::byte>((length >> 24U) & 0xffU);
    if (!text.empty()) {
        std::memcpy(frame.data() + 4U, text.data(), text.size());
    }
    return frame;
}

[[nodiscard]] std::vector<std::byte> frameFromJson(const Json& value)
{
    return frameFromText(value.dump());
}

[[nodiscard]] std::string payloadText(const std::vector<std::byte>& frame)
{
    REQUIRE(frame.size() >= 4U);
    std::string payload(frame.size() - 4U, '\0');
    if (!payload.empty()) {
        std::memcpy(payload.data(), frame.data() + 4U, payload.size());
    }
    return payload;
}

void replaceOne(
    std::string& value,
    const std::string_view before,
    const std::string_view after)
{
    const auto offset = value.find(before);
    REQUIRE(offset != std::string::npos);
    value.replace(offset, before.size(), after);
}

[[nodiscard]] Domain::ManagerSettings sampleSettings()
{
    Domain::ManagerSettings settings;
    settings.dashboardHost = "::1";
    settings.dashboardPort = 65'535U;
    settings.dashboardRefreshInterval = 17s;
    settings.autoRestart = false;
    settings.watchdogInterval = 11s;
    settings.openBrowserOnStart = true;
    settings.sessionIdleTtl = 23'456s;
    settings.shellTimeout = 119s;
    settings.logLevel = Domain::LogLevel::Critical;
    settings.localModelHost = "::1";
    settings.localModelPort = 12'345U;
    settings.localModelSecure = true;
    settings.localModelName = "fixture-model";
    settings.effectiveContextCapacity = 65'536U;
    settings.nextResponseReserve = 8'192U;
    settings.handoffReserve = 6'144U;
    settings.estimationSafetyMargin = 3'072U;
    settings.shellEnabled = false;
    return settings;
}

[[nodiscard]] Domain::ManagerSettingsPatch samplePatch()
{
    Domain::ManagerSettingsPatch patch;
    patch.dashboardHost = "::1";
    patch.dashboardPort = static_cast<std::uint16_t>(44'444U);
    patch.dashboardRefreshInterval = 12s;
    patch.autoRestart = false;
    patch.watchdogInterval = 4s;
    patch.openBrowserOnStart = true;
    patch.sessionIdleTtl = 8'888s;
    patch.shellTimeout = 77s;
    patch.logLevel = Domain::LogLevel::Debug;
    patch.localModelHost = "::1";
    patch.localModelPort = static_cast<std::uint16_t>(12'345U);
    patch.localModelSecure = true;
    patch.localModelName = "fixture-model";
    patch.effectiveContextCapacity = 65'536U;
    patch.nextResponseReserve = 8'192U;
    patch.handoffReserve = 6'144U;
    patch.estimationSafetyMargin = 3'072U;
    patch.shellEnabled = false;
    return patch;
}

[[nodiscard]] Domain::ManagerStatus sampleStatus()
{
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        42'424U,
        std::optional<Domain::UtcTimePoint>{
            Domain::UtcTimePoint{std::chrono::milliseconds{1'767'225'600'123LL}}},
        std::optional<std::chrono::seconds>{987s},
        9U,
        std::optional<std::string>{"prior restart recovered"},
        false,
        11s,
        true,
        "::1",
        65'535U,
        17s,
        path("C:\\Users\\tester\\.forge-conductor"),
        "0.9.0-alpha"};
}

[[nodiscard]] Domain::ManagerSettingsUpdateOutcome sampleSettingsUpdateOutcome(
    const bool applied,
    const bool bindingChanged)
{
    return Domain::ManagerSettingsUpdateOutcome{
        sampleSettings(), applied, bindingChanged, sampleStatus()};
}

[[nodiscard]] Domain::ManagedRunSnapshot sampleManagedRun()
{
    return Domain::ManagedRunSnapshot{
        Domain::ManagedRunRecord{
            identifier<Domain::SessionId>(
                "20000000-0000-4000-8000-000000000001"),
            identifier<Domain::ProjectId>(
                "20000000-0000-4000-8000-000000000002"),
            identifier<Domain::ClientId>(
                "20000000-0000-4000-8000-000000000003"),
            "Inspect the active project and report the result.",
            9U,
            Domain::ManagedRunState::Completed,
            identifier<Domain::ProviderSessionId>("response-fixture-1"),
            101U,
            37U,
            4096U,
            std::string{"The managed result."},
            std::nullopt,
            {},
            Domain::UtcTimePoint{std::chrono::milliseconds{1'767'225'600'123LL}},
            Domain::UtcTimePoint{std::chrono::milliseconds{1'767'225'601'456LL}},
            false},
        true,
        false};
}

[[nodiscard]] Domain::ManagerTelemetrySnapshot sampleManagerTelemetry()
{
    const auto capturedAt = Domain::UtcTimePoint{
        std::chrono::milliseconds{1'767'225'602'000LL}};
    const auto previousCpu = Domain::makeAvailableTelemetryMetric<double>(
        42.5, capturedAt - 1s, "GetSystemTimes");
    auto cpu = Domain::makeStaleTelemetryMetric(
        previousCpu,
        Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
        capturedAt,
        "The CPU counter rebased.");
    const auto ramPercent = Domain::makeAvailableTelemetryMetric<double>(
        61.25, capturedAt, "GlobalMemoryStatusEx");
    const auto ramUsed = Domain::makeAvailableTelemetryMetric<std::uint64_t>(
        10'000U, capturedAt, "GlobalMemoryStatusEx");
    const auto ramTotal = Domain::makeAvailableTelemetryMetric<std::uint64_t>(
        20'000U, capturedAt, "GlobalMemoryStatusEx");
    const auto ramAvailable = Domain::makeAvailableTelemetryMetric<std::uint64_t>(
        10'000U, capturedAt, "GlobalMemoryStatusEx");
    Domain::ManagerResourceSnapshot resources{
        capturedAt,
        "fixture-host",
        "Windows 11",
        "x64",
        std::move(cpu),
        ramPercent,
        ramUsed,
        ramTotal,
        ramAvailable,
        {Domain::GpuMetrics{
            "fixture-vendor", "fixture-gpu", std::nullopt,
            2'000U, 8'000U, 1'000U, true}},
        {Domain::ProcessMetrics{
            42'424U, "ForgeConductor.Manager", 3.5, 5'000U, 4'000U,
            12U, 88U, "GetProcessTimes"}},
        {Domain::HistoryPoint{
            capturedAt, 42.5, 61.25, std::nullopt, 0.0, 3U,
            Domain::TelemetryHealth::Ok}}};
    resources.cpuPerLogicalProcessor =
        Domain::makeAvailableTelemetryMetric<std::vector<double>>(
            {12.5, 72.25}, capturedAt, "PDH logical CPU");
    resources.cpuFrequencyMhz =
        Domain::makeAvailableTelemetryMetric<std::uint32_t>(
            3401U, capturedAt, "PDH CPU frequency");
    resources.cpuPerLogicalFrequencyMhz =
        Domain::makeAvailableTelemetryMetric<std::vector<std::uint32_t>>(
            {3401U, 3401U}, capturedAt, "PDH CPU frequency");
    resources.diskIo = Domain::makeAvailableTelemetryMetric(
        Domain::DiskIoMetrics{1024.0, 2048.0, 3.0, 4.0},
        capturedAt, "PDH PhysicalDisk");
    resources.disks.push_back(Domain::DiskVolume{
        "C:\\", path("C:\\"), "NTFS", 10'000U, 4'000U, 6'000U,
        40.0, capturedAt, "GetDiskFreeSpaceEx"});
    resources.targetSampleIntervalMilliseconds = 250U;
    resources.measuredSampleIntervalMilliseconds = 251.5;
    resources.samplingPolicy = "realtime_cpu_ram; gpu_disk_1s; process_volume_5s";
    resources.gpus.front().adapterId = "0:1";
    resources.gpus.front().engines.push_back(
        Domain::GpuEngineMetrics{"3D 0", 37.5});
    resources.gpus.front().capturedAt = capturedAt;
    resources.gpus.front().utilizationSource = "PDH GPU Engine";
    resources.gpus.front().memoryScope = "current-process usage; adapter capacity";
    resources.processes.front().capturedAt = capturedAt;
    const auto run = sampleManagedRun();
    return Domain::ManagerTelemetrySnapshot{
        capturedAt,
        std::move(resources),
        sampleStatus(),
        Domain::RuntimeDiagnosticSnapshot{
            capturedAt, 2U, 1U, 4U, 3U, 0U,
            Domain::ResourcePressureLevel::Nominal, 2U, 1U, 0U, 1U},
        Domain::ManagerProviderSnapshot{
            "127.0.0.1", 1234U, false, std::string{"fixture-model"},
            run.record.providerResponseId},
        Domain::ManagerContextSnapshot{
            65'536U, 8'192U, 6'144U, 3'072U, 101U, 37U, 4'096U,
            44'032U, true},
        Domain::ManagerContinuitySnapshot{
            true, true, run.record.runId, run.record.projectId,
            run.record.state, run.record.providerResponseId},
        run,
        {run.record.projectId},
        {"filesystem.read", "shell.execute"},
        1U,
        2U,
        1U,
        {Domain::AuditEvent{
            capturedAt,
            run.record.clientId,
            "shell.execute",
            identifier<Domain::Sha256Digest>(std::string(64U, 'b')),
            "success",
            25ms,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            run.record.projectId}},
        Domain::makeUnavailableTelemetryMetric<bool>(
            Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
            capturedAt,
            "manager_operational_store",
            "The store is busy."),
        "windows-native"};
}

[[nodiscard]] Manager::ManagerRequest request(
    Manager::ManagerRequestPayload payload)
{
    return Manager::ManagerRequest{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        1'767'225'630'000LL,
        identifier<Domain::Sha256Digest>(std::string(64U, 'a')),
        std::move(payload)};
}

[[nodiscard]] Manager::ManagerResponse response(Manager::ManagerResult result)
{
    return Manager::ManagerResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{std::move(result)}};
}

[[nodiscard]] Json requestJson(const Manager::ManagerRequestPayload& payload)
{
    return Json::parse(payloadText(take(
        Manager::ManagerProtocolCodec::encodeRequest(request(payload)))));
}

[[nodiscard]] Json responseJson(const Manager::ManagerResult& result)
{
    return Json::parse(payloadText(take(
        Manager::ManagerProtocolCodec::encodeResponse(response(result)))));
}

void testTypeAndPrefixContract()
{
    static_assert(std::is_final_v<Manager::ManagerProtocolCodec>);
    static_assert(Manager::ManagerProtocolVersion == 1U);
    static_assert(
        Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes == 2'097'152U);
    static_assert(Manager::ManagerProtocolCodec::MaximumJsonNesting == 64U);

    const auto encoded = take(Manager::ManagerProtocolCodec::encodeRequest(
        request(Manager::ManagerStatusRequest{})));
    const auto payloadBytes = encoded.size() - 4U;
    const auto prefix =
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[0])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[3])) << 24U);
    REQUIRE(prefix == payloadBytes);

    const auto payload = payloadText(encoded);
    REQUIRE(!payload.empty() && payload.front() == '{' && payload.back() == '}');
    REQUIRE(payload.find('\n') == std::string::npos);
    REQUIRE(payload.starts_with("{\"correlation_id\":"));
}

void testEveryRequestMethodRoundTripsDeterministically()
{
    std::vector<Manager::ManagerRequestPayload> payloads;
    payloads.emplace_back(Manager::ManagerStatusRequest{});
    payloads.emplace_back(Manager::ManagerSettingsRequest{});
    payloads.emplace_back(Manager::ManagerTelemetryRequest{
        identifier<Domain::SessionId>(
            "20000000-0000-4000-8000-000000000001")});
    payloads.emplace_back(Manager::ManagerProjectsListRequest{32U});
    payloads.emplace_back(Manager::ManagerProjectInitializeRequest{
        path("D:\\Projects\\Alpha"), std::string{"Alpha"}, std::nullopt});
    payloads.emplace_back(Manager::ManagerProjectMemoryRequest{
        identifier<Domain::ProjectId>(
            "20000000-0000-4000-8000-000000000002"),
        "decision", 20U});
    payloads.emplace_back(Manager::ManagerProjectRememberRequest{
        identifier<Domain::ProjectId>(
            "20000000-0000-4000-8000-000000000002"),
        "Decision", "Keep project identity stable.",
        std::string{"Runs bind to the selected exact project ID."},
        {"architecture", "identity"}});
    payloads.emplace_back(Manager::ManagerInstructionPackageRequest{
        identifier<Domain::ProjectId>(
            "20000000-0000-4000-8000-000000000002"),
        path("D:\\Packages\\Alpha"), true,
        identifier<Domain::Sha256Digest>(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")});
    payloads.emplace_back(Manager::ManagerInstructionPackageQueueRequest{
        identifier<Domain::ProjectId>(
            "20000000-0000-4000-8000-000000000002"),
        Manager::ManagerInstructionPackageQueueAction::ReadContent,
        std::string{"queue-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        std::nullopt,
        std::string{"page-cursor"},
        25U,
        std::string{"assets/binary.dat"},
        65'536U,
        32U * 1024U});
    payloads.emplace_back(ForgeConductor::Contracts::ProjectPolicyRequest{
        identifier<Domain::ProjectId>("20000000-0000-4000-8000-000000000002"),
        ForgeConductor::Contracts::ProjectPolicyAction::Bind,
        "https://github.com/flynn33/raven-forge-development", {}, {}});
    payloads.emplace_back(Manager::ManagerMaintenanceRequest{
        Manager::ManagerMaintenanceScope::ProjectAllData,
        identifier<Domain::ProjectId>(
            "20000000-0000-4000-8000-000000000002"),
        "RESET PROJECT DATA 20000000-0000-4000-8000-000000000002"});
    payloads.emplace_back(Domain::ManagerControlRequest{
        Domain::ManagerControlAction::Repair});
    payloads.emplace_back(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), true});
    payloads.emplace_back(Manager::ManagedRunStartRequest{
        identifier<Domain::SessionId>(
            "20000000-0000-4000-8000-000000000001"),
        identifier<Domain::ProjectId>(
            "20000000-0000-4000-8000-000000000002"),
        identifier<Domain::ClientId>(
            "20000000-0000-4000-8000-000000000003"),
        9U,
        "Run the ordinary managed turn."});
    payloads.emplace_back(Manager::ManagedRunStatusRequest{
        identifier<Domain::SessionId>(
            "20000000-0000-4000-8000-000000000001")});
    payloads.emplace_back(Manager::ManagedRunCancelRequest{
        identifier<Domain::SessionId>(
            "20000000-0000-4000-8000-000000000001")});
    payloads.emplace_back(Manager::ManagedRunPauseRequest{
        identifier<Domain::SessionId>(
            "20000000-0000-4000-8000-000000000001")});
    payloads.emplace_back(Manager::ManagedRunResumeRequest{
        identifier<Domain::SessionId>(
            "20000000-0000-4000-8000-000000000001")});
    payloads.emplace_back(Manager::ManagerCancelRequest{
        identifier<Domain::OperationId>(
            "10000000-0000-4000-8000-000000000016")});
    payloads.emplace_back(Manager::ManagerShutdownRequest{});

    const std::vector<std::string> methods{
        "manager.status",
        "manager.settings",
        "manager.telemetry",
        "projects.list",
        "projects.initialize",
        "projects.memory",
        "projects.remember",
        "projects.instructions",
        "projects.instruction_queue",
        "projects.policy",
        "maintenance.reset",
        "manager.control",
        "manager.settings.update",
        "managed_run.start",
        "managed_run.status",
        "managed_run.cancel",
        "managed_run.pause",
        "managed_run.resume",
        "manager.cancel",
        "manager.shutdown"};

    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const auto original = request(payloads[index]);
        const auto first = take(
            Manager::ManagerProtocolCodec::encodeRequest(original));
        const auto second = take(
            Manager::ManagerProtocolCodec::encodeRequest(original));
        REQUIRE(first == second);

        const auto decoded = take(
            Manager::ManagerProtocolCodec::decodeRequest(first));
        REQUIRE(decoded.version == Manager::ManagerProtocolVersion);
        REQUIRE(decoded.requestId.value() == original.requestId.value());
        REQUIRE(decoded.correlationId.value() == original.correlationId.value());
        REQUIRE(decoded.deadlineUtcMilliseconds == original.deadlineUtcMilliseconds);
        REQUIRE(decoded.nonce.value() == original.nonce.value());
        REQUIRE(decoded.payload.index() == original.payload.index());
        REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(decoded)) == first);

        const auto json = Json::parse(payloadText(first));
        REQUIRE(json.size() == 7U);
        REQUIRE(json.at("method").get<std::string>() == methods[index]);
    }

    const auto update = take(Manager::ManagerProtocolCodec::decodeRequest(
        take(Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagerSettingsUpdateRequest{samplePatch(), true})))));
    const auto& updatePayload =
        std::get<Manager::ManagerSettingsUpdateRequest>(update.payload);
    REQUIRE(updatePayload.applyImmediately);
    REQUIRE(updatePayload.patch.dashboardHost == "::1");
    REQUIRE(updatePayload.patch.dashboardPort == 44'444U);
    REQUIRE(updatePayload.patch.dashboardRefreshInterval == 12s);
    REQUIRE(updatePayload.patch.autoRestart == false);
    REQUIRE(updatePayload.patch.watchdogInterval == 4s);
    REQUIRE(updatePayload.patch.openBrowserOnStart == true);
    REQUIRE(updatePayload.patch.sessionIdleTtl == 8'888s);
    REQUIRE(updatePayload.patch.shellTimeout == 77s);
    REQUIRE(updatePayload.patch.logLevel == Domain::LogLevel::Debug);
    REQUIRE(updatePayload.patch.localModelHost == "::1");
    REQUIRE(updatePayload.patch.localModelPort == 12'345U);
    REQUIRE(updatePayload.patch.localModelSecure == true);
    REQUIRE(updatePayload.patch.localModelName == "fixture-model");
    REQUIRE(updatePayload.patch.effectiveContextCapacity == 65'536U);
    REQUIRE(updatePayload.patch.shellEnabled == false);

    const auto managedStart = take(Manager::ManagerProtocolCodec::decodeRequest(
        take(Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagedRunStartRequest{
                identifier<Domain::SessionId>(
                    "20000000-0000-4000-8000-000000000001"),
                identifier<Domain::ProjectId>(
                    "20000000-0000-4000-8000-000000000002"),
                identifier<Domain::ClientId>(
                    "20000000-0000-4000-8000-000000000003"),
                9U,
                "Run the ordinary managed turn.",
                false,
                false})))));
    const auto& managedPayload =
        std::get<Manager::ManagedRunStartRequest>(managedStart.payload);
    REQUIRE(managedPayload.authorityGeneration == 9U);
    REQUIRE(managedPayload.task == "Run the ordinary managed turn.");
    REQUIRE(!managedPayload.allowTools);
    REQUIRE(!managedPayload.automaticContinuity);
}

void testManagedRunResultRoundTrips()
{
    const auto frame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{sampleManagedRun()})));
    const auto root = Json::parse(payloadText(frame));
    REQUIRE(root.at("result").at("type") == "managed_run");
    REQUIRE(root.at("result").at("value").size() == 19U);
    REQUIRE(root.at("result").at("value").at("allow_tools") == false);
    const auto decoded = take(
        Manager::ManagerProtocolCodec::decodeResponse(frame));
    const auto& actual = std::get<Domain::ManagedRunSnapshot>(
        std::get<Manager::ManagerResult>(decoded.body));
    REQUIRE(actual.record.runId == sampleManagedRun().record.runId);
    REQUIRE(actual.record.projectId == sampleManagedRun().record.projectId);
    REQUIRE(actual.record.state == Domain::ManagedRunState::Completed);
    REQUIRE(actual.record.providerResponseId ==
            sampleManagedRun().record.providerResponseId);
    REQUIRE(actual.record.retainedContextTokens == 4096U);
    REQUIRE(actual.record.outputText == "The managed result.");
    REQUIRE(!actual.record.allowTools);
    REQUIRE(actual.managerOwned);
    REQUIRE(!actual.cancellationRequested);
    REQUIRE(!actual.pauseRequested);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decoded)) == frame);
}

void testManagerTelemetryRoundTripsWithoutLosingAvailability()
{
    const auto frame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{sampleManagerTelemetry()})));
    const auto root = Json::parse(payloadText(frame));
    REQUIRE(root.at("result").at("type") == "telemetry");
    REQUIRE(root.at("result").at("value").at("resources")
                .at("cpu_percent").at("stale") == true);
    REQUIRE(root.at("result").at("value").at("resources")
                .at("gpus").at(0).at("utilization_percent").is_null());
    REQUIRE(root.at("result").at("value").at("store_healthy")
                .at("availability") == "temporarily_unavailable");

    const auto decoded = take(
        Manager::ManagerProtocolCodec::decodeResponse(frame));
    const auto& actual = std::get<Domain::ManagerTelemetrySnapshot>(
        std::get<Manager::ManagerResult>(decoded.body));
    REQUIRE(actual.resources.cpuPercent.value == 42.5);
    REQUIRE(actual.resources.cpuPercent.stale);
    REQUIRE(actual.resources.cpuPercent.capturedAt ==
            sampleManagerTelemetry().resources.cpuPercent.capturedAt);
    REQUIRE(actual.resources.gpus.size() == 1U);
    REQUIRE(!actual.resources.gpus.front().utilizationPercent);
    REQUIRE(actual.resources.gpus.front().adapterId == "0:1");
    REQUIRE(actual.resources.gpus.front().engines.size() == 1U);
    REQUIRE(actual.resources.cpuPerLogicalProcessor.value &&
            actual.resources.cpuPerLogicalProcessor.value->at(1) == 72.25);
    REQUIRE(actual.resources.cpuFrequencyMhz.value == 3401U);
    REQUIRE(actual.resources.diskIo.value &&
            actual.resources.diskIo.value->writeBytesPerSecond == 2048.0);
    REQUIRE(actual.resources.disks.size() == 1U &&
            actual.resources.disks.front().mount.value() == "C:\\");
    REQUIRE(actual.resources.targetSampleIntervalMilliseconds == 250U);
    REQUIRE(actual.resources.measuredSampleIntervalMilliseconds == 251.5);
    REQUIRE(actual.context.retainedTokens == 4'096U);
    REQUIRE(actual.context.headroomTokens == 44'032U);
    REQUIRE(actual.context.authoritative);
    REQUIRE(actual.continuity.canonicalResponseId ==
            sampleManagedRun().record.providerResponseId);
    REQUIRE(actual.selectedRun->record.retainedContextTokens == 4'096U);
    REQUIRE(actual.projects.size() == 1U);
    REQUIRE(actual.tools.size() == 2U);
    REQUIRE(actual.recentEvents.size() == 1U);
    REQUIRE(actual.recentEvents[0].projectId == sampleManagedRun().record.projectId);
    REQUIRE(!actual.storeHealthy.value);
    REQUIRE(actual.storeHealthy.availability ==
            Domain::TelemetryMetricAvailability::TemporarilyUnavailable);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decoded)) == frame);
}

void testToolOutcomePreservesMeasuredDuration()
{
    const Manager::ManagerToolOutcomeSnapshot snapshot{
        identifier<Domain::ProjectId>(
            "30000000-0000-4000-8000-000000000001"),
        "agent_list", true, "{\"agents\":[]}", std::nullopt,
        std::chrono::milliseconds{47}};
    const auto frame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{snapshot})));
    const auto root = Json::parse(payloadText(frame));
    REQUIRE(root.at("result").at("type") == "tool_outcome");
    REQUIRE(root.at("result").at("value").at("elapsed_ms") == 47);
    const auto decoded = take(Manager::ManagerProtocolCodec::decodeResponse(frame));
    const auto& actual = std::get<Manager::ManagerToolOutcomeSnapshot>(
        std::get<Manager::ManagerResult>(decoded.body));
    REQUIRE(actual.elapsed == std::chrono::milliseconds{47});
    REQUIRE(actual.canonicalPayload == snapshot.canonicalPayload);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decoded)) == frame);
}

void testMaintenanceRoundTrips()
{
    const Manager::ManagerMaintenanceSnapshot snapshot{
        Manager::ManagerMaintenanceScope::ProjectAllData,
        "20000000-0000-4000-8000-000000000002",
        1U, 3U, 4U, 5U, true,
        "Project memory and continuity reset completed."};
    const auto frame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{snapshot})));
    const auto decoded = take(
        Manager::ManagerProtocolCodec::decodeResponse(frame));
    const auto& actual = std::get<Manager::ManagerMaintenanceSnapshot>(
        std::get<Manager::ManagerResult>(decoded.body));
    REQUIRE(actual.scope == Manager::ManagerMaintenanceScope::ProjectAllData);
    REQUIRE(actual.affectedScope == snapshot.affectedScope);
    REQUIRE(actual.projectsAffected == 1U);
    REQUIRE(actual.recordsRemoved == 3U);
    REQUIRE(actual.linksRemoved == 4U);
    REQUIRE(actual.eventsRemoved == 5U);
    REQUIRE(actual.verified);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decoded)) == frame);
}

void testProjectRunHistoryRoundTrips()
{
    const auto project = identifier<Domain::ProjectId>(
        "20000000-0000-4000-8000-000000000002");
    const auto framed = take(Manager::ManagerProtocolCodec::encodeRequest(request(
        Manager::ManagerOperationalRequest{
            Manager::ManagerOperationalArea::Runs,
            Manager::ManagerOperationalAction::Inspect,
            std::nullopt, {}, project})));
    const auto decoded = take(Manager::ManagerProtocolCodec::decodeRequest(framed));
    const auto& operation = std::get<Manager::ManagerOperationalRequest>(
        decoded.payload);
    REQUIRE(operation.area == Manager::ManagerOperationalArea::Runs);
    REQUIRE(operation.projectId == project);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(decoded)) == framed);
    const auto root = Json::parse(payloadText(framed));
    REQUIRE(root.at("params").at("project_id").get<std::string>() ==
        project.value());

    const Manager::ManagerOperationalSnapshot history{
        Manager::ManagerOperationalArea::Runs,
        "Recent Manager-owned runs · selected project",
        {"20000000-0000-4000-8000-000000000001 · completed\nfixture task"}};
    const auto responseFrame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{history})));
    const auto responseDecoded = take(
        Manager::ManagerProtocolCodec::decodeResponse(responseFrame));
    const auto& result = std::get<Manager::ManagerOperationalSnapshot>(
        std::get<Manager::ManagerResult>(responseDecoded.body));
    REQUIRE(result.area == Manager::ManagerOperationalArea::Runs);
    REQUIRE(result.lines == history.lines);

    const auto evidenceFrame = take(
        Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagerOperationalRequest{
                Manager::ManagerOperationalArea::Evidence,
                Manager::ManagerOperationalAction::Inspect,
                std::nullopt, {}, project})));
    const auto evidenceDecoded = take(
        Manager::ManagerProtocolCodec::decodeRequest(evidenceFrame));
    const auto& evidenceRequest = std::get<Manager::ManagerOperationalRequest>(
        evidenceDecoded.payload);
    REQUIRE(evidenceRequest.area == Manager::ManagerOperationalArea::Evidence);
    REQUIRE(evidenceRequest.projectId == project);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(
        evidenceDecoded)) == evidenceFrame);
    const Manager::ManagerOperationalSnapshot evidence{
        Manager::ManagerOperationalArea::Evidence,
        "Durable Manager-owned runs · exact selected project",
        {"{\"format\":\"forge-conductor-managed-run-evidence-v1\"}"}};
    const auto evidenceResponse = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{evidence})));
    const auto parsedEvidence = take(
        Manager::ManagerProtocolCodec::decodeResponse(evidenceResponse));
    const auto& evidenceResult = std::get<Manager::ManagerOperationalSnapshot>(
        std::get<Manager::ManagerResult>(parsedEvidence.body));
    REQUIRE(evidenceResult.area == Manager::ManagerOperationalArea::Evidence);
    REQUIRE(evidenceResult.lines == evidence.lines);
    const auto exactRun = identifier<Domain::SessionId>(
        "20000000-0000-4000-8000-000000000003");
    const auto verifyFrame = take(
        Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagerOperationalRequest{
                Manager::ManagerOperationalArea::Evidence,
                Manager::ManagerOperationalAction::VerifyTask,
                exactRun, "Write-Output CHECK_OK", project})));
    const auto verifyDecoded = take(
        Manager::ManagerProtocolCodec::decodeRequest(verifyFrame));
    const auto& verify = std::get<Manager::ManagerOperationalRequest>(
        verifyDecoded.payload);
    REQUIRE(verify.area == Manager::ManagerOperationalArea::Evidence);
    REQUIRE(verify.action == Manager::ManagerOperationalAction::VerifyTask);
    REQUIRE(verify.projectId == project);
    REQUIRE(verify.sessionId == exactRun);
    REQUIRE(verify.summary == "Write-Output CHECK_OK");
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(
        verifyDecoded)) == verifyFrame);
}

void testProjectWorkflowRoundTrips()
{
    const Domain::ProjectMemoryDescriptor descriptor{
        identifier<Domain::ProjectId>(
            "30000000-0000-4000-8000-000000000001"),
        "Alpha project",
        std::string{"https://github.com/example/alpha"},
        {path("D:\\Projects\\Alpha"), path("D:\\Worktrees\\Alpha")}};
    const auto projectsFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{
                Manager::ManagerProjectsSnapshot{{descriptor}}})));
    const auto decodedProjects = take(
        Manager::ManagerProtocolCodec::decodeResponse(projectsFrame));
    const auto& projects = std::get<Manager::ManagerProjectsSnapshot>(
        std::get<Manager::ManagerResult>(decodedProjects.body));
    REQUIRE(projects.projects.size() == 1U);
    REQUIRE(projects.projects.front().id == descriptor.id);
    REQUIRE(projects.projects.front().aliases.size() == 2U);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedProjects)) ==
            projectsFrame);

    const auto recordId = identifier<Domain::MemoryRecordId>(
        "30000000-0000-4000-8000-000000000002");
    const Manager::ManagerProjectWorkspaceSnapshot workspace{
        descriptor,
        7U,
        1U,
        11U,
        4'096U,
        512U,
        true,
        true,
        {Manager::ManagerProjectMemoryRecord{
            recordId,
            3U,
            "decision",
            "Keep exact identity",
            "Runs remain bound to the selected project.",
            std::string{"The authorized folder is not used as a substitute ID."},
            {"identity", "runs"},
            Domain::UtcTimePoint{
                std::chrono::milliseconds{1'767'225'602'321LL}}}},
        std::nullopt,
        std::string{"next-page"},
        true,
        recordId};
    const auto workspaceFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{workspace})));
    const auto decodedWorkspace = take(
        Manager::ManagerProtocolCodec::decodeResponse(workspaceFrame));
    const auto& actual = std::get<Manager::ManagerProjectWorkspaceSnapshot>(
        std::get<Manager::ManagerResult>(decodedWorkspace.body));
    REQUIRE(actual.project.id == descriptor.id);
    REQUIRE(actual.recordCount == 7U);
    REQUIRE(actual.integrityOk);
    REQUIRE(actual.records.size() == 1U);
    REQUIRE(actual.records.front().id == recordId);
    REQUIRE(actual.records.front().body ==
            "The authorized folder is not used as a substitute ID.");
    REQUIRE(actual.writtenRecordId == recordId);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedWorkspace)) ==
            workspaceFrame);

    const auto revision = identifier<Domain::Sha256Digest>(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const Manager::ManagerInstructionPackageSnapshot package{
        descriptor.id,
        "Alpha instructions",
        path("D:\\Packages\\Alpha"),
        revision,
        2U,
        1U,
        4'096U,
        {"START-HERE.md", "specs/policy.json"},
        true,
        recordId,
        std::string{"queue-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
        1024U,
        1U,
        std::string{"2"},
        true};
    const auto packageFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{package})));
    const auto decodedPackage = take(
        Manager::ManagerProtocolCodec::decodeResponse(packageFrame));
    const auto& actualPackage =
        std::get<Manager::ManagerInstructionPackageSnapshot>(
            std::get<Manager::ManagerResult>(decodedPackage.body));
    REQUIRE(actualPackage.projectId == descriptor.id);
    REQUIRE(actualPackage.revision == revision);
    REQUIRE(actualPackage.files == package.files);
    REQUIRE(actualPackage.activated);
    REQUIRE(actualPackage.manifestRecordId == recordId);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedPackage)) ==
            packageFrame);

    const Manager::ManagerInstructionPackageQueueSnapshot queue{
        descriptor.id,
        {Manager::ManagerInstructionPackageQueueRowSnapshot{
            "queue-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "Alpha instructions",
            path("D:\\Packages\\Alpha"),
            revision,
            1024U,
            "ready",
            44U,
            8U * 1024U * 1024U,
            2U,
            3U,
            2048U,
            std::nullopt}},
        {Manager::ManagerInstructionPackageEntrySnapshot{
            "assets/binary.dat", "file", 512U * 1024U, revision,
            "opaque", std::string{"No text representation"}}},
        std::string{"next-entry-page"},
        true,
        std::string{"AAEC/w=="},
        65'536U,
        65'540U,
        false};
    const auto queueFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{queue})));
    const auto decodedQueue = take(
        Manager::ManagerProtocolCodec::decodeResponse(queueFrame));
    const auto& actualQueue =
        std::get<Manager::ManagerInstructionPackageQueueSnapshot>(
            std::get<Manager::ManagerResult>(decodedQueue.body));
    REQUIRE(actualQueue.projectId == descriptor.id);
    REQUIRE(actualQueue.rows.size() == 1U);
    REQUIRE(actualQueue.entries.size() == 1U);
    REQUIRE(actualQueue.contentBase64 == "AAEC/w==");
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedQueue)) ==
            queueFrame);
}

void testResponseResultAndErrorRoundTrips()
{
    const auto statusFrame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{sampleStatus()})));
    const auto decodedStatus = take(
        Manager::ManagerProtocolCodec::decodeResponse(statusFrame));
    REQUIRE(std::holds_alternative<Manager::ManagerResult>(decodedStatus.body));
    const auto& statusResult =
        std::get<Manager::ManagerResult>(decodedStatus.body);
    REQUIRE(std::holds_alternative<Domain::ManagerStatus>(statusResult));
    const auto& status = std::get<Domain::ManagerStatus>(statusResult);
    REQUIRE(status.ok && status.isManager);
    REQUIRE(status.state == Domain::ManagerServiceState::Running);
    REQUIRE(status.processId == 42'424U);
    REQUIRE(status.startedAt == sampleStatus().startedAt);
    REQUIRE(status.uptime == 987s);
    REQUIRE(status.lastError == "prior restart recovered");
    REQUIRE(status.dashboardHost == "::1");
    REQUIRE(status.dashboardPort == 65'535U);
    REQUIRE(status.home.value() == "C:\\Users\\tester\\.forge-conductor");
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedStatus)) ==
            statusFrame);

    const auto statusRoot = Json::parse(payloadText(statusFrame));
    const auto& statusValue = statusRoot.at("result").at("value");
    REQUIRE(statusValue.size() == 19U);
    REQUIRE(statusValue.contains("started_at_utc_ms"));
    REQUIRE(statusValue.contains("dashboard_refresh_interval_seconds"));
    REQUIRE(statusValue.at("state") == "running");

    const auto settingsFrame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{sampleSettings()})));
    const auto decodedSettings = take(
        Manager::ManagerProtocolCodec::decodeResponse(settingsFrame));
    const auto& settings = std::get<Domain::ManagerSettings>(
        std::get<Manager::ManagerResult>(decodedSettings.body));
    REQUIRE(settings.dashboardHost == "::1");
    REQUIRE(settings.dashboardPort == 65'535U);
    REQUIRE(settings.dashboardRefreshInterval == 17s);
    REQUIRE(!settings.autoRestart);
    REQUIRE(settings.watchdogInterval == 11s);
    REQUIRE(settings.openBrowserOnStart);
    REQUIRE(settings.sessionIdleTtl == 23'456s);
    REQUIRE(settings.shellTimeout == 119s);
    REQUIRE(settings.logLevel == Domain::LogLevel::Critical);
    REQUIRE(settings.localModelHost == "::1");
    REQUIRE(settings.localModelPort == 12'345U);
    REQUIRE(settings.localModelSecure);
    REQUIRE(settings.localModelName == "fixture-model");
    REQUIRE(settings.effectiveContextCapacity == 65'536U);
    REQUIRE(settings.nextResponseReserve == 8'192U);
    REQUIRE(settings.handoffReserve == 6'144U);
    REQUIRE(settings.estimationSafetyMargin == 3'072U);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedSettings)) ==
            settingsFrame);

    const auto acknowledgementFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{Manager::ManagerAcknowledgement{false}})));
    const auto acknowledgement = take(
        Manager::ManagerProtocolCodec::decodeResponse(acknowledgementFrame));
    REQUIRE(!std::get<Manager::ManagerAcknowledgement>(
                 std::get<Manager::ManagerResult>(acknowledgement.body))
                 .acknowledged);

    Manager::ManagerResponse errorResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "manager transition conflict",
            true,
            "manager-evidence-17")}};
    const auto errorFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(errorResponse));
    const auto decodedError = take(
        Manager::ManagerProtocolCodec::decodeResponse(errorFrame));
    REQUIRE(std::holds_alternative<Domain::Error>(decodedError.body));
    REQUIRE(std::get<Domain::Error>(decodedError.body) ==
            std::get<Domain::Error>(errorResponse.body));
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedError)) ==
            errorFrame);
}

void testSettingsUpdateOutcomeRoundTrips()
{
    for (const bool applied : {false, true}) {
        for (const bool bindingChanged : {false, true}) {
            const auto original = sampleSettingsUpdateOutcome(
                applied, bindingChanged);
            const auto frame = take(
                Manager::ManagerProtocolCodec::encodeResponse(response(
                    Manager::ManagerResult{original})));
            const auto root = Json::parse(payloadText(frame));
            REQUIRE(root.at("result").at("type") == "settings_update");
            const auto& encoded = root.at("result").at("value");
            REQUIRE(encoded.size() == 4U);
            REQUIRE(encoded.at("applied") == applied);
            REQUIRE(encoded.at("binding_changed") == bindingChanged);
            REQUIRE(encoded.at("settings").is_object());
            REQUIRE(encoded.at("status").is_object());

            const auto decoded = take(
                Manager::ManagerProtocolCodec::decodeResponse(frame));
            const auto& result = std::get<Manager::ManagerResult>(decoded.body);
            REQUIRE(std::holds_alternative<
                    Domain::ManagerSettingsUpdateOutcome>(result));
            const auto& outcome =
                std::get<Domain::ManagerSettingsUpdateOutcome>(result);
            REQUIRE(outcome.applied == applied);
            REQUIRE(outcome.bindingChanged == bindingChanged);
            REQUIRE(outcome.settings.dashboardHost == "::1");
            REQUIRE(outcome.settings.dashboardPort == 65'535U);
            REQUIRE(outcome.settings.logLevel == Domain::LogLevel::Critical);
            REQUIRE(outcome.status.state == Domain::ManagerServiceState::Running);
            REQUIRE(outcome.status.processId == 42'424U);
            REQUIRE(take(
                Manager::ManagerProtocolCodec::encodeResponse(decoded)) == frame);
        }
    }
}

void testNullOptionalFieldsAreLossless()
{
    auto status = sampleStatus();
    status.startedAt.reset();
    status.uptime.reset();
    status.lastError.reset();
    const auto frame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{status})));
    const auto root = Json::parse(payloadText(frame));
    const auto& value = root.at("result").at("value");
    REQUIRE(value.at("started_at_utc_ms").is_null());
    REQUIRE(value.at("uptime_seconds").is_null());
    REQUIRE(value.at("last_error").is_null());
    const auto decoded = take(Manager::ManagerProtocolCodec::decodeResponse(frame));
    const auto& actual = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(decoded.body));
    REQUIRE(!actual.startedAt && !actual.uptime && !actual.lastError);

    Domain::ManagerSettingsPatch emptyPatch;
    const auto patchFrame = take(Manager::ManagerProtocolCodec::encodeRequest(
        request(Manager::ManagerSettingsUpdateRequest{emptyPatch, false})));
    const auto patchRoot = Json::parse(payloadText(patchFrame));
    const auto& patch = patchRoot.at("params").at("patch");
    REQUIRE(patch.size() == 18U);
    for (const auto& field : patch) REQUIRE(field.is_null());
    const auto decodedPatch = take(
        Manager::ManagerProtocolCodec::decodeRequest(patchFrame));
    const auto& actualPatch =
        std::get<Manager::ManagerSettingsUpdateRequest>(decodedPatch.payload).patch;
    REQUIRE(!actualPatch.dashboardHost && !actualPatch.dashboardPort);
    REQUIRE(!actualPatch.autoRestart && !actualPatch.logLevel);
}

void testTimestampPrecisionAndRepresentableBounds()
{
    using ClockDuration = Domain::UtcTimePoint::duration;

    const auto epochMilliseconds = std::chrono::milliseconds{1'767'225'600'123LL};
    const auto epochClockDuration =
        std::chrono::duration_cast<ClockDuration>(epochMilliseconds);
    const auto subMillisecondTick = ClockDuration{1};
    REQUIRE(subMillisecondTick > ClockDuration::zero());
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(
                subMillisecondTick) == std::chrono::milliseconds::zero());

    auto status = sampleStatus();
    status.startedAt = Domain::UtcTimePoint{
        epochClockDuration + subMillisecondTick};
    const auto subMillisecondFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{status})));
    const auto subMillisecondJson = Json::parse(payloadText(subMillisecondFrame));
    REQUIRE(subMillisecondJson.at("result").at("value").at(
                "started_at_utc_ms") == epochMilliseconds.count());
    const auto subMillisecondDecoded = take(
        Manager::ManagerProtocolCodec::decodeResponse(subMillisecondFrame));
    const auto& truncatedStatus = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(subMillisecondDecoded.body));
    REQUIRE(truncatedStatus.startedAt ==
            std::optional<Domain::UtcTimePoint>{
                Domain::UtcTimePoint{epochClockDuration}});

    const auto maximumMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ClockDuration::max());
    const auto maximumCount =
        static_cast<std::int64_t>(maximumMilliseconds.count());
    REQUIRE(maximumCount < (std::numeric_limits<std::int64_t>::max)());

    auto root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["started_at_utc_ms"] = 0;
    const auto epochResponse = take(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)));
    const auto& epochStatus = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(epochResponse.body));
    REQUIRE(epochStatus.startedAt == Domain::UtcTimePoint{});

    root["result"]["value"]["started_at_utc_ms"] = maximumCount;
    const auto maximumResponse = take(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)));
    const auto& maximumStatus = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(maximumResponse.body));
    REQUIRE(maximumStatus.startedAt ==
            Domain::UtcTimePoint{std::chrono::duration_cast<ClockDuration>(
                maximumMilliseconds)});
    const auto maximumRoundTrip = take(
        Manager::ManagerProtocolCodec::encodeResponse(maximumResponse));
    REQUIRE(Json::parse(payloadText(maximumRoundTrip))
                .at("result")
                .at("value")
                .at("started_at_utc_ms") == maximumCount);

    root["result"]["value"]["started_at_utc_ms"] = maximumCount + 1;
    requireError(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
        Domain::ErrorCodes::InvalidRequest);
    root["result"]["value"]["started_at_utc_ms"] =
        (std::numeric_limits<std::int64_t>::max)();
    requireError(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
        Domain::ErrorCodes::InvalidRequest);
    root["result"]["value"]["started_at_utc_ms"] =
        (std::numeric_limits<std::int64_t>::min)();
    requireError(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
        Domain::ErrorCodes::InvalidRequest);
}

void testHostileFramingAndJsonAreRejected()
{
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest({}),
        Domain::ErrorCodes::MalformedMessage);
    for (std::size_t size = 1U; size < 4U; ++size) {
        const std::vector<std::byte> incomplete(size, std::byte{});
        requireError(
            Manager::ManagerProtocolCodec::decodeRequest(incomplete),
            Domain::ErrorCodes::MalformedMessage);
    }
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(
            std::vector<std::byte>(4U, std::byte{})),
        Domain::ErrorCodes::MalformedMessage);

    auto valid = take(Manager::ManagerProtocolCodec::encodeRequest(
        request(Manager::ManagerStatusRequest{})));
    auto incomplete = valid;
    incomplete.pop_back();
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(incomplete),
        Domain::ErrorCodes::MalformedMessage);
    auto trailing = valid;
    trailing.push_back(static_cast<std::byte>('x'));
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(trailing),
        Domain::ErrorCodes::MalformedMessage);
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(valid, valid.size() - 5U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(valid, 0U),
        Domain::ErrorCodes::InvalidRequest);

    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText("{not-json")),
        Domain::ErrorCodes::MalformedMessage);
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText("[]")),
        Domain::ErrorCodes::InvalidRequest);

    std::string invalidUtf8{"{\"x\":\""};
    invalidUtf8.push_back(static_cast<char>(0xc0));
    invalidUtf8.push_back(static_cast<char>(0xaf));
    invalidUtf8 += "\"}";
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(invalidUtf8)),
        Domain::ErrorCodes::MalformedMessage);

    std::string embeddedNul{"{\"x\":\"a"};
    embeddedNul.push_back('\0');
    embeddedNul += "b\"}";
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(embeddedNul)),
        Domain::ErrorCodes::MalformedMessage);

    auto escapedNul = requestJson(Manager::ManagerStatusRequest{}).dump();
    replaceOne(
        escapedNul,
        "\"manager-request-7\"",
        "\"manager\\u0000request\"");
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(escapedNul)),
        Domain::ErrorCodes::MalformedMessage);

    auto duplicateRoot = requestJson(Manager::ManagerStatusRequest{}).dump();
    duplicateRoot.insert(duplicateRoot.size() - 1U, ",\"version\":1");
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(duplicateRoot)),
        Domain::ErrorCodes::MalformedMessage);

    auto duplicateNested =
        requestJson(Domain::ManagerControlRequest{
            Domain::ManagerControlAction::Start})
            .dump();
    replaceOne(
        duplicateNested,
        "\"params\":{\"action\":\"start\"}",
        "\"params\":{\"action\":\"start\",\"action\":\"stop\"}");
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(duplicateNested)),
        Domain::ErrorCodes::MalformedMessage);

    std::string depth64(64U, '[');
    depth64 += "0";
    depth64.append(64U, ']');
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(depth64)),
        Domain::ErrorCodes::InvalidRequest);
    std::string depth65(65U, '[');
    depth65 += "0";
    depth65.append(65U, ']');
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(depth65)),
        Domain::ErrorCodes::LimitExceeded);
}

void testHostileRequestSchemaAndIdentityAreRejected()
{
    const auto expectInvalid = [](Json root) {
        requireError(
            Manager::ManagerProtocolCodec::decodeRequest(frameFromJson(root)),
            Domain::ErrorCodes::InvalidRequest);
    };

    auto root = requestJson(Manager::ManagerStatusRequest{});
    root["unknown"] = true;
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root.erase("method");
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["version"] = 2;
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromJson(root)),
        Domain::ErrorCodes::UnsupportedVersion);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["request_id"] = "has spaces";
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["correlation_id"] = "";
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["nonce"] = std::string(64U, 'A');
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["nonce"] = std::string(63U, 'a');
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["deadline_utc_ms"] = -1;
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["deadline_utc_ms"] = 1.5;
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["method"] = "manager.unknown";
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["params"] = Json::array();
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["params"]["unknown"] = true;
    expectInvalid(root);

    root = requestJson(Domain::ManagerControlRequest{
        Domain::ManagerControlAction::Start});
    root["params"]["action"] = "invalid";
    expectInvalid(root);
    root = requestJson(Domain::ManagerControlRequest{
        Domain::ManagerControlAction::Start});
    root["params"]["unknown"] = false;
    expectInvalid(root);

    root = requestJson(Manager::ManagerCancelRequest{
        identifier<Domain::OperationId>(
            "10000000-0000-4000-8000-000000000016")});
    root["params"]["operation_id"] = "not-a-uuid";
    expectInvalid(root);

    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["unknown"] = 1;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["unknown"] = 1;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["dashboard_host"] = "0.0.0.0";
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["dashboard_port"] = 0;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["shell_timeout_seconds"] = 121;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["log_level"] = "warning";
    expectInvalid(root);
}

void testHostileResponseSchemasAndModelsAreRejected()
{
    const auto expectInvalid = [](Json root) {
        requireError(
            Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
            Domain::ErrorCodes::InvalidRequest);
    };

    auto root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["unknown"] = true;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["error"] = Json{
        {"code", "conflict"},
        {"evidence_id", nullptr},
        {"message", "both"},
        {"retryable", false}};
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root.erase("result");
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["result"]["unknown"] = true;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["result"]["type"] = "unknown";
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["result"]["value"]["unknown"] = true;
    expectInvalid(root);

    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["unknown"] = true;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["dashboard_host"] = "0.0.0.0";
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["shell_timeout_seconds"] = 121;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["log_level"] = "warning";
    expectInvalid(root);

    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["unknown"] = 1;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["state"] = "unknown";
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["uptime_seconds"] = -1;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["process_id"] =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1U;
    expectInvalid(root);

    Manager::ManagerResponse errorResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{
            Domain::makeError("conflict", "failed", false, "evidence-1")}};
    root = Json::parse(payloadText(take(
        Manager::ManagerProtocolCodec::encodeResponse(errorResponse))));
    root["error"]["unknown"] = true;
    expectInvalid(root);
    root.erase("unknown");
    root["error"].erase("unknown");
    root["error"]["code"] = "not a code";
    expectInvalid(root);
    root["error"]["code"] = "conflict";
    root["error"]["evidence_id"] = "bad evidence";
    expectInvalid(root);
}

void testHostileSettingsUpdateOutcomeIsRejected()
{
    const auto expectInvalid = [](Json root) {
        requireError(
            Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
            Domain::ErrorCodes::InvalidRequest);
    };
    const auto valid = [] {
        return responseJson(Manager::ManagerResult{
            sampleSettingsUpdateOutcome(true, false)});
    };

    auto root = valid();
    root["result"]["value"].erase("settings");
    expectInvalid(root);
    root = valid();
    root["result"]["value"].erase("applied");
    expectInvalid(root);
    root = valid();
    root["result"]["value"].erase("binding_changed");
    expectInvalid(root);
    root = valid();
    root["result"]["value"].erase("status");
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["unknown"] = true;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["applied"] = "true";
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["binding_changed"] = 1;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"] = nullptr;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["status"] = Json::array();
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["dashboard_host"] = "0.0.0.0";
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["status"]["state"] = "unknown";
    expectInvalid(root);

    root = valid();
    root["result"]["value"]["settings"]["dashboard_host"] = "127.0.0.1";
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["dashboard_port"] = 7788;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]
        ["dashboard_refresh_interval_seconds"] = 18;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["auto_restart"] = true;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["watchdog_interval_seconds"] = 12;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["open_browser_on_start"] = false;
    expectInvalid(root);
}

void testInvalidTypedModelsFailClosed()
{
    auto invalidVersion = request(Manager::ManagerStatusRequest{});
    invalidVersion.version = 2U;
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(invalidVersion),
        Domain::ErrorCodes::UnsupportedVersion);

    auto negativeDeadline = request(Manager::ManagerStatusRequest{});
    negativeDeadline.deadlineUtcMilliseconds = -1;
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(negativeDeadline),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidControl = request(Domain::ManagerControlRequest{
        static_cast<Domain::ManagerControlAction>(255)});
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(invalidControl),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidPatch = samplePatch();
    invalidPatch.shellTimeout = 121s;
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagerSettingsUpdateRequest{invalidPatch, false})),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidSettings = sampleSettings();
    invalidSettings.dashboardHost = "0.0.0.0";
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidSettings})),
        Domain::ErrorCodes::InvalidRequest);

    auto inconsistentOutcome = sampleSettingsUpdateOutcome(false, true);
    inconsistentOutcome.settings.dashboardPort = 7788U;
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{inconsistentOutcome})),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidStatus = sampleStatus();
    invalidStatus.state = static_cast<Domain::ManagerServiceState>(255);
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidStatus})),
        Domain::ErrorCodes::InvalidRequest);

    invalidStatus = sampleStatus();
    invalidStatus.version = std::string{"bad\0version", 11U};
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidStatus})),
        Domain::ErrorCodes::InvalidRequest);

    invalidStatus = sampleStatus();
    invalidStatus.version = std::string{
        {static_cast<char>(0xc3), static_cast<char>(0x28)}};
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidStatus})),
        Domain::ErrorCodes::InvalidRequest);

    Manager::ManagerResponse invalidError{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{
            Domain::makeError("conflict", "failed", false, "bad evidence")}};
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(invalidError),
        Domain::ErrorCodes::InvalidRequest);
}

void testExactMaximumBoundBehavior()
{
    const auto normal = request(Manager::ManagerStatusRequest{});
    const auto frame = take(Manager::ManagerProtocolCodec::encodeRequest(normal));
    const auto payloadBytes = frame.size() - 4U;
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(
                    normal, payloadBytes)) == frame);
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(normal, payloadBytes - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(Manager::ManagerProtocolCodec::decodeRequest(frame, payloadBytes));
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frame, payloadBytes - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    Manager::ManagerResponse exactResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-maximum"),
        identifier<Domain::CorrelationId>("manager-correlation-maximum"),
        Manager::ManagerResponseBody{
            Domain::makeError("conflict", "", false, std::nullopt)}};
    const auto baseline = take(
        Manager::ManagerProtocolCodec::encodeResponse(exactResponse));
    const auto baselinePayloadBytes = baseline.size() - 4U;
    REQUIRE(baselinePayloadBytes <
            Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes);
    auto& error = std::get<Domain::Error>(exactResponse.body);
    error.message.assign(
        Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes -
            baselinePayloadBytes,
        'x');
    const auto exact = take(
        Manager::ManagerProtocolCodec::encodeResponse(exactResponse));
    REQUIRE(exact.size() ==
            Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes + 4U);
    REQUIRE(Manager::ManagerProtocolCodec::decodeResponse(exact));
    error.message.push_back('x');
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(exactResponse),
        Domain::ErrorCodes::PayloadTooLarge);
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"type-and-prefix", testTypeAndPrefixContract},
        {"request-round-trips", testEveryRequestMethodRoundTripsDeterministically},
        {"response-round-trips", testResponseResultAndErrorRoundTrips},
        {"managed-run-round-trips", testManagedRunResultRoundTrips},
        {"manager-telemetry-round-trips",
         testManagerTelemetryRoundTripsWithoutLosingAvailability},
        {"tool-outcome-duration", testToolOutcomePreservesMeasuredDuration},
        {"project-workflow-round-trips", testProjectWorkflowRoundTrips},
        {"maintenance-round-trips", testMaintenanceRoundTrips},
        {"project-run-history-round-trips", testProjectRunHistoryRoundTrips},
        {"settings-update-outcome-round-trips",
         testSettingsUpdateOutcomeRoundTrips},
        {"optional-fields", testNullOptionalFieldsAreLossless},
        {"timestamp-precision-bounds", testTimestampPrecisionAndRepresentableBounds},
        {"hostile-framing-json", testHostileFramingAndJsonAreRejected},
        {"hostile-request", testHostileRequestSchemaAndIdentityAreRejected},
        {"hostile-response", testHostileResponseSchemasAndModelsAreRejected},
        {"hostile-settings-update-outcome",
         testHostileSettingsUpdateOutcomeIsRejected},
        {"invalid-models", testInvalidTypedModelsFailClosed},
        {"exact-maximum", testExactMaximumBoundBehavior}};

    try {
        for (const auto& [name, run] : tests) {
            run();
            std::cout << "PASS " << name << '\n';
        }
        std::cout << "PASS manager protocol codec assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL manager protocol codec: " << error.what() << '\n';
        return 1;
    }
}
