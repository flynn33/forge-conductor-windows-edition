#include "ForgeConductor/Telemetry/Windows/WindowsTelemetryService.h"

#include "ForgeConductor/Telemetry/Windows/WindowsCpuMetricsCollector.h"
#include "ForgeConductor/Telemetry/Windows/WindowsRamMetricsCollector.h"

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iterator>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

namespace ForgeConductor::Telemetry::Windows {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] Domain::Error closedError()
{
    return Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "The Windows telemetry service is stopped.");
}

[[nodiscard]] std::uint64_t fileTimeValue(const FILETIME value) noexcept
{
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

[[nodiscard]] std::string utf8ComputerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1U]{};
    DWORD count = static_cast<DWORD>(std::size(name));
    if (::GetComputerNameW(name, &count) == FALSE || count == 0U) return "windows-host";
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, name, static_cast<int>(count),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "windows-host";
    std::string value(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, name, static_cast<int>(count),
            value.data(), required, nullptr, nullptr) != required) {
        return "windows-host";
    }
    return value;
}

[[nodiscard]] std::string utf8Text(const wchar_t* const text)
{
    if (text == nullptr) return {};
    const int length = static_cast<int>(wcsnlen_s(text, 128U));
    if (length <= 0) return {};
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text, length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string value(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
            value.data(), required, nullptr, nullptr) != required) return {};
    return value;
}

[[nodiscard]] std::string adapterKey(const LUID luid)
{
    std::ostringstream value;
    value << std::hex << std::nouppercase
          << static_cast<std::uint32_t>(luid.HighPart) << ':' << luid.LowPart;
    return value.str();
}

struct GpuEngineSample final {
    std::string adapterId;
    std::string engine;
    double percent{};
};

class GpuEngineQuery final {
public:
    GpuEngineQuery()
    {
        if (::PdhOpenQueryW(nullptr, 0U, &query_) != ERROR_SUCCESS) return;
        if (::PdhAddEnglishCounterW(
                query_, L"\\GPU Engine(*)\\Utilization Percentage", 0U,
                &counter_) != ERROR_SUCCESS) {
            ::PdhCloseQuery(query_);
            query_ = nullptr;
            return;
        }
        available_ = true;
    }

    ~GpuEngineQuery()
    {
        if (query_ != nullptr) ::PdhCloseQuery(query_);
    }

