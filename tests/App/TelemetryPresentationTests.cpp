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
    snapshot.resources.gpus.push_back(Domain::GpuMetrics{
        "vendor", "DXGI adapter", std::nullopt, 1024U, 4096U,
        std::nullopt, true});
    snapshot.resources.history = {
        Domain::HistoryPoint{
            time - std::chrono::seconds{1}, 25.0, 60.0, std::nullopt,
            0.0, 0U, Domain::TelemetryHealth::Ok},
        Domain::HistoryPoint{
            time, 48.5, 67.25, std::nullopt,
            0.0, 1U, Domain::TelemetryHealth::Ok}};
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
    require(presentation.gpu.value == "Unavailable", "GPU unavailable value");
    require(presentation.gpu.state.find("Utilization unsupported") !=
                std::string::npos,
            "GPU unsupported explanation");
    require(presentation.context.value == "12000 / 32768 tokens",
            "authoritative context direct values");
    require(presentation.context.state ==
                "10528 tokens available after reserves",
            "Manager-provided headroom");
    require(presentation.cpuHistory.size() == 2U &&
                presentation.ramHistory.size() == 2U,
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
