#pragma once

#include "ForgeConductor/Domain/ManagerTelemetryModels.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
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
    std::string systemStatus;
    std::string samplingStatus;
    std::string diskStatus;
    std::string workflowStatus;
    std::vector<std::string> cpuLogicalRows;
    std::vector<double> cpuLogicalValues;
    std::vector<std::string> gpuRows;
    std::vector<std::string> volumeRows;
    std::vector<std::string> processRows;
    std::vector<double> cpuHistory;
    std::vector<double> ramHistory;
    std::vector<double> gpuHistory;
    std::vector<double> diskHistoryBytesPerSecond;
    std::vector<double> latencyHistoryMilliseconds;
    std::vector<std::string> timeline;
};

[[nodiscard]] inline std::string sampleAgeText(
    const Domain::UtcTimePoint now,
    const std::optional<Domain::UtcTimePoint>& capturedAt)
{
    if (!capturedAt || *capturedAt > now) return "sample time unavailable";
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *capturedAt).count();
    return "sample age " + std::to_string(age) + " ms";
}

[[nodiscard]] inline std::string bytesText(const std::uint64_t bytes)
{
    constexpr double Gibibyte = 1024.0 * 1024.0 * 1024.0;
    constexpr double Mebibyte = 1024.0 * 1024.0;
    std::ostringstream value;
    value << std::fixed << std::setprecision(1);
    if (bytes >= static_cast<std::uint64_t>(Gibibyte)) {
        value << static_cast<double>(bytes) / Gibibyte << " GiB";
    } else {
        value << static_cast<double>(bytes) / Mebibyte << " MiB";
    }
    return value.str();
}

[[nodiscard]] inline std::wstring scopedViewStateValueName(
    const std::wstring_view base,
    const std::optional<std::string>& scope)
{
    if (!scope) return std::wstring{base};
    constexpr std::uint64_t Offset = 14695981039346656037ULL;
    constexpr std::uint64_t Prime = 1099511628211ULL;
    std::uint64_t hash = Offset;
    for (auto byte : *scope) {
        if (byte == '\\') byte = '/';
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
        hash ^= static_cast<unsigned char>(byte);
        hash *= Prime;
    }
    std::array<wchar_t, 18> suffix{};
    static_cast<void>(swprintf_s(
        suffix.data(), suffix.size(), L".%016llx",
        static_cast<unsigned long long>(hash)));
    return std::wstring{base} + suffix.data();
}