    [[nodiscard]] std::vector<GpuEngineSample> sample(std::string& status)
    {
        std::vector<GpuEngineSample> result;
        if (!available_) {
            status = "GPU Engine PDH counter is unavailable.";
            return result;
        }
        if (::PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            status = "GPU Engine PDH collection failed.";
            return result;
        }
        if (!warmed_) {
            warmed_ = true;
            status = "GPU Engine PDH counter is warming up.";
            return result;
        }
        DWORD bytes{};
        DWORD count{};
        auto pdh = ::PdhGetFormattedCounterArrayW(
            counter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (pdh != PDH_MORE_DATA || bytes == 0U) {
            status = "GPU Engine PDH instances are temporarily unavailable.";
            return result;
        }
        std::vector<std::byte> storage(bytes);
        auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        pdh = ::PdhGetFormattedCounterArrayW(
            counter_, PDH_FMT_DOUBLE, &bytes, &count, values);
        if (pdh != ERROR_SUCCESS) {
            status = "GPU Engine PDH instance formatting failed.";
            return result;
        }
        for (DWORD index{}; index < count; ++index) {
            if (values[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                values[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const std::wstring instance{values[index].szName};
            const auto luid = instance.find(L"luid_0x");
            const auto low = luid == std::wstring::npos
                ? std::wstring::npos : instance.find(L"_0x", luid + 7U);
            const auto physical = low == std::wstring::npos
                ? std::wstring::npos : instance.find(L"_phys_", low + 3U);
            const auto engineIndex = instance.find(L"_eng_", physical);
            const auto engineType = instance.find(L"_engtype_", engineIndex);
            if (luid == std::wstring::npos || low == std::wstring::npos ||
                physical == std::wstring::npos || engineIndex == std::wstring::npos ||
                engineType == std::wstring::npos) continue;
            try {
                const auto highValue = std::stoul(
                    instance.substr(luid + 7U, low - (luid + 7U)), nullptr, 16);
                const auto lowValue = std::stoul(
                    instance.substr(low + 3U, physical - (low + 3U)), nullptr, 16);
                const auto engineNumber = instance.substr(
                    engineIndex + 5U, engineType - (engineIndex + 5U));
                const auto type = instance.substr(engineType + 9U);
                std::ostringstream key;
                key << std::hex << std::nouppercase << highValue << ':' << lowValue;
                result.push_back(GpuEngineSample{
                    key.str(),
                    utf8Text((type + L" " + engineNumber).c_str()),
                    std::clamp(values[index].FmtValue.doubleValue, 0.0, 100.0)});
            } catch (...) {
            }
        }
        status = "GPU utilization measured by native GPU Engine PDH counters.";
        return result;
    }

private:
    PDH_HQUERY query_{};
    PDH_HCOUNTER counter_{};
    bool available_{};
    bool warmed_{};
};

class DiskIoQuery final {
public:
    DiskIoQuery()
    {
        if (::PdhOpenQueryW(nullptr, 0U, &query_) != ERROR_SUCCESS) return;
        const wchar_t* paths[] = {
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
            L"\\PhysicalDisk(_Total)\\Disk Reads/sec",
            L"\\PhysicalDisk(_Total)\\Disk Writes/sec"};
        for (std::size_t index{}; index < std::size(paths); ++index) {
            if (::PdhAddEnglishCounterW(
                    query_, paths[index], 0U, &counters_[index]) != ERROR_SUCCESS) {
                ::PdhCloseQuery(query_);
                query_ = nullptr;
                return;
            }
        }
        available_ = true;
    }

    ~DiskIoQuery()
    {
        if (query_ != nullptr) ::PdhCloseQuery(query_);
    }

    [[nodiscard]] Domain::TelemetryMetric<Domain::DiskIoMetrics> sample(
        const Domain::UtcTimePoint capturedAt)
    {
        constexpr auto source = "PDH PhysicalDisk(_Total) rate counters";
        if (!available_) {
            return Domain::makeUnavailableTelemetryMetric<Domain::DiskIoMetrics>(
                Domain::TelemetryMetricAvailability::Unsupported, capturedAt, source,
                "PhysicalDisk PDH counters are unavailable.");
        }
        if (::PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return Domain::makeUnavailableTelemetryMetric<Domain::DiskIoMetrics>(
                Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
                capturedAt, source, "PhysicalDisk PDH collection failed.");
        }
        if (!warmed_) {
            warmed_ = true;
            return Domain::makeUnavailableTelemetryMetric<Domain::DiskIoMetrics>(
                Domain::TelemetryMetricAvailability::WarmingUp, capturedAt, source,
                "PhysicalDisk rate counters require a second sample.");
        }
        double values[4]{};
        for (std::size_t index{}; index < std::size(counters_); ++index) {
            PDH_FMT_COUNTERVALUE formatted{};
            if (::PdhGetFormattedCounterValue(
                    counters_[index], PDH_FMT_DOUBLE, nullptr, &formatted) != ERROR_SUCCESS ||
                (formatted.CStatus != PDH_CSTATUS_VALID_DATA &&
                 formatted.CStatus != PDH_CSTATUS_NEW_DATA)) {
                return Domain::makeUnavailableTelemetryMetric<Domain::DiskIoMetrics>(
                    Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
                    capturedAt, source, "A PhysicalDisk rate counter is invalid.");
            }
            values[index] = std::max(0.0, formatted.doubleValue);
        }
        return Domain::makeAvailableTelemetryMetric(
            Domain::DiskIoMetrics{values[0], values[1], values[2], values[3]},
            capturedAt, source);
    }

private:
    PDH_HQUERY query_{};
    PDH_HCOUNTER counters_[4]{};
    bool available_{};
    bool warmed_{};
};

[[nodiscard]] std::vector<Domain::DiskVolume> collectVolumes(
    const Domain::UtcTimePoint capturedAt)
{
    DWORD characters = ::GetLogicalDriveStringsW(0U, nullptr);
    if (characters == 0U) return {};
    std::vector<wchar_t> roots(static_cast<std::size_t>(characters) + 1U);
    if (::GetLogicalDriveStringsW(characters, roots.data()) == 0U) return {};
    std::vector<Domain::DiskVolume> result;
    for (const wchar_t* root = roots.data(); *root != L'\0'; root += wcslen(root) + 1U) {
        const UINT type = ::GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER available{}, total{}, free{};
        if (::GetDiskFreeSpaceExW(root, &available, &total, &free) == FALSE ||
            total.QuadPart == 0U) continue;
        wchar_t fileSystem[MAX_PATH]{};
        static_cast<void>(::GetVolumeInformationW(
            root, nullptr, 0U, nullptr, nullptr, nullptr, fileSystem,
            static_cast<DWORD>(std::size(fileSystem))));
        const auto mountText = utf8Text(root);
        auto mount = Domain::PathText::create(mountText);
        if (!mount) continue;
        const auto used = total.QuadPart - free.QuadPart;
        result.push_back(Domain::DiskVolume{
            mountText, std::move(mount).value(), utf8Text(fileSystem),
            total.QuadPart, used, available.QuadPart,
            static_cast<double>(used) * 100.0 / static_cast<double>(total.QuadPart),
            capturedAt, "GetLogicalDriveStrings/GetDiskFreeSpaceEx"});
    }
    return result;
}

[[nodiscard]] std::vector<Domain::GpuMetrics> collectGpus(
    GpuEngineQuery& query,
    const Domain::UtcTimePoint capturedAt,
    std::string& status)
{
    std::string engineStatus;
    const auto engineSamples = query.sample(engineStatus);
    std::map<std::string, std::map<std::string, double>> engineTotals;
    for (const auto& sample : engineSamples) {
        engineTotals[sample.adapterId][sample.engine] = std::clamp(
            engineTotals[sample.adapterId][sample.engine] + sample.percent, 0.0, 100.0);
    }
    std::vector<Domain::GpuMetrics> result;
    IDXGIFactory1* factory{};
    const HRESULT created = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(created) || factory == nullptr) {
        status = "DXGI adapter discovery unavailable (HRESULT " +
            std::to_string(static_cast<std::uint32_t>(created)) + ").";
        return result;
    }
    for (UINT index{};; ++index) {
        IDXGIAdapter1* adapter{};
        const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
        if (enumerated == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enumerated) || adapter == nullptr) {
            status = "DXGI adapter enumeration failed.";
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U) {
            std::ostringstream vendor;
            vendor << "PCI vendor 0x" << std::hex << std::uppercase
                   << description.VendorId;
            Domain::GpuMetrics metrics;
            metrics.vendor = vendor.str();
            metrics.name = utf8Text(description.Description);
            metrics.adapterId = adapterKey(description.AdapterLuid);
            metrics.dedicatedBytesTotal = static_cast<std::uint64_t>(
                description.DedicatedVideoMemory);
            IDXGIAdapter3* adapter3{};
            if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) &&
                adapter3 != nullptr) {
                DXGI_QUERY_VIDEO_MEMORY_INFO information{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                        0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &information))) {
                    metrics.dedicatedBytesUsed = static_cast<std::uint64_t>(
                        information.CurrentUsage);
                }
                adapter3->Release();
                metrics.direct3dAvailable = true;
            }
            metrics.capturedAt = capturedAt;
            metrics.utilizationSource = engineStatus;
            metrics.memoryScope = "DXGI current-process local-segment usage; adapter capacity";
            if (const auto found = engineTotals.find(metrics.adapterId);
                found != engineTotals.end()) {
                double maximum{};
                for (const auto& [engine, percent] : found->second) {
                    metrics.engines.push_back(Domain::GpuEngineMetrics{engine, percent});
                    maximum = std::max(maximum, percent);
                }
                metrics.utilizationPercent = maximum;
            }
            result.push_back(std::move(metrics));
        }
        adapter->Release();
    }
    factory->Release();
    status = result.empty()
        ? "DXGI found no hardware adapters; GPU measurements are unsupported."
        : "DXGI adapter identity and scoped memory accounting; " + engineStatus;
    return result;
}

} // namespace

