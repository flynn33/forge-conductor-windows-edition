#include "ForgeConductor/Dashboard/DashboardTelemetryJsonCodec.h"
#include "ForgeConductor/Dashboard/DashboardSseFrameEncoder.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
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

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
    REQUIRE(!result.error().message.empty());
}

[[nodiscard]] Domain::UtcTimePoint utc(
    const std::int64_t milliseconds = 1'704'164'645'678LL)
{
    return Domain::UtcTimePoint{std::chrono::milliseconds{milliseconds}};
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

template <typename T>
[[nodiscard]] Domain::TelemetryMetric<T> availableRamMetric(T value)
{
    return Domain::makeAvailableTelemetryMetric(
        std::move(value), utc(), "fixture.available");
}

[[nodiscard]] Domain::TelemetrySnapshot richSnapshot()
{
    Domain::CpuMetrics cpu{
        37.5,
        {25.0, 50.0},
        2U,
        1U,
        3'600U,
        {3'500U, 3'600U},
        Domain::LoadAverage{0.5, 0.25, 0.125},
        "Forge CPU \xE2\x98\x83",
        12.5,
        25.0,
        62.5};
    Domain::RamMetrics ram{
        availableRamMetric<std::uint64_t>(
            16ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            6ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            10ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric(37.5),
        availableRamMetric(41.0),
        availableRamMetric<std::uint64_t>(
            5ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            1ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            256ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            4ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            1ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric(25.0),
        availableRamMetric<std::uint64_t>(
            7ULL * 1024ULL * 1024ULL * 1024ULL),
        availableRamMetric<std::uint64_t>(
            128ULL * 1024ULL * 1024ULL)};
    Domain::DiskVolume disk{
        "Disk0",
        path("C:\\"),
        "NTFS",
        1ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL,
        512ULL * 1024ULL * 1024ULL * 1024ULL,
        512ULL * 1024ULL * 1024ULL * 1024ULL,
        50.0};
    Domain::DiskIoMetrics diskIo{
        2.0 * 1024.0 * 1024.0,
        1.0 * 1024.0 * 1024.0,
        10.0,
        20.0};
    Domain::GpuMetrics gpu{
        "NVIDIA",
        "RTX",
        72.5,
        512ULL * 1024ULL * 1024ULL,
        1'024ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL,
        true};
    Domain::ProcessMetrics process{
        42U,
        "forge-conductor.exe",
        125.0,
        512ULL * 1024ULL * 1024ULL,
        384ULL * 1024ULL * 1024ULL,
        17U,
        91U,
        "GetProcessMemoryInfo"};
    Domain::PowerMetrics power{
        false,
        "discharging",
        75.0,
        false,
        120min};
    Domain::SystemMetrics system{
        utc(),
        "forge-host",
        "windows",
        "x64",
        std::move(cpu),
        std::move(ram),
        {std::move(disk)},
        diskIo,
        {std::move(gpu)},
        {std::move(process)},
        std::move(power)};

    std::vector<Domain::ToolDescriptor> tools{
        Domain::ToolDescriptor{
            "project_memory.status",
            "Read project memory status.",
            "project_memory",
            Domain::ToolEffect::Read,
            Domain::ToolAvailability::Available,
            true,
            false},
        Domain::ToolDescriptor{
            "fs.write",
            "Write an authorized file.",
            "filesystem",
            Domain::ToolEffect::Write,
            Domain::ToolAvailability::Disabled,
            true,
            false}};

    Domain::AgentSession session{
        take(Domain::SessionId::parse(
            "10000000-0000-4000-8000-000000000001")),
        take(Domain::AgentId::parse("implementer")),
        take(Domain::ClientId::parse("dashboard-client")),
        Domain::SessionStatus::Running,
        std::string{"Working"},
        utc(),
        utc() + 1s};
    Domain::ForgeSnapshot forge{
        utc(),
        path("C:\\Forge Home"),
        "windows-native",
        3U,
        7U,
        std::move(tools),
        {std::move(session)},
        11U,
        Domain::TelemetryHealth::Warn};

    std::vector<Domain::HistoryPoint> history{
        Domain::HistoryPoint{
            utc() - 2s,
            25.0,
            35.0,
            std::nullopt,
            3.0 * 1024.0 * 1024.0,
            4U,
            Domain::TelemetryHealth::Ok},
        Domain::HistoryPoint{
            utc() - 1s,
            37.5,
            41.0,
            72.5,
            4.0 * 1024.0 * 1024.0,
            5U,
            Domain::TelemetryHealth::Warn}};
    return Domain::TelemetrySnapshot{
        std::move(system),
        std::move(forge),
        utc() + 2s,
        std::move(history),
        "windows-native"};
}

[[nodiscard]] std::set<std::string> keys(const Json& value)
{
    std::set<std::string> result;
    for (const auto& [name, unused] : value.items()) {
        static_cast<void>(unused);
        result.insert(name);
    }
    return result;
}

void mapsTheCompleteMacCompatibleFrameContract()
{
    const auto snapshot = richSnapshot();
    const auto encoded = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeLiveFrame(snapshot, 2.0));
    const auto document = Json::parse(encoded);

    REQUIRE(keys(document) == std::set<std::string>({
        "forge", "history", "runtime", "sample_hz", "stream", "system", "updated"}));
    REQUIRE(keys(document.at("system")) == std::set<std::string>({
        "arch", "cpu", "disk", "disk_io", "gpu", "host", "platform", "power",
        "processes", "ram", "ts"}));
    REQUIRE(keys(document.at("forge")) == std::set<std::string>({
        "agent_sessions", "agents", "agents_summary", "audit_recent", "feed_stats",
        "files", "home", "jobs", "live_feed", "mcp_load", "mcp_packs",
        "mcp_servers", "mcp_tools", "orchestration", "presence",
        "presence_count", "runtime", "ts"}));
    REQUIRE(document.at("stream") == "realtime");
    REQUIRE(document.at("sample_hz") == 2.0);
    REQUIRE(document.at("runtime") == "windows-native");
    REQUIRE(std::abs(document.at("system").at("ts").get<double>() -
                     1'704'164'645.678) < 0.0001);
    REQUIRE(std::abs(document.at("updated").get<double>() -
                     1'704'164'647.678) < 0.0001);

    const auto& cpu = document.at("system").at("cpu");
    REQUIRE(cpu.at("percent") == 37.5);
    REQUIRE(cpu.at("per_cpu") == Json::array({25.0, 50.0}));
    REQUIRE(cpu.at("freq_mhz") == 3'600U);
    REQUIRE(cpu.at("freq_per_core_mhz") == Json::array({3'500U, 3'600U}));
    REQUIRE(cpu.at("load_avg").at("m15") == 0.125);
    REQUIRE(cpu.at("brand") == "Forge CPU \xE2\x98\x83");

    const auto& ram = document.at("system").at("ram");
    const std::set<std::string> ramValueKeys{
        "active_gb", "available_gb", "committed_gb", "compressed_gb",
        "paged_pool_gb", "percent", "pressure_percent", "swap_percent",
        "swap_total_gb", "swap_used_gb", "total_gb", "used_gb", "wired_gb"};
    auto expectedRamKeys = ramValueKeys;
    expectedRamKeys.insert("availability");
    REQUIRE(keys(ram) == expectedRamKeys);
    REQUIRE(ram.at("total_gb") == 16.0);
    REQUIRE(ram.at("used_gb") == 6.0);
    REQUIRE(ram.at("available_gb") == 10.0);
    REQUIRE(ram.at("percent") == 37.5);
    REQUIRE(ram.at("pressure_percent") == 41.0);
    REQUIRE(ram.at("active_gb") == 5.0);
    REQUIRE(ram.at("wired_gb") == 1.0);
    REQUIRE(ram.at("compressed_gb") == 0.25);
    REQUIRE(ram.at("swap_total_gb") == 4.0);
    REQUIRE(ram.at("swap_used_gb") == 1.0);
    REQUIRE(ram.at("swap_percent") == 25.0);
    REQUIRE(ram.at("committed_gb") == 7.0);
    REQUIRE(ram.at("paged_pool_gb") == 0.125);
    const auto& ramAvailability = ram.at("availability");
    REQUIRE(keys(ramAvailability) == ramValueKeys);
    for (const auto& name : ramValueKeys) {
        const auto& metadata = ramAvailability.at(name);
        REQUIRE(keys(metadata) == std::set<std::string>({
            "captured_at", "observed_at", "reason", "source", "stale", "state"}));
        REQUIRE(metadata.at("state") == "available");
        REQUIRE(metadata.at("stale") == false);
        REQUIRE(std::abs(metadata.at("captured_at").get<double>() -
                         1'704'164'645.678) < 0.0001);
        REQUIRE(std::abs(metadata.at("observed_at").get<double>() -
                         1'704'164'645.678) < 0.0001);
        REQUIRE(metadata.at("source") == "fixture.available");
        REQUIRE(metadata.at("reason").is_null());
    }

    const auto& diskIo = document.at("system").at("disk_io");
    REQUIRE(diskIo.at("read_mb_s") == 2.0);
    REQUIRE(diskIo.at("write_mb_s") == 1.0);
    REQUIRE(diskIo.at("total_mb_s") == 3.0);
    REQUIRE(diskIo.at("total_iops") == 30.0);
    REQUIRE(diskIo.at("read_bytes_total").is_null());
    REQUIRE(diskIo.at("write_bytes_total").is_null());
    REQUIRE(diskIo.at("read_ops_total").is_null());
    REQUIRE(diskIo.at("write_ops_total").is_null());

    const auto& gpu = document.at("system").at("gpu").at(0U);
    REQUIRE(gpu.at("util_gpu") == 72.5);
    REQUIRE(gpu.at("mem_used_mib") == 512.0);
    REQUIRE(gpu.at("mem_total_mib") == 1'024.0);
    REQUIRE(gpu.at("mem_free_mib") == 512.0);
    REQUIRE(gpu.at("shared_mem_used_mib") == 256.0);
    REQUIRE(gpu.at("direct3d_available") == true);
    REQUIRE(gpu.at("shared_memory").is_null());
    REQUIRE(gpu.at("processes").is_null());
    REQUIRE(gpu.at("metal").is_null());

    const auto& process = document.at("system").at("processes").at(0U);
    REQUIRE(process.at("rss_gb") == 0.5);
    REQUIRE(process.at("footprint_gb").is_null());
    REQUIRE(process.at("private_gb") == 0.375);
    REQUIRE(process.at("handle_count") == 91U);
    const auto& power = document.at("system").at("power");
    REQUIRE(power.at("on_ac") == false);
    REQUIRE(power.at("time_to_empty_min") == 120);
    REQUIRE(power.at("time_to_full_min").is_null());
    REQUIRE(power.at("is_charged").is_null());
    REQUIRE(power.at("source_count").is_null());

    auto chargingSystem = snapshot.system;
    chargingSystem.power.charging = true;
    const auto chargingPower = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(chargingSystem)))
                                   .at("power");
    REQUIRE(chargingPower.at("time_to_empty_min").is_null());
    REQUIRE(chargingPower.at("time_to_full_min") == 120);
    chargingSystem.power.charging = std::nullopt;
    const auto unknownDirectionPower = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(chargingSystem)))
                                             .at("power");
    REQUIRE(unknownDirectionPower.at("time_to_empty_min").is_null());
    REQUIRE(unknownDirectionPower.at("time_to_full_min").is_null());

    const auto& forge = document.at("forge");
    REQUIRE(forge.at("presence_count") == 3U);
    REQUIRE(forge.at("presence").is_null());
    REQUIRE(forge.at("mcp_servers").is_null());
    REQUIRE(forge.at("agents").is_null());
    REQUIRE(forge.at("jobs").is_null());
    REQUIRE(forge.at("live_feed").is_null());
    REQUIRE(forge.at("mcp_load").is_null());
    REQUIRE(forge.at("files").is_null());
    REQUIRE(forge.at("audit_recent").is_null());
    REQUIRE(forge.at("mcp_tools").size() == 2U);
    REQUIRE(forge.at("mcp_tools").at(0U).at("health") == "ok");
    REQUIRE(forge.at("mcp_tools").at(0U).at("live").is_null());
    REQUIRE(forge.at("mcp_tools").at(1U).at("health") == "config");
    REQUIRE(forge.at("mcp_packs").size() == 2U);
    REQUIRE(forge.at("mcp_packs").at(0U).at("pack") == "filesystem");
    REQUIRE(forge.at("mcp_packs").at(1U).at("pack") == "project_memory");
    REQUIRE(forge.at("mcp_packs").at(1U).at("active") == 1U);
    REQUIRE(forge.at("agent_sessions").at(0U).at("status") == "running");
    REQUIRE(
        forge.at("agent_sessions").at(0U).at("created_at") ==
        "2024-01-02T03:04:05.678Z");
    REQUIRE(
        forge.at("agent_sessions").at(0U).at("updated_at") ==
        "2024-01-02T03:04:06.678Z");
    REQUIRE(forge.at("agents_summary").at("total") == 1U);
    REQUIRE(forge.at("agents_summary").at("open") == 1U);
    REQUIRE(forge.at("agents_summary").at("by_status").at("running") == 1U);
    REQUIRE(forge.at("feed_stats").is_null());
    REQUIRE(forge.at("orchestration").at("health") == "warn");
    REQUIRE(forge.at("orchestration").at("health_label") == "WARN");
    REQUIRE(forge.at("orchestration").at("manager_alive").is_null());

    REQUIRE(document.at("history").size() == 2U);
    REQUIRE(document.at("history").at(0U).at("gpu").is_null());
    REQUIRE(document.at("history").at(0U).at("disk_io") == 3.0);
    REQUIRE(document.at("history").at(1U).at("orch") == "warn");
}

void mapsUnsupportedRamMetricsToNullWithExplicitMetadata()
{
    auto snapshot = richSnapshot();
    snapshot.system.ram.compressedBytes =
        Domain::makeUnavailableTelemetryMetric<std::uint64_t>(
            Domain::TelemetryMetricAvailability::Unsupported,
            utc() + 1s,
            "windows.memory.compression",
            "The selected Windows source does not expose compressed RAM.");

    const auto ram = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system)))
                         .at("ram");
    REQUIRE(ram.at("compressed_gb").is_null());
    REQUIRE(ram.at("total_gb") == 16.0);
    const auto& metadata = ram.at("availability").at("compressed_gb");
    REQUIRE(keys(metadata) == std::set<std::string>({
        "captured_at", "observed_at", "reason", "source", "stale", "state"}));
    REQUIRE(metadata.at("state") == "unsupported");
    REQUIRE(metadata.at("stale") == false);
    REQUIRE(metadata.at("captured_at").is_null());
    REQUIRE(std::abs(metadata.at("observed_at").get<double>() -
                     1'704'164'646.678) < 0.0001);
    REQUIRE(metadata.at("source") == "windows.memory.compression");
    REQUIRE(metadata.at("reason") ==
            "The selected Windows source does not expose compressed RAM.");
}

void mapsStaleRamValuesWithoutLosingTheLastGoodSample()
{
    auto snapshot = richSnapshot();
    const auto previous = Domain::makeAvailableTelemetryMetric<std::uint64_t>(
        6ULL * 1024ULL * 1024ULL * 1024ULL,
        utc() - 5s,
        "GlobalMemoryStatusEx");
    snapshot.system.ram.usedBytes =
        Domain::makeStaleTelemetryMetric(
            previous,
            Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
            utc() + 1s,
            "The most recent refresh failed.");

    const auto ram = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system)))
                         .at("ram");
    REQUIRE(ram.at("used_gb") == 6.0);
    const auto& metadata = ram.at("availability").at("used_gb");
    REQUIRE(metadata.at("state") == "temporarily_unavailable");
    REQUIRE(metadata.at("stale") == true);
    REQUIRE(std::abs(metadata.at("captured_at").get<double>() -
                     1'704'164'640.678) < 0.0001);
    REQUIRE(std::abs(metadata.at("observed_at").get<double>() -
                     1'704'164'646.678) < 0.0001);
    REQUIRE(metadata.at("source") == "GlobalMemoryStatusEx");
    REQUIRE(metadata.at("reason") == "The most recent refresh failed.");
}

void exposesHealthAndStandaloneViews()
{
    const Domain::TelemetryHealthReport report{
        true,
        "forge-telemetry",
        "windows-native",
        false,
        "continuous-native",
        "Windows native collectors",
        "WinUI 3 + SSE",
        false};
    const auto healthText = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeHealth(report));
    const auto health = Json::parse(healthText);
    REQUIRE(keys(health) == std::set<std::string>({
        "auth", "collectors", "export_present", "interferes_with_mcp", "mode",
        "node_available", "node_required", "ok", "runtime", "service",
        "static_present", "ui"}));
    REQUIRE(health.at("ok") == true);
    REQUIRE(health.at("service") == "forge-telemetry");
    REQUIRE(health.at("interferes_with_mcp") == false);
    REQUIRE(health.at("node_required") == false);
    REQUIRE(health.at("auth") == true);
    REQUIRE(health.at("export_present").is_null());
    REQUIRE(health.at("static_present").is_null());
    REQUIRE(health.at("node_available").is_null());

    const auto snapshot = richSnapshot();
    const auto system = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system)));
    const auto forge = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeForge(snapshot.forge)));
    REQUIRE(keys(system).size() == 11U);
    REQUIRE(system.at("power").is_object());
    REQUIRE(keys(forge).size() == 18U);
    REQUIRE(forge.at("agent_sessions").size() == 1U);
}

void preservesRouteAliasesAndSseFraming()
{
    const auto snapshot = richSnapshot();
    const auto live = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeLiveFrame(snapshot, 1.5));
    const auto frame = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 1.5));
    const auto compatibility = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSnapshot(snapshot, 1.5));
    REQUIRE(live == frame);
    REQUIRE(frame == compatibility);
    REQUIRE(live == take(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 1.5)));
    REQUIRE(live.starts_with("{\"system\":"));

    const auto withoutRate = Json::parse(take(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot)));
    REQUIRE(withoutRate.at("sample_hz").is_null());

    const auto event = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeServerSentEvent(snapshot, 1.5));
    constexpr std::string_view Prefix{"event: telemetry\ndata: "};
    constexpr std::string_view Suffix{"\n\n"};
    REQUIRE(event.starts_with(Prefix));
    REQUIRE(event.ends_with(Suffix));
    const auto payload = event.substr(
        Prefix.size(), event.size() - Prefix.size() - Suffix.size());
    REQUIRE(payload == live);
    REQUIRE(Json::parse(payload).at("stream") == "realtime");
}

