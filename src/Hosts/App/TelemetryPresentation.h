#pragma once

#include "ForgeConductor/Domain/ManagerTelemetryModels.h"

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Hosts::App {

struct MetricPresentation final {
    std::string value;
    std::string state;
    std::optional<double> gaugePercent;
};

struct TelemetryPresentation final {
    MetricPresentation cpu;
    MetricPresentation ram;
    MetricPresentation gpu;
    MetricPresentation context;
    std::string managerStatus;
    std::string providerStatus;
    std::string storeStatus;
    std::string continuityStatus;
    std::vector<double> cpuHistory;
    std::vector<double> ramHistory;
    std::vector<double> latencyHistoryMilliseconds;
    std::vector<std::string> timeline;
};

[[nodiscard]] inline std::string availabilityText(
    const Domain::TelemetryMetricAvailability availability)
{
    return std::string{Domain::telemetryMetricAvailabilityName(availability)};
}

[[nodiscard]] inline MetricPresentation percentPresentation(
    const Domain::TelemetryMetric<double>& metric)
{
    if (!metric.value) {
        return MetricPresentation{
            "Unavailable",
            metric.unavailableReason.value_or(availabilityText(metric.availability)),
            std::nullopt};
    }
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << *metric.value << '%';
    return MetricPresentation{
        value.str(),
        metric.stale ? "Stale · " + metric.source : "Measured · " + metric.source,
        std::clamp(*metric.value, 0.0, 100.0)};
}

[[nodiscard]] inline TelemetryPresentation makeTelemetryPresentation(
    const Domain::ManagerTelemetrySnapshot& snapshot)
{
    TelemetryPresentation presentation;
    presentation.cpu = percentPresentation(snapshot.resources.cpuPercent);
    presentation.ram = percentPresentation(snapshot.resources.ramPercent);
    if (snapshot.resources.gpus.empty()) {
        presentation.gpu = MetricPresentation{
            "Unavailable", "No DXGI adapter was reported.", std::nullopt};
    } else {
        const auto& gpu = snapshot.resources.gpus.front();
        if (gpu.utilizationPercent) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(1)
                  << *gpu.utilizationPercent << '%';
            presentation.gpu = MetricPresentation{
                value.str(), "Measured · " + gpu.name,
                std::clamp(*gpu.utilizationPercent, 0.0, 100.0)};
        } else {
            presentation.gpu = MetricPresentation{
                "Unavailable",
                "Utilization unsupported · " + gpu.name +
                    (gpu.dedicatedBytesTotal
                        ? " · dedicated memory detected"
                        : " · memory capacity unavailable"),
                std::nullopt};
        }
    }
    if (snapshot.context.authoritative && snapshot.context.retainedTokens) {
        const auto capacity = snapshot.context.capacityTokens;
        const auto retained = *snapshot.context.retainedTokens;
        const auto percent = capacity == 0U ? 0.0 :
            std::clamp(100.0 * static_cast<double>(retained) /
                static_cast<double>(capacity), 0.0, 100.0);
        presentation.context = MetricPresentation{
            std::to_string(retained) + " / " + std::to_string(capacity) +
                " tokens",
            snapshot.context.headroomTokens
                ? std::to_string(*snapshot.context.headroomTokens) +
                    " tokens available after reserves"
                : "No remaining headroom after reserves",
            percent};
    } else {
        presentation.context = MetricPresentation{
            "No selected run",
            "Enter or start a run to display authoritative retained context.",
            std::nullopt};
    }

    presentation.managerStatus = snapshot.manager.serviceActive
        ? "Service active · PID " + std::to_string(snapshot.manager.processId)
        : "Service inactive";
    presentation.providerStatus =
        (snapshot.provider.secure ? "HTTPS · " : "HTTP · ") +
        snapshot.provider.host + ':' + std::to_string(snapshot.provider.port);
    if (snapshot.provider.model) {
        presentation.providerStatus += " · " + *snapshot.provider.model;
    }
    if (snapshot.storeHealthy.value) {
        presentation.storeStatus = *snapshot.storeHealthy.value
            ? "Healthy · operational reads succeeded"
            : "Unhealthy";
    } else {
        presentation.storeStatus =
            availabilityText(snapshot.storeHealthy.availability) + " · " +
            snapshot.storeHealthy.unavailableReason.value_or("No store observation");
    }
    presentation.continuityStatus = snapshot.continuity.runId
        ? "Context-only · Manager-owned · " +
            snapshot.continuity.runId->value()
        : "Context-only · no selected run";

    presentation.cpuHistory.reserve(snapshot.resources.history.size());
    presentation.ramHistory.reserve(snapshot.resources.history.size());
    for (const auto& point : snapshot.resources.history) {
        presentation.cpuHistory.push_back(std::clamp(point.cpuPercent, 0.0, 100.0));
        presentation.ramHistory.push_back(std::clamp(point.ramPercent, 0.0, 100.0));
    }

    presentation.timeline.reserve(snapshot.recentEvents.size() + 1U);
    for (const auto& event : snapshot.recentEvents) {
        auto item = event.tool + " · " + event.status;
        if (event.duration) {
            item += " · " + std::to_string(event.duration->count()) + " ms";
            presentation.latencyHistoryMilliseconds.push_back(
                static_cast<double>(event.duration->count()));
        }
        if (event.error) item += " · " + *event.error;
        presentation.timeline.push_back(std::move(item));
    }
    if (snapshot.selectedRun) {
        presentation.timeline.push_back(
            "Run " + snapshot.selectedRun->record.runId.value() +
            " · retained " +
            (snapshot.selectedRun->record.retainedContextTokens
                ? std::to_string(
                    *snapshot.selectedRun->record.retainedContextTokens) + " tokens"
                : std::string{"awaiting authoritative context"}));
    }
    if (presentation.timeline.empty()) {
        presentation.timeline.push_back("No recent Manager activity.");
    }
    return presentation;
}

