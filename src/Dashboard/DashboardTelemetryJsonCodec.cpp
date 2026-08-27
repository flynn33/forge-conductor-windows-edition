#include "ForgeConductor/Dashboard/DashboardTelemetryJsonCodec.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

constexpr double BytesPerGibibyte = 1'073'741'824.0;
constexpr double BytesPerMebibyte = 1'048'576.0;
constexpr std::int64_t FirstUnsupportedUtcMillisecond =
    253'402'300'800'000LL; // 10000-01-01T00:00:00.000Z

class TelemetryCodecException final : public std::runtime_error {
public:
    TelemetryCodecException(std::string code, std::string message)
        : std::runtime_error{std::move(message)}, code_{std::move(code)}
    {
    }

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

[[noreturn]] void reject(
    const std::string_view code,
    const std::string_view message)
{
    throw TelemetryCodecException{std::string{code}, std::string{message}};
}

void validateMaximumEncodedBytes(const std::size_t maximumEncodedBytes)
{
    if (maximumEncodedBytes == 0U ||
        maximumEncodedBytes >
            DashboardTelemetryJsonCodec::DefaultMaximumEncodedBytes) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry maximum encoded bytes must be between 1 and 2097152.");
    }
}

class BoundedJsonWriter final {
public:
    explicit BoundedJsonWriter(const std::size_t maximumBytes)
        : maximumBytes_{maximumBytes}
    {
        output_.reserve((std::min)(maximumBytes_, static_cast<std::size_t>(16U * 1024U)));
    }

    void raw(const std::string_view value)
    {
        if (value.size() > maximumBytes_ - output_.size()) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard telemetry JSON exceeds the configured encoded-response limit.");
        }
        output_.append(value);
    }

    void character(const char value)
    {
        if (output_.size() == maximumBytes_) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard telemetry JSON exceeds the configured encoded-response limit.");
        }
        output_.push_back(value);
    }

    void nullValue() { raw("null"); }
    void boolean(const bool value) { raw(value ? "true" : "false"); }

    template <typename Integer>
    void integer(const Integer value)
    {
        std::array<char, 32U> buffer{};
        const auto encoded = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), value);
        if (encoded.ec != std::errc{}) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard telemetry integer formatting failed.");
        }
        raw(std::string_view{
            buffer.data(), static_cast<std::size_t>(encoded.ptr - buffer.data())});
    }

    void number(const double value)
    {
        if (!std::isfinite(value)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard telemetry numeric values must be finite.");
        }
        if (value == 0.0) {
            raw("0");
            return;
        }
        std::array<char, 64U> buffer{};
        const auto encoded = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            value,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (encoded.ec != std::errc{}) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard telemetry floating-point formatting failed.");
        }
        raw(std::string_view{
            buffer.data(), static_cast<std::size_t>(encoded.ptr - buffer.data())});
    }

    void string(const std::string_view value)
    {
        if (value.size() > maximumBytes_) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "A dashboard telemetry string exceeds the encoded-response limit.");
        }
        if (value.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(value)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard telemetry text contains an embedded NUL or invalid UTF-8.");
        }

        constexpr char Hex[] = "0123456789abcdef";
        character('"');
        for (const unsigned char valueByte : value) {
            switch (valueByte) {
            case '"': raw("\\\""); break;
            case '\\': raw("\\\\"); break;
            case '\b': raw("\\b"); break;
            case '\f': raw("\\f"); break;
            case '\n': raw("\\n"); break;
            case '\r': raw("\\r"); break;
            case '\t': raw("\\t"); break;
            default:
                if (valueByte < 0x20U) {
                    raw("\\u00");
                    character(Hex[(valueByte >> 4U) & 0x0fU]);
                    character(Hex[valueByte & 0x0fU]);
                } else {
                    character(static_cast<char>(valueByte));
                }
                break;
            }
        }
        character('"');
    }

    void member(bool& first, const std::string_view name)
    {
        if (!first) {
            character(',');
        }
        first = false;
        string(name);
        character(':');
    }

    [[nodiscard]] std::string finish() && { return std::move(output_); }

private:
    std::size_t maximumBytes_;
    std::string output_;
};

class TextBudget final {
public:
    explicit TextBudget(const std::size_t maximumBytes)
        : maximumBytes_{maximumBytes}
    {
    }

    void add(const std::string_view value)
    {
        if (value.size() > maximumBytes_ - bytes_) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard telemetry source text cannot fit the encoded-response limit.");
        }
        bytes_ += value.size();
        if (value.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(value)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard telemetry text contains an embedded NUL or invalid UTF-8.");
        }
    }

    void addOptional(const std::optional<std::string>& value)
    {
        if (value) {
            add(*value);
        }
    }

private:
    std::size_t maximumBytes_;
    std::size_t bytes_{};
};

void requireCollectionBound(
    const std::size_t actual,
    const std::size_t maximum,
    const std::string_view collection)
{
    if (actual > maximum) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            std::string{"Dashboard telemetry "} + std::string{collection} +
                " exceeds its collection limit.");
    }
}

void requireFinite(const double value, const std::string_view field)
{
    if (!std::isfinite(value)) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{"Dashboard telemetry "} + std::string{field} +
                " must be finite.");
    }
}