void emitsBoundedMacCompatibleCompactSseFrames()
{
    auto snapshot = richSnapshot();
    const auto processSeed = snapshot.system.processes.front();
    snapshot.system.processes.clear();
    for (std::uint32_t index{}; index < 12U; ++index) {
        auto process = processSeed;
        process.processId = 100U + index;
        process.name = "process-" + std::to_string(index);
        snapshot.system.processes.push_back(std::move(process));
    }

    const auto historySeed = snapshot.history.front();
    snapshot.history.clear();
    for (std::size_t index{}; index < 25U; ++index) {
        auto point = historySeed;
        point.timestamp = utc() + std::chrono::seconds{index};
        point.cpuPercent = static_cast<double>(index);
        point.mcpEvents = index;
        snapshot.history.push_back(std::move(point));
    }

    const auto compact = take(
        Dashboard::DashboardTelemetryJsonCodec::
            encodeCompactServerSentEvent(snapshot, 2.0));
    constexpr std::string_view Prefix{"event: telemetry\ndata: "};
    constexpr std::string_view Suffix{"\n\n"};
    REQUIRE(compact.starts_with(Prefix));
    REQUIRE(compact.ends_with(Suffix));
    const auto payload = compact.substr(
        Prefix.size(), compact.size() - Prefix.size() - Suffix.size());
    const auto document = Json::parse(payload);

    REQUIRE(keys(document) == std::set<std::string>({
        "history", "runtime", "sample_hz", "stream", "system", "updated"}));
    REQUIRE(!document.contains("forge"));
    REQUIRE(keys(document.at("system")) == std::set<std::string>({
        "arch", "cpu", "disk", "disk_io", "gpu", "host", "platform",
        "processes", "ram", "ts"}));
    REQUIRE(!document.at("system").contains("power"));
    REQUIRE(document.at("system").at("processes").size() ==
            Dashboard::DashboardTelemetryJsonCodec::MaximumCompactProcesses);
    REQUIRE(document.at("system").at("processes").front().at("pid") == 100U);
    REQUIRE(document.at("system").at("processes").back().at("pid") == 107U);
    REQUIRE(document.at("history").size() ==
            Dashboard::DashboardTelemetryJsonCodec::MaximumCompactHistoryPoints);
    REQUIRE(document.at("history").front().at("mcp") == 5U);
    REQUIRE(document.at("history").back().at("mcp") == 24U);
    REQUIRE(document.at("stream") == "realtime");
    REQUIRE(document.at("sample_hz") == 2.0);

    const auto full = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeServerSentEvent(
            snapshot, 2.0));
    REQUIRE(compact.size() < full.size());
    const auto fullPayload = full.substr(
        Prefix.size(), full.size() - Prefix.size() - Suffix.size());
    const auto fullDocument = Json::parse(fullPayload);
    REQUIRE(fullDocument.contains("forge"));
    REQUIRE(fullDocument.at("system").contains("power"));
    REQUIRE(fullDocument.at("system").at("processes").size() == 12U);
    REQUIRE(fullDocument.at("history").size() == 25U);

    const auto exact = Dashboard::DashboardTelemetryJsonCodec::
        encodeCompactServerSentEvent(snapshot, 2.0, compact.size());
    REQUIRE(exact);
    REQUIRE(exact.value() == compact);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::
            encodeCompactServerSentEvent(snapshot, 2.0, compact.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::
            encodeCompactServerSentEvent(snapshot, 2.0, 0U),
        Domain::ErrorCodes::InvalidRequest);
}