[[nodiscard]] inline bool containsProjectId(
    const std::vector<Domain::ProjectId>& projects,
    const std::string_view selectedProjectId)
{
    return !selectedProjectId.empty() && std::ranges::any_of(
        projects,
        [selectedProjectId](const Domain::ProjectId& project) {
            return project.value() == selectedProjectId;
        });
}

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
    } else if (snapshot.continuity.runId) {
        presentation.context = MetricPresentation{
            "Awaiting run context",
            "Exact run selected; the provider has not returned authoritative retained usage yet.",
            std::nullopt};
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

    presentation.systemStatus = snapshot.resources.host + " · " +
        snapshot.resources.platform + " " + snapshot.resources.architecture;
    presentation.samplingStatus = "Target " +
        std::to_string(snapshot.resources.targetSampleIntervalMilliseconds) + " ms";
    if (snapshot.resources.measuredSampleIntervalMilliseconds) {
        std::ostringstream interval;
        interval << std::fixed << std::setprecision(0)
                 << *snapshot.resources.measuredSampleIntervalMilliseconds;
        presentation.samplingStatus += " · measured " + interval.str() + " ms";
    }
    presentation.samplingStatus += " · native resource sample";

    if (snapshot.resources.cpuPerLogicalProcessor.value) {
        const auto& logical = *snapshot.resources.cpuPerLogicalProcessor.value;
        const auto* frequencies = snapshot.resources.cpuPerLogicalFrequencyMhz.value
            ? &*snapshot.resources.cpuPerLogicalFrequencyMhz.value : nullptr;
        presentation.cpuLogicalRows.reserve(logical.size());
        for (std::size_t index{}; index < logical.size(); ++index) {
            std::ostringstream row;
            row << "Logical " << index << " · " << std::fixed
                << std::setprecision(1) << logical[index] << '%';
            if (frequencies != nullptr && index < frequencies->size()) {
                row << " · " << (*frequencies)[index] << " MHz";
            }
            presentation.cpuLogicalRows.push_back(row.str());
            presentation.cpuLogicalValues.push_back(
                std::clamp(logical[index], 0.0, 100.0));
        }
    }
    for (const auto& gpu : snapshot.resources.gpus) {
        std::string row = gpu.name + " · " + gpu.vendor;
        if (gpu.dedicatedBytesTotal) row += " · " + bytesText(*gpu.dedicatedBytesTotal);
        row += "\n" + gpu.memoryScope + " · " +
            sampleAgeText(snapshot.capturedAt, gpu.capturedAt);
        presentation.gpuRows.push_back(std::move(row));
        for (const auto& engine : gpu.engines) {
            std::ostringstream engineRow;
            engineRow << "  " << engine.name << " · " << std::fixed
                      << std::setprecision(1) << engine.utilizationPercent << '%';
            presentation.gpuRows.push_back(engineRow.str());
        }
    }
    if (snapshot.resources.diskIo.value) {
        const auto& disk = *snapshot.resources.diskIo.value;
        presentation.diskStatus = "Read " + bytesText(static_cast<std::uint64_t>(
            disk.readBytesPerSecond)) + "/s · Write " +
            bytesText(static_cast<std::uint64_t>(disk.writeBytesPerSecond)) +
            "/s · " + std::to_string(static_cast<std::uint64_t>(
                disk.readOperationsPerSecond + disk.writeOperationsPerSecond)) + " IOPS · " +
            snapshot.resources.diskIo.source + " · " +
            sampleAgeText(snapshot.capturedAt, snapshot.resources.diskIo.capturedAt);
    } else {
        presentation.diskStatus = snapshot.resources.diskIo.unavailableReason.value_or(
            availabilityText(snapshot.resources.diskIo.availability));
    }
    for (const auto& volume : snapshot.resources.disks) {
        std::ostringstream row;
        row << volume.mount.value() << " · " << volume.fileSystem << " · "
            << std::fixed << std::setprecision(1) << volume.percent
            << "% used · " << bytesText(volume.availableBytes) << " free · "
            << sampleAgeText(snapshot.capturedAt, volume.capturedAt);
        presentation.volumeRows.push_back(row.str());
    }
    for (const auto& process : snapshot.resources.processes) {
        std::ostringstream row;
        row << process.name << " · PID " << process.processId << " · "
            << std::fixed << std::setprecision(1) << process.cpuPercent
            << "% CPU · " << bytesText(process.workingSetBytes)
            << " RAM · " << process.threadCount << " threads · "
            << sampleAgeText(snapshot.capturedAt, process.capturedAt);
        presentation.processRows.push_back(row.str());
    }
    presentation.workflowStatus = std::to_string(snapshot.projects.size()) +
        " projects · " + std::to_string(snapshot.tools.size()) + " tools · " +
        std::to_string(snapshot.openSessionCount) + " open agents · " +
        std::to_string(snapshot.presenceCount) + " presence records";

    presentation.cpuHistory.reserve(snapshot.resources.history.size());
    presentation.ramHistory.reserve(snapshot.resources.history.size());
    presentation.gpuHistory.reserve(snapshot.resources.history.size());
    presentation.diskHistoryBytesPerSecond.reserve(snapshot.resources.history.size());
    const auto chartStart = snapshot.resources.capturedAt - std::chrono::seconds{60};
    for (const auto& point : snapshot.resources.history) {
        if (point.timestamp < chartStart) continue;
        presentation.cpuHistory.push_back(std::clamp(point.cpuPercent, 0.0, 100.0));
        presentation.ramHistory.push_back(std::clamp(point.ramPercent, 0.0, 100.0));
        presentation.gpuHistory.push_back(std::clamp(
            point.gpuPercent.value_or(0.0), 0.0, 100.0));
        presentation.diskHistoryBytesPerSecond.push_back(
            std::max(0.0, point.diskBytesPerSecond));
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