void requireNonnegative(const double value, const std::string_view field)
{
    requireFinite(value, field);
    if (value < 0.0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{"Dashboard telemetry "} + std::string{field} +
                " must not be negative.");
    }
}

void requirePercent(const double value, const std::string_view field)
{
    requireFinite(value, field);
    if (value < 0.0 || value > 100.0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{"Dashboard telemetry "} + std::string{field} +
                " must be within 0 through 100.");
    }
}

void requireOptionalPercent(
    const std::optional<double>& value,
    const std::string_view field)
{
    if (value) {
        requirePercent(*value, field);
    }
}

[[nodiscard]] std::int64_t utcMilliseconds(
    const Domain::UtcTimePoint timestamp)
{
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(
        timestamp.time_since_epoch());
    const auto value = milliseconds.count();
    if (value < 0 || value >= FirstUnsupportedUtcMillisecond) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry timestamps must be canonical UTC instants from 1970 through 9999.");
    }
    return value;
}

[[nodiscard]] double unixUtcSeconds(const Domain::UtcTimePoint timestamp)
{
    return static_cast<double>(utcMilliseconds(timestamp)) / 1'000.0;
}

[[nodiscard]] std::string iso8601Utc(const Domain::UtcTimePoint timestamp)
{
    using namespace std::chrono;
    const milliseconds sinceEpoch{utcMilliseconds(timestamp)};
    const auto day = floor<days>(sinceEpoch);
    const year_month_day calendar{sys_days{day}};
    const hh_mm_ss time{sinceEpoch - day};
    if (!calendar.ok()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry timestamp is not a valid UTC calendar instant.");
    }

    std::array<char, 25U> buffer{};
    const auto written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<int>(calendar.year()),
        static_cast<unsigned>(calendar.month()),
        static_cast<unsigned>(calendar.day()),
        static_cast<long long>(time.hours().count()),
        static_cast<long long>(time.minutes().count()),
        static_cast<long long>(time.seconds().count()),
        static_cast<long long>(time.subseconds().count()));
    if (written != 24) {
        reject(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard telemetry UTC timestamp formatting failed.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] std::string_view healthName(const Domain::TelemetryHealth health)
{
    switch (health) {
    case Domain::TelemetryHealth::Ok: return "ok";
    case Domain::TelemetryHealth::Warn: return "warn";
    case Domain::TelemetryHealth::Error: return "error";
    case Domain::TelemetryHealth::Down: return "down";
    case Domain::TelemetryHealth::Config: return "config";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard telemetry contains an unknown health state.");
}

[[nodiscard]] std::string_view healthLabel(const Domain::TelemetryHealth health)
{
    switch (health) {
    case Domain::TelemetryHealth::Ok: return "READY";
    case Domain::TelemetryHealth::Warn: return "WARN";
    case Domain::TelemetryHealth::Error: return "ERROR";
    case Domain::TelemetryHealth::Down: return "DOWN";
    case Domain::TelemetryHealth::Config: return "CONFIG";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard telemetry contains an unknown health state.");
}

[[nodiscard]] std::string_view toolEffectName(const Domain::ToolEffect effect)
{
    switch (effect) {
    case Domain::ToolEffect::Read: return "read";
    case Domain::ToolEffect::Write: return "write";
    case Domain::ToolEffect::Execute: return "execute";
    case Domain::ToolEffect::Destructive: return "destructive";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard telemetry contains an unknown tool effect.");
}

[[nodiscard]] std::string_view toolAvailabilityName(
    const Domain::ToolAvailability availability)
{
    switch (availability) {
    case Domain::ToolAvailability::Available: return "available";
    case Domain::ToolAvailability::Disabled: return "disabled";
    case Domain::ToolAvailability::MissingDependency: return "missing_dependency";
    case Domain::ToolAvailability::Unhealthy: return "unhealthy";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard telemetry contains an unknown tool availability state.");
}

[[nodiscard]] Domain::TelemetryHealth toolHealth(
    const Domain::ToolAvailability availability)
{
    switch (availability) {
    case Domain::ToolAvailability::Available: return Domain::TelemetryHealth::Ok;
    case Domain::ToolAvailability::Disabled: return Domain::TelemetryHealth::Config;
    case Domain::ToolAvailability::MissingDependency: return Domain::TelemetryHealth::Down;
    case Domain::ToolAvailability::Unhealthy: return Domain::TelemetryHealth::Error;
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard telemetry contains an unknown tool availability state.");
}

[[nodiscard]] std::string_view sessionStatusName(
    const Domain::SessionStatus status)
{
    switch (status) {
    case Domain::SessionStatus::Open: return "open";
    case Domain::SessionStatus::Active: return "active";
    case Domain::SessionStatus::Running: return "running";
    case Domain::SessionStatus::Started: return "started";
    case Domain::SessionStatus::Closed: return "closed";
    case Domain::SessionStatus::Completed: return "completed";
    case Domain::SessionStatus::Failed: return "failed";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard telemetry contains an unknown agent-session state.");
}

void validateSampleHz(const std::optional<double> measuredSampleHz)
{
    if (!measuredSampleHz) {
        return;
    }
    requireFinite(*measuredSampleHz, "sample_hz");
    if (*measuredSampleHz <= 0.0 || *measuredSampleHz > 60.0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry sample_hz must be greater than zero and no more than 60.");
    }
}

void validateSystem(
    const Domain::SystemMetrics& system,
    const std::size_t maximumEncodedBytes)
{
    static_cast<void>(utcMilliseconds(system.timestamp));
    requireCollectionBound(
        system.cpu.perLogicalProcessor.size(),
        DashboardTelemetryJsonCodec::MaximumLogicalProcessorEntries,
        "per-logical-processor samples");
    requireCollectionBound(
        system.cpu.perCoreFrequencyMhz.size(),
        DashboardTelemetryJsonCodec::MaximumLogicalProcessorEntries,
        "per-core frequencies");
    requireCollectionBound(
        system.disks.size(),
        DashboardTelemetryJsonCodec::MaximumDiskVolumes,
        "disk volumes");
    requireCollectionBound(
        system.gpus.size(),
        DashboardTelemetryJsonCodec::MaximumGpuDevices,
        "GPU devices");
    requireCollectionBound(
        system.processes.size(),
        DashboardTelemetryJsonCodec::MaximumProcesses,
        "process rows");
    if (system.cpu.logicalProcessorCount >
            DashboardTelemetryJsonCodec::MaximumLogicalProcessorEntries ||
        system.cpu.physicalCoreCount >
            DashboardTelemetryJsonCodec::MaximumLogicalProcessorEntries) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            "Dashboard telemetry processor counts exceed the supported bound.");
    }

    requirePercent(system.cpu.percent, "cpu.percent");
    requirePercent(system.cpu.userPercent, "cpu.user");
    requirePercent(system.cpu.systemPercent, "cpu.system");
    requirePercent(system.cpu.idlePercent, "cpu.idle");
    for (const auto value : system.cpu.perLogicalProcessor) {
        requirePercent(value, "cpu.per_cpu");
    }
    if (system.cpu.frequencyMhz && *system.cpu.frequencyMhz == 0U) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry cpu.freq_mhz must be positive when available.");
    }
    for (const auto value : system.cpu.perCoreFrequencyMhz) {
        if (value == 0U) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard telemetry cpu.freq_per_core_mhz values must be positive.");
        }
    }
    requireNonnegative(system.cpu.loadAverage.oneMinute, "cpu.load_avg.m1");
    requireNonnegative(system.cpu.loadAverage.fiveMinutes, "cpu.load_avg.m5");
    requireNonnegative(system.cpu.loadAverage.fifteenMinutes, "cpu.load_avg.m15");
    requirePercent(system.ram.percent, "ram.percent");
    requirePercent(system.ram.pressurePercent, "ram.pressure_percent");

    for (const auto& disk : system.disks) {
        requirePercent(disk.percent, "disk.percent");
    }
    requireNonnegative(system.diskIo.readBytesPerSecond, "disk_io.read_bytes_per_second");
    requireNonnegative(system.diskIo.writeBytesPerSecond, "disk_io.write_bytes_per_second");
    requireNonnegative(
        system.diskIo.readOperationsPerSecond,
        "disk_io.read_operations_per_second");
    requireNonnegative(
        system.diskIo.writeOperationsPerSecond,
        "disk_io.write_operations_per_second");

    for (const auto& gpu : system.gpus) {
        requireOptionalPercent(gpu.utilizationPercent, "gpu.util_gpu");
    }
    for (const auto& process : system.processes) {
        requireNonnegative(process.cpuPercent, "processes.cpu_percent");
    }
    requireOptionalPercent(system.power.batteryPercent, "power.battery_percent");
    if (system.power.timeRemaining && system.power.timeRemaining->count() < 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard telemetry power.time_remaining must not be negative.");
    }

    TextBudget text{maximumEncodedBytes};
    text.add(system.host);
    text.add(system.platform);
    text.add(system.architecture);
    text.add(system.cpu.brand);
    for (const auto& disk : system.disks) {
        text.add(disk.device);
        text.add(disk.mount.value());
        text.add(disk.fileSystem);
    }
    for (const auto& gpu : system.gpus) {
        text.add(gpu.vendor);
        text.add(gpu.name);
    }
    for (const auto& process : system.processes) {
        text.add(process.name);
        text.add(process.source);
    }
    text.add(system.power.state);
}

void validateForge(
    const Domain::ForgeSnapshot& forge,
    const std::size_t maximumEncodedBytes)
{
    static_cast<void>(utcMilliseconds(forge.timestamp));
    requireCollectionBound(
        forge.tools.size(),
        DashboardTelemetryJsonCodec::MaximumTools,
        "MCP tool rows");
    requireCollectionBound(
        forge.agentSessions.size(),
        DashboardTelemetryJsonCodec::MaximumAgentSessions,
        "agent-session rows");
    static_cast<void>(healthName(forge.orchestrationHealth));

    TextBudget text{maximumEncodedBytes};
    text.add(forge.home.value());
    text.add(forge.runtime);
    for (const auto& tool : forge.tools) {
        const auto valid = Domain::validateToolDescriptor(tool);
        if (!valid) {
            throw TelemetryCodecException{
                valid.error().code,
                "Dashboard telemetry contains an invalid tool descriptor: " +
                    valid.error().message};
        }
        text.add(tool.name);
        text.add(tool.description);
        text.add(tool.pack);
    }
    for (const auto& session : forge.agentSessions) {
        static_cast<void>(sessionStatusName(session.status));
        static_cast<void>(utcMilliseconds(session.createdAt));
        static_cast<void>(utcMilliseconds(session.updatedAt));
        text.add(session.id.value());
        text.add(session.agentId.value());
        if (session.clientId) {
            text.add(session.clientId->value());
        }
        text.addOptional(session.summary);
    }
}

void validateSnapshot(
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz,
    const std::size_t maximumEncodedBytes)
{
    validateSystem(snapshot.system, maximumEncodedBytes);
    validateForge(snapshot.forge, maximumEncodedBytes);
    static_cast<void>(utcMilliseconds(snapshot.updatedAt));
    requireCollectionBound(
        snapshot.history.size(),
        DashboardTelemetryJsonCodec::MaximumHistoryPoints,
        "history points");
    validateSampleHz(measuredSampleHz);
    for (const auto& point : snapshot.history) {
        static_cast<void>(utcMilliseconds(point.timestamp));
        requirePercent(point.cpuPercent, "history.cpu");
        requirePercent(point.ramPercent, "history.ram");
        requireOptionalPercent(point.gpuPercent, "history.gpu");
        requireNonnegative(point.diskBytesPerSecond, "history.disk_io");
        static_cast<void>(healthName(point.orchestrationHealth));
    }
    TextBudget text{maximumEncodedBytes};
    text.add(snapshot.runtime);
}

void writeOptionalNumber(
    BoundedJsonWriter& writer,
    const std::optional<double>& value)
{
    if (value) {
        writer.number(*value);
    } else {
        writer.nullValue();
    }
}

template <typename Integer>
void writeOptionalInteger(
    BoundedJsonWriter& writer,
    const std::optional<Integer>& value)
{
    if (value) {
        writer.integer(*value);
    } else {
        writer.nullValue();
    }
}

void writeCpu(BoundedJsonWriter& writer, const Domain::CpuMetrics& cpu)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "percent");
    writer.number(cpu.percent);
    writer.member(first, "per_cpu");
    writer.character('[');
    for (std::size_t index{}; index < cpu.perLogicalProcessor.size(); ++index) {
        if (index != 0U) writer.character(',');
        writer.number(cpu.perLogicalProcessor[index]);
    }
    writer.character(']');
    writer.member(first, "count_logical");
    writer.integer(cpu.logicalProcessorCount);
    writer.member(first, "count_physical");
    writer.integer(cpu.physicalCoreCount);
    writer.member(first, "freq_mhz");
    writeOptionalInteger(writer, cpu.frequencyMhz);
    writer.member(first, "freq_per_core_mhz");
    if (cpu.perCoreFrequencyMhz.empty()) {
        writer.nullValue();
    } else {
        writer.character('[');
        for (std::size_t index{}; index < cpu.perCoreFrequencyMhz.size(); ++index) {
            if (index != 0U) writer.character(',');
            writer.integer(cpu.perCoreFrequencyMhz[index]);
        }
        writer.character(']');
    }
    writer.member(first, "load_avg");
    writer.character('{');
    bool loadFirst{true};
    writer.member(loadFirst, "m1");
    writer.number(cpu.loadAverage.oneMinute);
    writer.member(loadFirst, "m5");
    writer.number(cpu.loadAverage.fiveMinutes);
    writer.member(loadFirst, "m15");
    writer.number(cpu.loadAverage.fifteenMinutes);
    writer.character('}');
    writer.member(first, "brand");
    writer.string(cpu.brand);
    writer.member(first, "user");
    writer.number(cpu.userPercent);
    writer.member(first, "system");
    writer.number(cpu.systemPercent);
    writer.member(first, "idle");
    writer.number(cpu.idlePercent);
    writer.character('}');
}

void writeRam(BoundedJsonWriter& writer, const Domain::RamMetrics& ram)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "total_gb");
    writer.number(static_cast<double>(ram.totalBytes) / BytesPerGibibyte);
    writer.member(first, "used_gb");
    writer.number(static_cast<double>(ram.usedBytes) / BytesPerGibibyte);
    writer.member(first, "available_gb");
    writer.number(static_cast<double>(ram.availableBytes) / BytesPerGibibyte);
    writer.member(first, "percent");
    writer.number(ram.percent);
    writer.member(first, "pressure_percent");
    writer.number(ram.pressurePercent);
    writer.member(first, "active_gb");
    writer.nullValue();
    writer.member(first, "wired_gb");
    writer.nullValue();
    writer.member(first, "compressed_gb");
    writer.number(static_cast<double>(ram.compressedBytes) / BytesPerGibibyte);
    writer.member(first, "swap_total_gb");
    writer.nullValue();
    writer.member(first, "swap_used_gb");
    writer.nullValue();
    writer.member(first, "swap_percent");
    writer.nullValue();
    writer.member(first, "committed_gb");
    writer.number(static_cast<double>(ram.committedBytes) / BytesPerGibibyte);
    writer.member(first, "paged_pool_gb");
    writer.number(static_cast<double>(ram.pagedPoolBytes) / BytesPerGibibyte);
    writer.character('}');
}