class WindowsTelemetryService::Impl final {
public:
    Impl(
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator,
        Domain::PathText home,
        Domain::ResourceBudgets budgets)
        : clock_{clock},
          uuidGenerator_{uuidGenerator},
          home_{std::move(home)},
          budgets_{std::move(budgets)},
          cpu_{createWindowsCpuMetricsCollector(clock)},
          ram_{createWindowsRamMetricsCollector(clock)}
    {
    }

    ~Impl() { stop(); }

    [[nodiscard]] Domain::Result<void> start(
        const Domain::OperationContext& context)
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "Windows telemetry startup was cancelled."));
        }
        if (context.isExpired(clock_.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "Windows telemetry startup deadline expired."));
        }
        std::lock_guard lock{mutex_};
        if (closed_) return Domain::Result<void>::failure(closedError());
        if (worker_.joinable()) return Domain::Result<void>::success();
        worker_ = std::jthread([this](const std::stop_token stop) { run(stop); });
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<Snapshot> sample(
        const Domain::OperationContext& context)
    {
        {
            std::lock_guard lock{mutex_};
            if (closed_) return Domain::Result<Snapshot>::failure(closedError());
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<Snapshot>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "Windows telemetry sampling was cancelled."));
        }
        if (context.isExpired(clock_.monotonicNow())) {
            return Domain::Result<Snapshot>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "Windows telemetry sampling deadline expired."));
        }

        std::unique_lock collectionLock{collectionMutex_, std::try_to_lock};
        if (!collectionLock.owns_lock()) {
            const auto value = latest();
            return value
                ? Domain::Result<Snapshot>::success(value)
                : Domain::Result<Snapshot>::failure(Domain::makeError(
                      Domain::ErrorCodes::LimitExceeded,
                      "A Windows telemetry sample is already in progress."));
        }

        auto cpu = cpu_->collect(context);
        if (!cpu) return Domain::Result<Snapshot>::failure(cpu.error());
        auto ram = ram_->collect(context);
        if (!ram) return Domain::Result<Snapshot>::failure(ram.error());
        const auto capturedAt = clock_.utcNow();
        const auto observedAt = clock_.monotonicNow();
        std::optional<double> measuredIntervalMilliseconds;
        if (lastSampleObservedAt_ && observedAt > *lastSampleObservedAt_) {
            measuredIntervalMilliseconds = std::chrono::duration<double, std::milli>(
                observedAt - *lastSampleObservedAt_).count();
        }
        lastSampleObservedAt_ = observedAt;
        const auto sequence = sampleSequence_++;
        if (sequence % 4U == 0U || cachedGpus_.empty()) {
            cachedGpus_ = collectGpus(gpuQuery_, capturedAt, gpuStatus_);
            cachedDiskIo_ = diskQuery_.sample(capturedAt);
        }
        if (sequence % 20U == 0U || cachedProcesses_.empty()) {
            cachedProcesses_ = processMetrics(
                capturedAt, observedAt,
                cpu.value().logicalProcessorCount.value.value_or(1U));
            cachedVolumes_ = collectVolumes(capturedAt);
        }

        Domain::SystemMetrics system;
        system.timestamp = capturedAt;
        system.host = utf8ComputerName();
        system.platform = "Windows 11";
        system.architecture = "x64";
        system.cpu = std::move(cpu).value();
        system.ram = std::move(ram).value();
        system.gpus = cachedGpus_;
        system.processes = cachedProcesses_;
        system.disks = cachedVolumes_;
        system.diskIoSample = cachedDiskIo_;
        if (cachedDiskIo_.value) system.diskIo = *cachedDiskIo_.value;
        system.targetSampleIntervalMilliseconds = 250U;
        system.measuredSampleIntervalMilliseconds = measuredIntervalMilliseconds;

        Domain::ForgeSnapshot forge{
            capturedAt,
            home_,
            "windows-manager",
            1U,
            0U,
            {},
            {},
            0U,
            Domain::TelemetryHealth::Ok};

        std::vector<Domain::HistoryPoint> history;
        Consumer consumer;
        Snapshot snapshot;
        {
            std::lock_guard lock{mutex_};
            if (system.cpu.percent.value && system.ram.percent.value) {
                std::optional<double> gpuPercent;
                for (const auto& gpu : system.gpus) {
                    if (gpu.utilizationPercent) {
                        gpuPercent = std::max(
                            gpuPercent.value_or(0.0), *gpu.utilizationPercent);
                    }
                }
                history_.push_back(Domain::HistoryPoint{
                    capturedAt, *system.cpu.percent.value,
                    *system.ram.percent.value, gpuPercent,
                    system.diskIo.readBytesPerSecond +
                        system.diskIo.writeBytesPerSecond,
                    0U, Domain::TelemetryHealth::Ok});
            }
            const auto maximum = std::max<std::size_t>(
                1U, budgets_.historyPointsDefault);
            if (history_.size() > maximum) {
                history_.erase(history_.begin(), history_.begin() +
                    static_cast<std::ptrdiff_t>(history_.size() - maximum));
            }
            history = history_;
            snapshot = std::make_shared<const Domain::TelemetrySnapshot>(
                Domain::TelemetrySnapshot{
                    std::move(system), std::move(forge), capturedAt,
                    std::move(history), "windows-native"});
            const auto valid = Domain::validateTelemetrySnapshot(*snapshot, budgets_);
            if (!valid) return Domain::Result<Snapshot>::failure(valid.error());
            latest_ = snapshot;
            consumer = consumer_;
        }
        if (consumer) consumer(snapshot);
        return Domain::Result<Snapshot>::success(std::move(snapshot));
    }

    [[nodiscard]] Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext& context) const
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<Domain::TelemetryHealthReport>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                    "Windows telemetry health was cancelled."));
        }
        std::lock_guard lock{mutex_};
        if (closed_) {
            return Domain::Result<Domain::TelemetryHealthReport>::failure(closedError());
        }
        return Domain::Result<Domain::TelemetryHealthReport>::success(
            Domain::TelemetryHealthReport{
                true,
                "forge-telemetry",
                "windows-native",
                false,
                "continuous-native",
                "PDH logical CPU/frequency and disk/GPU engines; GlobalMemoryStatusEx RAM; "
                "Toolhelp relevant-process inventory; logical-volume capacity; " + gpuStatus_,
                "WinUI 3 + SSE",
                false});
    }

    [[nodiscard]] Domain::Result<void> setConsumer(Consumer consumer)
    {
        std::lock_guard lock{mutex_};
        if (closed_) return Domain::Result<void>::failure(closedError());
        consumer_ = std::move(consumer);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Snapshot latest() const
    {
        std::lock_guard lock{mutex_};
        return latest_;
    }

    void stop() noexcept
    {
        std::jthread worker;
        {
            std::lock_guard lock{mutex_};
            if (closed_) return;
            closed_ = true;
            consumer_ = {};
            worker = std::move(worker_);
        }
        worker.request_stop();
        wake_.notify_all();
        if (worker.joinable()) worker.join();
        cpu_->shutdown();
        ram_->shutdown();
    }

private:
    void run(const std::stop_token stop) noexcept
    {
        try {
            while (!stop.stop_requested()) {
                auto operation = uuidGenerator_.next();
                auto correlation = uuidGenerator_.next();
                auto correlationId = correlation
                    ? Domain::CorrelationId::parse(correlation.value().value())
                    : Domain::Result<Domain::CorrelationId>::failure(
                          correlation.error());
                if (operation && correlationId) {
                    const Domain::OperationContext context{
                        Domain::OperationId{operation.value()},
                        clock_.monotonicNow() + 10s,
                        stop,
                        std::move(correlationId).value()};
                    static_cast<void>(sample(context));
                }
                std::unique_lock lock{waitMutex_};
                wake_.wait_for(lock, stop, 250ms, [] { return false; });
            }
        } catch (...) {
        }
    }

    struct ProcessCpuState final {
        std::uint64_t times{};
        Domain::MonotonicTimePoint observedAt;
    };

    [[nodiscard]] static bool relevantProcessName(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](const unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        return name.find("forgeconductor") != std::string::npos ||
            name.find("forge-conductor") != std::string::npos ||
            name.find("lm studio") != std::string::npos ||
            name.find("lmstudio") != std::string::npos ||
            name == "lms.exe" || name == "ollama.exe" ||
            name.find("llama-server") != std::string::npos;
    }

    [[nodiscard]] std::vector<Domain::ProcessMetrics> processMetrics(
        const Domain::UtcTimePoint capturedAt,
        const Domain::MonotonicTimePoint observedAt,
        const std::uint32_t logicalProcessorCount)
    {
        std::vector<Domain::ProcessMetrics> result;
        std::set<std::uint32_t> observed;
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
        if (snapshot == INVALID_HANDLE_VALUE) return result;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                const auto name = utf8Text(entry.szExeFile);
                if (!relevantProcessName(name)) continue;
                observed.insert(entry.th32ProcessID);
                HANDLE process = ::OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                    FALSE, entry.th32ProcessID);
                if (process == nullptr) {
                    process = ::OpenProcess(
                        PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, entry.th32ProcessID);
                }
                FILETIME creation{}, exit{}, kernel{}, user{};
                PROCESS_MEMORY_COUNTERS_EX memory{};
                memory.cb = sizeof(memory);
                DWORD handles{};
                const bool timesOk = process != nullptr && ::GetProcessTimes(
                    process, &creation, &exit, &kernel, &user) != FALSE;
                const bool memoryOk = process != nullptr && ::GetProcessMemoryInfo(
                    process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                    sizeof(memory)) != FALSE;
                if (process != nullptr) {
                    static_cast<void>(::GetProcessHandleCount(process, &handles));
                }
                double percent{};
                bool measured{};
                if (timesOk) {
                    const auto current = fileTimeValue(kernel) + fileTimeValue(user);
                    if (const auto prior = processTimes_.find(entry.th32ProcessID);
                        prior != processTimes_.end() && current >= prior->second.times &&
                        observedAt > prior->second.observedAt) {
                        const auto elapsed100ns = std::chrono::duration_cast<
                            std::chrono::duration<long double, std::ratio<1, 10'000'000>>>(
                                observedAt - prior->second.observedAt).count();
                        if (elapsed100ns > 0.0L && logicalProcessorCount > 0U) {
                            percent = std::clamp(static_cast<double>(
                                static_cast<long double>(current - prior->second.times) /
                                elapsed100ns * 100.0L / logicalProcessorCount), 0.0, 100.0);
                            measured = true;
                        }
                    }
                    processTimes_[entry.th32ProcessID] = ProcessCpuState{current, observedAt};
                }
                result.push_back(Domain::ProcessMetrics{
                    entry.th32ProcessID, name, percent,
                    memoryOk ? static_cast<std::uint64_t>(memory.WorkingSetSize) : 0U,
                    memoryOk ? static_cast<std::uint64_t>(memory.PrivateUsage) : 0U,
                    entry.cntThreads, static_cast<std::uint32_t>(handles),
                    timesOk
                        ? std::string{measured ? "measured; " : "warming_up; "} +
                            "Toolhelp/GetProcessTimes/GetProcessMemoryInfo"
                        : "Toolhelp identity; process counters unavailable",
                    capturedAt});
                if (process != nullptr) ::CloseHandle(process);
            } while (::Process32NextW(snapshot, &entry) != FALSE);
        }
        ::CloseHandle(snapshot);
        for (auto iterator = processTimes_.begin(); iterator != processTimes_.end();) {
            iterator = observed.contains(iterator->first)
                ? std::next(iterator) : processTimes_.erase(iterator);
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.workingSetBytes > right.workingSetBytes;
        });
        return result;
    }

    Contracts::IClock& clock_;
    Contracts::IUuidGenerator& uuidGenerator_;
    Domain::PathText home_;
    Domain::ResourceBudgets budgets_;
    std::unique_ptr<Contracts::ICpuMetricsCollector> cpu_;
    std::unique_ptr<Contracts::IRamMetricsCollector> ram_;
    mutable std::mutex mutex_;
    std::mutex collectionMutex_;
    std::mutex waitMutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
    Consumer consumer_;
    Snapshot latest_;
    std::vector<Domain::HistoryPoint> history_;
    GpuEngineQuery gpuQuery_;
    DiskIoQuery diskQuery_;
    std::vector<Domain::GpuMetrics> cachedGpus_;
    Domain::TelemetryMetric<Domain::DiskIoMetrics> cachedDiskIo_;
    std::vector<Domain::DiskVolume> cachedVolumes_;
    std::vector<Domain::ProcessMetrics> cachedProcesses_;
    std::unordered_map<std::uint32_t, ProcessCpuState> processTimes_;
    std::optional<Domain::MonotonicTimePoint> lastSampleObservedAt_;
    std::uint64_t sampleSequence_{};
    std::string gpuStatus_{"GPU discovery has not sampled yet."};
    bool closed_{};
};