[[nodiscard]] inline std::string telemetryDetailText(
    const Domain::ManagerTelemetrySnapshot& snapshot,
    const std::string_view page)
{
    if (page == "Projects") {
        std::string text = "Registered projects: " +
            std::to_string(snapshot.projects.size()) +
            "\nOpen sessions: " + std::to_string(snapshot.openSessionCount) +
            " · Recent sessions: " + std::to_string(snapshot.recentSessionCount);
        for (const auto& project : snapshot.projects) {
            text += "\n• " + project.value();
        }
        if (snapshot.selectedRun) {
            text += "\n\nSelected run project: " +
                snapshot.selectedRun->record.projectId.value();
        }
        return text;
    }
    if (page == "Tools" || page == "LM Studio MCP") {
        std::string text = "Available Manager tools: " +
            std::to_string(snapshot.tools.size());
        for (const auto& tool : snapshot.tools) text += "\n• " + tool;
        return text;
    }
    if (page == "Events & Evidence" || page == "Feed") {
        const auto presentation = makeTelemetryPresentation(snapshot);
        std::string text = "Recent operational events: " +
            std::to_string(snapshot.recentEvents.size());
        for (const auto& event : presentation.timeline) text += "\n• " + event;
        return text;
    }
    if (page == "Runtimes") {
        std::string text = "Telemetry runtime: " + snapshot.runtime +
            "\nHost: " + snapshot.resources.host + " · " +
            snapshot.resources.platform + " · " + snapshot.resources.architecture +
            "\nObserved Manager processes: " +
            std::to_string(snapshot.resources.processes.size());
        if (snapshot.runtimeDiagnostics) {
            const auto& runtime = *snapshot.runtimeDiagnostics;
            text += "\nBackground threads: " +
                std::to_string(runtime.backgroundThreads) +
                " · Open repositories: " +
                std::to_string(runtime.openRepositories) +
                " · Open databases: " + std::to_string(runtime.openDatabases) +
                " · Child processes: " + std::to_string(runtime.childProcesses);
        }
        for (const auto& process : snapshot.resources.processes) {
            text += "\n• " + process.name + " · PID " +
                std::to_string(process.processId) + " · " +
                std::to_string(process.workingSetBytes) + " bytes working set";
        }
        return text;
    }
    if (page == "Provider") {
        std::string text = "Endpoint: " +
            std::string{snapshot.provider.secure ? "https://" : "http://"} +
            snapshot.provider.host + ':' + std::to_string(snapshot.provider.port);
        text += "\nModel: " + snapshot.provider.model.value_or("automatic");
        text += "\nCanonical response: " +
            (snapshot.provider.responseId
                ? snapshot.provider.responseId->value()
                : std::string{"no selected run response"});
        return text;
    }
    if (page == "Manager" || page == "Diagnostics" || page == "Settings") {
        const auto presentation = makeTelemetryPresentation(snapshot);
        return presentation.managerStatus +
            "\nStore: " + presentation.storeStatus +
            "\nCPU: " + presentation.cpu.value + " · RAM: " +
            presentation.ram.value + " · GPU: " + presentation.gpu.value +
            "\nContext capacity: " +
            std::to_string(snapshot.context.capacityTokens) +
            " · Next response reserve: " +
            std::to_string(snapshot.context.nextResponseReserveTokens) +
            " · Handoff reserve: " +
            std::to_string(snapshot.context.handoffReserveTokens);
    }
    return "Agents: " + std::to_string(snapshot.openSessionCount) +
        " open sessions · Presence: " + std::to_string(snapshot.presenceCount) +
        " · Tools: " + std::to_string(snapshot.tools.size());
}

} // namespace ForgeConductor::Hosts::App