void writeDiskVolume(BoundedJsonWriter& writer, const Domain::DiskVolume& disk)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "device");
    writer.string(disk.device);
    writer.member(first, "mount");
    writer.string(disk.mount.value());
    writer.member(first, "fstype");
    writer.string(disk.fileSystem);
    writer.member(first, "total_gb");
    writer.number(static_cast<double>(disk.totalBytes) / BytesPerGibibyte);
    writer.member(first, "used_gb");
    writer.number(static_cast<double>(disk.usedBytes) / BytesPerGibibyte);
    writer.member(first, "available_gb");
    writer.number(static_cast<double>(disk.availableBytes) / BytesPerGibibyte);
    writer.member(first, "percent");
    writer.number(disk.percent);
    writer.character('}');
}

void writeDiskIo(BoundedJsonWriter& writer, const Domain::DiskIoMetrics& diskIo)
{
    const auto readMib = diskIo.readBytesPerSecond / BytesPerMebibyte;
    const auto writeMib = diskIo.writeBytesPerSecond / BytesPerMebibyte;
    writer.character('{');
    bool first{true};
    writer.member(first, "read_mb_s");
    writer.number(readMib);
    writer.member(first, "write_mb_s");
    writer.number(writeMib);
    writer.member(first, "total_mb_s");
    writer.number(readMib + writeMib);
    writer.member(first, "read_iops");
    writer.number(diskIo.readOperationsPerSecond);
    writer.member(first, "write_iops");
    writer.number(diskIo.writeOperationsPerSecond);
    writer.member(first, "total_iops");
    writer.number(
        diskIo.readOperationsPerSecond + diskIo.writeOperationsPerSecond);
    writer.member(first, "read_bytes_total");
    writer.nullValue();
    writer.member(first, "write_bytes_total");
    writer.nullValue();
    writer.member(first, "read_ops_total");
    writer.nullValue();
    writer.member(first, "write_ops_total");
    writer.nullValue();
    writer.character('}');
}