[[nodiscard]] std::string textOf(
    const Dashboard::DashboardSseFramePair::ImmutableBytes& bytes)
{
    if (bytes == nullptr || bytes->empty()) {
        return {};
    }
    return std::string{
        reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

void createsOneImmutableCompactAndFullFramePair()
{
    auto snapshot = richSnapshot();
    const auto expectedCompact = take(
        Dashboard::DashboardTelemetryJsonCodec::
            encodeCompactServerSentEvent(snapshot, 2.0));
    const auto expectedFull = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeServerSentEvent(
            snapshot, 2.0));
    REQUIRE(expectedCompact.size() < expectedFull.size());

    const auto pair = take(Dashboard::DashboardSseFrameEncoder::encode(
        77U, snapshot, 2.0));
    REQUIRE(pair != nullptr);
    REQUIRE(pair->sourceSequence() == 77U);
    REQUIRE(pair->compactBytes() != nullptr);
    REQUIRE(pair->fullBytes() != nullptr);
    REQUIRE(pair->compactBytes() != pair->fullBytes());
    REQUIRE(textOf(pair->compactBytes()) == expectedCompact);
    REQUIRE(textOf(pair->fullBytes()) == expectedFull);

    snapshot.runtime = "changed-after-encoding";
    REQUIRE(textOf(pair->compactBytes()) == expectedCompact);
    REQUIRE(textOf(pair->fullBytes()) == expectedFull);

    const auto exact = Dashboard::DashboardSseFrameEncoder::encode(
        78U, richSnapshot(), 2.0, expectedFull.size());
    REQUIRE(exact);
    REQUIRE(textOf(exact.value()->fullBytes()) == expectedFull);
    requireError(
        Dashboard::DashboardSseFrameEncoder::encode(
            79U, richSnapshot(), 2.0, expectedFull.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardSseFrameEncoder::encode(
            80U,
            richSnapshot(),
            std::numeric_limits<double>::quiet_NaN()),
        Domain::ErrorCodes::InvalidRequest);
}

void enforcesTheEncodedResponseCeilingDuringSerialization()
{
    const Domain::TelemetryHealthReport report{
        true, "service", "runtime", false, "mode", "collectors", "ui", false};
    const auto canonical = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeHealth(report));
    const auto exact = Dashboard::DashboardTelemetryJsonCodec::encodeHealth(
        report, canonical.size());
    REQUIRE(exact);
    REQUIRE(exact.value() == canonical);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeHealth(
            report, canonical.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    auto snapshot = richSnapshot();
    const auto event = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeServerSentEvent(snapshot, 2.0));
    REQUIRE(Dashboard::DashboardTelemetryJsonCodec::encodeServerSentEvent(
        snapshot, 2.0, event.size()));
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeServerSentEvent(
            snapshot, 2.0, event.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    auto large = report;
    large.ui.assign(
        Dashboard::DashboardTelemetryJsonCodec::DefaultMaximumEncodedBytes,
        'x');
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeHealth(large),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeHealth(report, 0U),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeHealth(
            report,
            Dashboard::DashboardTelemetryJsonCodec::DefaultMaximumEncodedBytes + 1U),
        Domain::ErrorCodes::InvalidRequest);
}

void rejectsNonfiniteValuesInvalidTextAndNoncanonicalTimestamps()
{
    auto snapshot = richSnapshot();
    snapshot.system.cpu.percent = std::numeric_limits<double>::quiet_NaN();
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 2.0),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.system.gpus.front().utilizationPercent =
        std::numeric_limits<double>::infinity();
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.history.front().diskBytesPerSecond = -1.0;
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSnapshot(snapshot, 2.0),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.system.host = std::string{"embedded\0nul", 12U};
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.system.host = std::string{"\xC0\xAF", 2U};
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.system.ram.percent.value.reset();
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.system.ram.totalBytes.source = std::string{"source\0bad", 10U};
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.system.ram.availableBytes.capturedAt = utc(-1);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.updatedAt = utc(-1);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 2.0),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.forge.agentSessions.front().createdAt =
        Domain::UtcTimePoint{
            std::chrono::milliseconds{253'402'300'800'000LL}};
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeForge(snapshot.forge),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 0.0),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(
            snapshot, std::numeric_limits<double>::infinity()),
        Domain::ErrorCodes::InvalidRequest);

    snapshot = richSnapshot();
    snapshot.forge.orchestrationHealth =
        static_cast<Domain::TelemetryHealth>(255);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeForge(snapshot.forge),
        Domain::ErrorCodes::InvalidRequest);
}

