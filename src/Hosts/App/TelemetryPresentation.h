#pragma once

#include "ForgeConductor/Domain/ManagerTelemetryModels.h"

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
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

} // namespace ForgeConductor::Hosts::App