void writeGpu(BoundedJsonWriter& writer, const Domain::GpuMetrics& gpu)
{
    const auto toMebibytes = [](const std::optional<std::uint64_t> value)
        -> std::optional<double> {
        return value
            ? std::optional<double>{static_cast<double>(*value) / BytesPerMebibyte}
            : std::nullopt;
    };
    const auto dedicatedUsed = toMebibytes(gpu.dedicatedBytesUsed);
    const auto dedicatedTotal = toMebibytes(gpu.dedicatedBytesTotal);
    std::optional<double> dedicatedFree;
    if (dedicatedUsed && dedicatedTotal && *dedicatedUsed <= *dedicatedTotal) {
        dedicatedFree = *dedicatedTotal - *dedicatedUsed;
    }

    writer.character('{');
    bool first{true};
    writer.member(first, "vendor");
    writer.string(gpu.vendor);
    writer.member(first, "name");
    writer.string(gpu.name);
    writer.member(first, "util_gpu");
    writeOptionalNumber(writer, gpu.utilizationPercent);
    writer.member(first, "util_mem");
    writer.nullValue();
    writer.member(first, "util_renderer");
    writer.nullValue();
    writer.member(first, "util_tiler");
    writer.nullValue();
    writer.member(first, "mem_used_mib");
    writeOptionalNumber(writer, dedicatedUsed);
    writer.member(first, "mem_total_mib");
    writeOptionalNumber(writer, dedicatedTotal);
    writer.member(first, "mem_alloc_mib");
    writeOptionalNumber(writer, dedicatedUsed);
    writer.member(first, "mem_free_mib");
    writeOptionalNumber(writer, dedicatedFree);
    writer.member(first, "temp_c");
    writer.nullValue();
    writer.member(first, "power_w");
    writer.nullValue();
    writer.member(first, "power_limit_w");
    writer.nullValue();
    writer.member(first, "clock_sm_mhz");
    writer.nullValue();
    writer.member(first, "clock_sm_max_mhz");
    writer.nullValue();
    writer.member(first, "clock_mem_mhz");
    writer.nullValue();
    writer.member(first, "cores");
    writer.nullValue();
    writer.member(first, "shared_memory");
    writer.nullValue();
    writer.member(first, "metal");
    writer.nullValue();
    writer.member(first, "processes");
    writer.nullValue();
    writer.member(first, "shared_mem_used_mib");
    writeOptionalNumber(writer, toMebibytes(gpu.sharedBytesUsed));
    writer.member(first, "direct3d_available");
    writer.boolean(gpu.direct3dAvailable);
    writer.character('}');
}