WindowsTelemetryService::WindowsTelemetryService(
    Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuidGenerator,
    Domain::PathText home,
    Domain::ResourceBudgets budgets)
    : implementation_{std::make_shared<Impl>(
          clock, uuidGenerator, std::move(home), std::move(budgets))} {}

WindowsTelemetryService::~WindowsTelemetryService()
{
    auto implementation = std::move(implementation_);
    if (implementation) implementation->stop();
}

Domain::Result<void> WindowsTelemetryService::start(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation ? implementation->start(context)
            : Domain::Result<void>::failure(closedError());
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Windows telemetry startup failed safely."));
    }
}

Domain::Result<Contracts::ITelemetryService::Snapshot>
WindowsTelemetryService::sample(
    const bool,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation ? implementation->sample(context)
            : Domain::Result<Snapshot>::failure(closedError());
    } catch (...) {
        return Domain::Result<Snapshot>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Windows telemetry sampling failed safely."));
    }
}

Domain::Result<Domain::TelemetryHealthReport> WindowsTelemetryService::health(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation ? implementation->health(context)
            : Domain::Result<Domain::TelemetryHealthReport>::failure(closedError());
    } catch (...) {
        return Domain::Result<Domain::TelemetryHealthReport>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                "Windows telemetry health failed safely."));
    }
}

Domain::Result<void> WindowsTelemetryService::setConsumer(Consumer consumer) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation ? implementation->setConsumer(std::move(consumer))
            : Domain::Result<void>::failure(closedError());
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Windows telemetry consumer registration failed safely."));
    }
}

Contracts::ITelemetryService::Snapshot WindowsTelemetryService::latest() const noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation ? implementation->latest() : Snapshot{};
    } catch (...) { return {}; }
}

std::size_t WindowsTelemetryService::pendingCount() const noexcept { return 0U; }

void WindowsTelemetryService::stop() noexcept
{
    try {
        const auto implementation = implementation_;
        if (implementation) implementation->stop();
    } catch (...) {}
}

std::unique_ptr<Contracts::ITelemetryService> createWindowsTelemetryService(
    Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuidGenerator,
    Domain::PathText home,
    Domain::ResourceBudgets budgets)
{
    return std::make_unique<WindowsTelemetryService>(
        clock, uuidGenerator, std::move(home), std::move(budgets));
}

} // namespace ForgeConductor::Telemetry::Windows