void escapesValidTextDeterministically()
{
    auto snapshot = richSnapshot();
    snapshot.system.host = "host\"\\\n\t\xE2\x98\x83";
    const auto first = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system));
    const auto second = take(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system));
    REQUIRE(first == second);
    REQUIRE(first.find("host\\\"\\\\\\n\\t") != std::string::npos);
    REQUIRE(Json::parse(first).at("host") == snapshot.system.host);
}

void enforcesEveryCollectionBoundBeforeSerialization()
{
    auto snapshot = richSnapshot();
    snapshot.system.cpu.perLogicalProcessor.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumLogicalProcessorEntries,
        1.0);
    REQUIRE(Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system));
    snapshot.system.cpu.perLogicalProcessor.push_back(1.0);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    snapshot.system.cpu.perCoreFrequencyMhz.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumLogicalProcessorEntries + 1U,
        1U);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    const auto diskSeed = snapshot.system.disks.front();
    snapshot.system.disks.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumDiskVolumes + 1U,
        diskSeed);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    const auto gpuSeed = snapshot.system.gpus.front();
    snapshot.system.gpus.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumGpuDevices + 1U,
        gpuSeed);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    const auto processSeed = snapshot.system.processes.front();
    snapshot.system.processes.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumProcesses + 1U,
        processSeed);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeSystem(snapshot.system),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    const auto toolSeed = snapshot.forge.tools.front();
    snapshot.forge.tools.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumTools + 1U,
        toolSeed);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeForge(snapshot.forge),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    const auto sessionSeed = snapshot.forge.agentSessions.front();
    snapshot.forge.agentSessions.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumAgentSessions + 1U,
        sessionSeed);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeForge(snapshot.forge),
        Domain::ErrorCodes::LimitExceeded);

    snapshot = richSnapshot();
    const auto historySeed = snapshot.history.front();
    snapshot.history.assign(
        Dashboard::DashboardTelemetryJsonCodec::MaximumHistoryPoints,
        historySeed);
    REQUIRE(Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 2.0));
    snapshot.history.push_back(historySeed);
    requireError(
        Dashboard::DashboardTelemetryJsonCodec::encodeFrame(snapshot, 2.0),
        Domain::ErrorCodes::LimitExceeded);
}

} // namespace

int main()
{
    try {
        mapsTheCompleteMacCompatibleFrameContract();
        mapsUnsupportedRamMetricsToNullWithExplicitMetadata();
        mapsStaleRamValuesWithoutLosingTheLastGoodSample();
        exposesHealthAndStandaloneViews();
        preservesRouteAliasesAndSseFraming();
        emitsBoundedMacCompatibleCompactSseFrames();
        createsOneImmutableCompactAndFullFramePair();
        enforcesTheEncodedResponseCeilingDuringSerialization();
        rejectsNonfiniteValuesInvalidTextAndNoncanonicalTimestamps();
        escapesValidTextDeterministically();
        enforcesEveryCollectionBoundBeforeSerialization();
        std::cout << "Dashboard telemetry JSON codec tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard telemetry JSON codec tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard telemetry JSON codec tests failed with an unknown error.\n";
        return 1;
    }
}