void writeProcess(BoundedJsonWriter& writer, const Domain::ProcessMetrics& process)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "pid");
    writer.integer(process.processId);
    writer.member(first, "name");
    writer.string(process.name);
    writer.member(first, "cpu_percent");
    writer.number(process.cpuPercent);
    writer.member(first, "rss_gb");
    writer.number(static_cast<double>(process.workingSetBytes) / BytesPerGibibyte);
    writer.member(first, "footprint_gb");
    writer.nullValue();
    writer.member(first, "thread_count");
    writer.integer(process.threadCount);
    writer.member(first, "source");
    if (process.source.empty()) {
        writer.nullValue();
    } else {
        writer.string(process.source);
    }
    writer.member(first, "private_gb");
    writer.number(static_cast<double>(process.privateBytes) / BytesPerGibibyte);
    writer.member(first, "handle_count");
    writer.integer(process.handleCount);
    writer.character('}');
}

void writePower(BoundedJsonWriter& writer, const Domain::PowerMetrics& power)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "on_ac");
    writer.boolean(power.onAcPower);
    writer.member(first, "state");
    writer.string(power.state);
    writer.member(first, "battery_percent");
    writeOptionalNumber(writer, power.batteryPercent);
    writer.member(first, "is_charging");
    if (power.charging) {
        writer.boolean(*power.charging);
    } else {
        writer.nullValue();
    }
    writer.member(first, "is_charged");
    writer.nullValue();
    writer.member(first, "time_to_empty_min");
    if (power.timeRemaining && power.charging && !*power.charging) {
        writer.integer(power.timeRemaining->count());
    } else {
        writer.nullValue();
    }
    writer.member(first, "time_to_full_min");
    if (power.timeRemaining && power.charging && *power.charging) {
        writer.integer(power.timeRemaining->count());
    } else {
        writer.nullValue();
    }
    writer.member(first, "source_count");
    writer.nullValue();
    writer.member(first, "providing");
    writer.nullValue();
    writer.character('}');
}

