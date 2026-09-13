#include "TelemetryPresentation.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace App = ForgeConductor::Hosts::App;
namespace Domain = ForgeConductor::Domain;

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error{message};
}

void testPresentationPreservesTelemetryMeaning()
{
    const auto time = Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}};
    Domain::ManagerTelemetrySnapshot snapshot{
        time,
        Domain::ManagerResourceSnapshot{time},
        Domain::ManagerStatus{
            true, true, Domain::ManagerServiceState::Running, true, true, true,
            4242U, std::nullopt, std::nullopt, 0U, std::nullopt, true,
            std::chrono::seconds{3}, false, "127.0.0.1", 7788U,
            std::chrono::seconds{8},
            Domain::PathText::create("C:\\TelemetryPresentationTest").value(),
            "test"},
        std::nullopt,
        Domain::ManagerProviderSnapshot{},
        Domain::ManagerContextSnapshot{},
        Domain::ManagerContinuitySnapshot{},
        std::nullopt,
        {},
        {},
        0U,
        0U,
        0U,
        {},
        Domain::makeUnavailableTelemetryMetric<bool>(
            Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
            time,
            "manager_operational_store",
            "Store busy"),
        "windows-native"};
    const auto priorCpu = Domain::makeAvailableTelemetryMetric<double>(
        48.5, time - std::chrono::seconds{1}, "GetSystemTimes");
    snapshot.resources.cpuPercent = Domain::makeStaleTelemetryMetric(
        priorCpu,
        Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
        time,
        "Counter reset");
    snapshot.resources.ramPercent = Domain::makeAvailableTelemetryMetric<double>(
        67.25, time, "GlobalMemoryStatusEx");
    snapshot.resources.cpuPerLogicalProcessor =
        Domain::makeAvailableTelemetryMetric<std::vector<double>>(
            {25.0, 75.0}, time, "PDH logical CPU");
    snapshot.resources.cpuPerLogicalFrequencyMhz =
        Domain::makeAvailableTelemetryMetric<std::vector<std::uint32_t>>(
            {3400U, 3400U}, time, "PDH CPU frequency");
    Domain::GpuMetrics gpu{
        "vendor", "DXGI adapter", 33.5, 1024U, 4096U,
        std::nullopt, true};
    gpu.adapterId = "0:1";
    gpu.engines.push_back(Domain::GpuEngineMetrics{"3D 0", 33.5});
    gpu.capturedAt = time - std::chrono::milliseconds{250};
    gpu.utilizationSource = "PDH GPU Engine";
    gpu.memoryScope = "current-process local memory; adapter capacity";
    snapshot.resources.gpus.push_back(std::move(gpu));
    snapshot.resources.diskIo = Domain::makeAvailableTelemetryMetric(
        Domain::DiskIoMetrics{1024.0, 2048.0, 2.0, 3.0},
        time - std::chrono::seconds{1}, "PDH PhysicalDisk");
    snapshot.resources.disks.push_back(Domain::DiskVolume{
        "C:\\", Domain::PathText::create("C:\\").value(), "NTFS",
        10'000U, 4'000U, 6'000U, 40.0, time, "GetDiskFreeSpaceEx"});
    snapshot.resources.processes.push_back(Domain::ProcessMetrics{
        4242U, "ForgeConductor.Manager.exe", 2.5, 1024U * 1024U,
        900U * 1024U, 12U, 30U, "Toolhelp/GetProcessTimes", time});
    snapshot.resources.history = {
        Domain::HistoryPoint{
            time - std::chrono::seconds{1}, 25.0, 60.0, std::nullopt,
            0.0, 0U, Domain::TelemetryHealth::Ok},
        Domain::HistoryPoint{
            time, 48.5, 67.25, 33.5,
            3072.0, 1U, Domain::TelemetryHealth::Ok}};
    snapshot.provider.host = "127.0.0.1";
    snapshot.provider.port = 1234U;
    snapshot.context.capacityTokens = 32'768U;
    snapshot.context.retainedTokens = 12'000U;
    snapshot.context.headroomTokens = 10'528U;
    snapshot.context.authoritative = true;
    snapshot.recentEvents.push_back(Domain::AuditEvent{
        time, std::nullopt, "filesystem.read", std::nullopt, "success",
        std::chrono::milliseconds{42}, std::nullopt});
    snapshot.projects.push_back(Domain::ProjectId::parse(
        "11111111-1111-4111-8111-111111111111").value());
    snapshot.tools.push_back("filesystem.read");
    snapshot.runtimeDiagnostics = Domain::RuntimeDiagnosticSnapshot{
        time, 0U, 0U, 3U, 2U, 0U, Domain::ResourcePressureLevel::Nominal,
        0U, 4U, 0U, 1U};
    snapshot.resources.host = "workstation";
    snapshot.resources.platform = "Windows";
    snapshot.resources.architecture = "x64";
    snapshot.provider.model = "local-model";

    const auto presentation = App::makeTelemetryPresentation(snapshot);
    require(presentation.cpu.value == "48.5%", "CPU direct value");
    require(presentation.cpu.state.starts_with("Stale"), "CPU stale state");
    require(presentation.ram.value == "67.2%", "RAM direct value");
    require(presentation.gpu.value == "33.5%", "GPU measured value");
    require(presentation.cpuLogicalRows.size() == 2U &&
                presentation.cpuLogicalValues[1] == 75.0,
            "logical CPU projection");
    require(presentation.gpuRows.size() == 2U &&
                presentation.gpuRows[1].find("3D 0") != std::string::npos,
            "GPU engine projection");
    require(presentation.diskStatus.find("5 IOPS") != std::string::npos &&
                presentation.volumeRows.size() == 1U,
            "disk and volume projection");
    require(presentation.processRows.size() == 1U &&
                presentation.processRows.front().find("PID 4242") !=
                    std::string::npos,
            "relevant process projection");
    require(presentation.context.value == "12000 / 32768 tokens",
            "authoritative context direct values");
    require(presentation.context.state ==
                "10528 tokens available after reserves",
            "Manager-provided headroom");
    require(presentation.cpuHistory.size() == 2U &&
                presentation.ramHistory.size() == 2U &&
                presentation.gpuHistory.back() == 33.5 &&
                presentation.diskHistoryBytesPerSecond.back() == 3072.0,
            "bounded history projection");
    require(presentation.latencyHistoryMilliseconds.size() == 1U &&
                presentation.latencyHistoryMilliseconds.front() == 42.0,
            "event latency projection");
    require(presentation.storeStatus.find("Store busy") != std::string::npos,
            "store unavailable explanation");

    require(App::telemetryDetailText(snapshot, "Projects").find(
                "11111111-1111-4111-8111-111111111111") !=
                std::string::npos,
            "project drill-down");
    require(App::telemetryDetailText(snapshot, "Tools").find(
                "filesystem.read") != std::string::npos,
            "tool drill-down");
    require(App::telemetryDetailText(snapshot, "Events & Evidence").find(
                "42 ms") != std::string::npos,
            "event drill-down");
    const auto runtimes = App::telemetryDetailText(snapshot, "Runtimes");
    require(runtimes.find("workstation · Windows · x64") != std::string::npos &&
                runtimes.find("Background threads: 3") != std::string::npos,
            "runtime drill-down");
    require(App::telemetryDetailText(snapshot, "Provider").find(
                "local-model") != std::string::npos,
            "provider drill-down");
}

void testProjectSelectionRequiresAnAuthoritativeMatch()
{
    const auto projectA = Domain::ProjectId::parse(
        "11111111-1111-4111-8111-111111111111").value();
    const auto projectB = Domain::ProjectId::parse(
        "22222222-2222-4222-8222-222222222222").value();
    const std::vector<Domain::ProjectId> projects{projectA};

    require(App::containsProjectId(projects, projectA.value()),
            "authoritative project selection");
    require(!App::containsProjectId(projects, projectB.value()),
            "stale selection from another profile");
    require(!App::containsProjectId(projects, ""),
            "empty project selection");

    require(App::scopedViewStateValueName(
                L"SelectedProjectId", std::nullopt) == L"SelectedProjectId",
            "production view-state compatibility");
    const auto scopeA = App::scopedViewStateValueName(
        L"SelectedProjectId", std::string{"D:\\Alpha\\A"});
    const auto scopeAEquivalent = App::scopedViewStateValueName(
        L"SelectedProjectId", std::string{"d:/alpha/a"});
    const auto scopeB = App::scopedViewStateValueName(
        L"SelectedProjectId", std::string{"D:\\Alpha\\B"});
    require(scopeA == scopeAEquivalent,
            "case-insensitive normalized Windows profile scope");
    require(scopeA != scopeB,
            "independent Alpha profile view-state scope");
}

} // namespace

int main()
{
    try {
        testPresentationPreservesTelemetryMeaning();
        testProjectSelectionRequiresAnAuthoritativeMatch();
        std::cout << "Telemetry presentation tests passed: 2 groups\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Telemetry presentation tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