void writeSystem(BoundedJsonWriter& writer, const Domain::SystemMetrics& system)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "ts");
    writer.number(unixUtcSeconds(system.timestamp));
    writer.member(first, "host");
    writer.string(system.host);
    writer.member(first, "platform");
    writer.string(system.platform);
    writer.member(first, "arch");
    writer.string(system.architecture);
    writer.member(first, "cpu");
    writeCpu(writer, system.cpu);
    writer.member(first, "ram");
    writeRam(writer, system.ram);
    writer.member(first, "disk");
    writer.character('[');
    for (std::size_t index{}; index < system.disks.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeDiskVolume(writer, system.disks[index]);
    }
    writer.character(']');
    writer.member(first, "disk_io");
    writeDiskIo(writer, system.diskIo);
    writer.member(first, "gpu");
    writer.character('[');
    for (std::size_t index{}; index < system.gpus.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeGpu(writer, system.gpus[index]);
    }
    writer.character(']');
    writer.member(first, "processes");
    writer.character('[');
    for (std::size_t index{}; index < system.processes.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeProcess(writer, system.processes[index]);
    }
    writer.character(']');
    writer.member(first, "power");
    writePower(writer, system.power);
    writer.character('}');
}

void writeTool(BoundedJsonWriter& writer, const Domain::ToolDescriptor& tool)
{
    const auto health = toolHealth(tool.availability);
    writer.character('{');
    bool first{true};
    writer.member(first, "name");
    writer.string(tool.name);
    writer.member(first, "pack");
    writer.string(tool.pack);
    writer.member(first, "status");
    writer.string(toolAvailabilityName(tool.availability));
    writer.member(first, "health");
    writer.string(healthName(health));
    writer.member(first, "health_label");
    writer.string(healthLabel(health));
    writer.member(first, "activity");
    writer.nullValue();
    writer.member(first, "live");
    writer.nullValue();
    writer.member(first, "usage_1h");
    writer.nullValue();
    writer.member(first, "usage_5m");
    writer.nullValue();
    writer.member(first, "description");
    writer.string(tool.description);
    writer.member(first, "effect");
    writer.string(toolEffectName(tool.effect));
    writer.member(first, "requires_project");
    writer.boolean(tool.requiresProject);
    writer.member(first, "requires_shell");
    writer.boolean(tool.requiresShell);
    writer.character('}');
}

void writeAgentSession(
    BoundedJsonWriter& writer,
    const Domain::AgentSession& session)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "id");
    writer.string(session.id.value());
    writer.member(first, "agent_id");
    writer.string(session.agentId.value());
    writer.member(first, "client_id");
    if (session.clientId) {
        writer.string(session.clientId->value());
    } else {
        writer.nullValue();
    }
    writer.member(first, "status");
    writer.string(sessionStatusName(session.status));
    writer.member(first, "summary");
    if (session.summary) {
        writer.string(*session.summary);
    } else {
        writer.nullValue();
    }
    writer.member(first, "created_at");
    writer.string(iso8601Utc(session.createdAt));
    writer.member(first, "updated_at");
    writer.string(iso8601Utc(session.updatedAt));
    writer.character('}');
}

void writeAgentsSummary(
    BoundedJsonWriter& writer,
    const std::vector<Domain::AgentSession>& sessions)
{
    std::size_t open{};
    std::map<std::string_view, std::size_t> byStatus;
    for (const auto& session : sessions) {
        if (Domain::isOpen(session.status)) {
            ++open;
        }
        ++byStatus[sessionStatusName(session.status)];
    }

    writer.character('{');
    bool first{true};
    writer.member(first, "total");
    writer.integer(sessions.size());
    writer.member(first, "open");
    writer.integer(open);
    writer.member(first, "by_status");
    writer.character('{');
    bool statusFirst{true};
    for (const auto& [status, count] : byStatus) {
        writer.member(statusFirst, status);
        writer.integer(count);
    }
    writer.character('}');
    writer.character('}');
}

void writeOrchestration(
    BoundedJsonWriter& writer,
    const Domain::ForgeSnapshot& forge)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "home");
    writer.string(forge.home.value());
    writer.member(first, "mode");
    writer.nullValue();
    writer.member(first, "health");
    writer.string(healthName(forge.orchestrationHealth));
    writer.member(first, "health_label");
    writer.string(healthLabel(forge.orchestrationHealth));
    writer.member(first, "manager_alive");
    writer.nullValue();
    writer.member(first, "manager_pid");
    writer.nullValue();
    writer.member(first, "manager_state");
    writer.nullValue();
    writer.member(first, "primary_alive");
    writer.nullValue();
    writer.member(first, "fallback_alive");
    writer.nullValue();
    writer.member(first, "watchdog_alive");
    writer.nullValue();
    writer.member(first, "orchestrator_alive");
    writer.nullValue();
    writer.member(first, "serve_count");
    writer.nullValue();
    writer.member(first, "supervise_count");
    writer.nullValue();
    writer.member(first, "mcp_external_count");
    writer.nullValue();
    writer.member(first, "heartbeat_age_sec");
    writer.nullValue();
    writer.member(first, "heartbeat");
    writer.nullValue();
    writer.member(first, "failover_events_1h");
    writer.nullValue();
    writer.member(first, "supervisor_tail");
    writer.nullValue();
    writer.member(first, "orchestrator_tail");
    writer.nullValue();
    writer.member(first, "role_events");
    writer.nullValue();
    writer.character('}');
}

void writeForge(BoundedJsonWriter& writer, const Domain::ForgeSnapshot& forge)
{
    std::map<std::string_view, std::pair<std::size_t, std::size_t>> packs;
    for (const auto& tool : forge.tools) {
        auto& counts = packs[tool.pack];
        ++counts.first;
        if (tool.availability == Domain::ToolAvailability::Available) {
            ++counts.second;
        }
    }

    writer.character('{');
    bool first{true};
    writer.member(first, "ts");
    writer.number(unixUtcSeconds(forge.timestamp));
    writer.member(first, "home");
    writer.string(forge.home.value());
    writer.member(first, "runtime");
    writer.string(forge.runtime);
    writer.member(first, "presence_count");
    writer.integer(forge.presenceCount);
    writer.member(first, "presence");
    writer.nullValue();
    writer.member(first, "mcp_servers");
    writer.nullValue();
    writer.member(first, "mcp_tools");
    writer.character('[');
    for (std::size_t index{}; index < forge.tools.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeTool(writer, forge.tools[index]);
    }
    writer.character(']');
    writer.member(first, "mcp_packs");
    writer.character('[');
    bool packFirst{true};
    for (const auto& [pack, counts] : packs) {
        if (!packFirst) writer.character(',');
        packFirst = false;
        writer.character('{');
        bool packMemberFirst{true};
        writer.member(packMemberFirst, "pack");
        writer.string(pack);
        writer.member(packMemberFirst, "tools");
        writer.integer(counts.first);
        writer.member(packMemberFirst, "active");
        writer.integer(counts.second);
        writer.character('}');
    }
    writer.character(']');
    writer.member(first, "agents");
    writer.nullValue();
    writer.member(first, "agent_sessions");
    writer.character('[');
    for (std::size_t index{}; index < forge.agentSessions.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeAgentSession(writer, forge.agentSessions[index]);
    }
    writer.character(']');
    writer.member(first, "agents_summary");
    writeAgentsSummary(writer, forge.agentSessions);
    writer.member(first, "jobs");
    writer.nullValue();
    writer.member(first, "live_feed");
    writer.nullValue();
    writer.member(first, "feed_stats");
    writer.nullValue();
    writer.member(first, "orchestration");
    writeOrchestration(writer, forge);
    writer.member(first, "mcp_load");
    writer.nullValue();
    writer.member(first, "files");
    writer.nullValue();
    writer.member(first, "audit_recent");
    writer.nullValue();
    writer.character('}');
}

void writeHistoryPoint(
    BoundedJsonWriter& writer,
    const Domain::HistoryPoint& point)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "ts");
    writer.number(unixUtcSeconds(point.timestamp));
    writer.member(first, "cpu");
    writer.number(point.cpuPercent);
    writer.member(first, "ram");
    writer.number(point.ramPercent);
    writer.member(first, "gpu");
    writeOptionalNumber(writer, point.gpuPercent);
    writer.member(first, "disk_io");
    writer.number(point.diskBytesPerSecond / BytesPerMebibyte);
    writer.member(first, "mcp");
    writer.integer(point.mcpEvents);
    writer.member(first, "orch");
    writer.string(healthName(point.orchestrationHealth));
    writer.character('}');
}

void writeFrame(
    BoundedJsonWriter& writer,
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "system");
    writeSystem(writer, snapshot.system);
    writer.member(first, "forge");
    writeForge(writer, snapshot.forge);
    writer.member(first, "updated");
    writer.number(unixUtcSeconds(snapshot.updatedAt));
    writer.member(first, "history");
    writer.character('[');
    for (std::size_t index{}; index < snapshot.history.size(); ++index) {
        if (index != 0U) writer.character(',');
        writeHistoryPoint(writer, snapshot.history[index]);
    }
    writer.character(']');
    writer.member(first, "runtime");
    writer.string(snapshot.runtime);
    writer.member(first, "stream");
    writer.string("realtime");
    writer.member(first, "sample_hz");
    writeOptionalNumber(writer, measuredSampleHz);
    writer.character('}');
}

void validateHealth(
    const Domain::TelemetryHealthReport& report,
    const std::size_t maximumEncodedBytes)
{
    TextBudget text{maximumEncodedBytes};
    text.add(report.service);
    text.add(report.runtime);
    text.add(report.mode);
    text.add(report.collectors);
    text.add(report.ui);
}

void writeHealth(
    BoundedJsonWriter& writer,
    const Domain::TelemetryHealthReport& report)
{
    writer.character('{');
    bool first{true};
    writer.member(first, "ok");
    writer.boolean(report.ok);
    writer.member(first, "service");
    writer.string(report.service);
    writer.member(first, "runtime");
    writer.string(report.runtime);
    writer.member(first, "interferes_with_mcp");
    writer.boolean(report.interferesWithMcp);
    writer.member(first, "mode");
    writer.string(report.mode);
    writer.member(first, "auth");
    writer.boolean(true);
    writer.member(first, "collectors");
    writer.string(report.collectors);
    writer.member(first, "ui");
    writer.string(report.ui);
    writer.member(first, "export_present");
    writer.nullValue();
    writer.member(first, "static_present");
    writer.nullValue();
    writer.member(first, "node_available");
    writer.nullValue();
    writer.member(first, "node_required");
    writer.boolean(report.nodeRequired);
    writer.character('}');
}

template <typename Validate, typename Write>
[[nodiscard]] Domain::Result<std::string> encode(
    const std::size_t maximumEncodedBytes,
    Validate&& validate,
    Write&& write) noexcept
{
    try {
        validateMaximumEncodedBytes(maximumEncodedBytes);
        validate();
        BoundedJsonWriter writer{maximumEncodedBytes};
        write(writer);
        auto encoded = std::move(writer).finish();
        if (encoded.empty()) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard telemetry encoder produced an empty payload.");
        }
        return Domain::Result<std::string>::success(std::move(encoded));
    } catch (const TelemetryCodecException& error) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            error.code(), error.what()));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard telemetry encoding failed safely."));
    }
}

} // namespace

Domain::Result<std::string> DashboardTelemetryJsonCodec::encodeHealth(
    const Domain::TelemetryHealthReport& report,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encode(
        maximumEncodedBytes,
        [&] { validateHealth(report, maximumEncodedBytes); },
        [&](BoundedJsonWriter& writer) { writeHealth(writer, report); });
}

Domain::Result<std::string> DashboardTelemetryJsonCodec::encodeLiveFrame(
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encode(
        maximumEncodedBytes,
        [&] {
            validateSnapshot(snapshot, measuredSampleHz, maximumEncodedBytes);
        },
        [&](BoundedJsonWriter& writer) {
            writeFrame(writer, snapshot, measuredSampleHz);
        });
}

Domain::Result<std::string> DashboardTelemetryJsonCodec::encodeFrame(
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encodeLiveFrame(snapshot, measuredSampleHz, maximumEncodedBytes);
}

Domain::Result<std::string> DashboardTelemetryJsonCodec::encodeSnapshot(
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encodeLiveFrame(snapshot, measuredSampleHz, maximumEncodedBytes);
}

Domain::Result<std::string> DashboardTelemetryJsonCodec::encodeSystem(
    const Domain::SystemMetrics& system,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encode(
        maximumEncodedBytes,
        [&] { validateSystem(system, maximumEncodedBytes); },
        [&](BoundedJsonWriter& writer) { writeSystem(writer, system); });
}

Domain::Result<std::string> DashboardTelemetryJsonCodec::encodeForge(
    const Domain::ForgeSnapshot& forge,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encode(
        maximumEncodedBytes,
        [&] { validateForge(forge, maximumEncodedBytes); },
        [&](BoundedJsonWriter& writer) { writeForge(writer, forge); });
}

Domain::Result<std::string>
DashboardTelemetryJsonCodec::encodeServerSentEvent(
    const Domain::TelemetrySnapshot& snapshot,
    const std::optional<double> measuredSampleHz,
    const std::size_t maximumEncodedBytes) noexcept
{
    return encode(
        maximumEncodedBytes,
        [&] {
            validateSnapshot(snapshot, measuredSampleHz, maximumEncodedBytes);
        },
        [&](BoundedJsonWriter& writer) {
            writer.raw("event: telemetry\ndata: ");
            writeFrame(writer, snapshot, measuredSampleHz);
            writer.raw("\n\n");
        });
}

} // namespace ForgeConductor::Dashboard
