#include "ForgeConductor/Telemetry/Windows/WindowsTelemetryService.h"

#include "ForgeConductor/Telemetry/Windows/WindowsCpuMetricsCollector.h"
#include "ForgeConductor/Telemetry/Windows/WindowsRamMetricsCollector.h"

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

[[nodiscard]] std::uint32_t currentThreadCount() noexcept
{
    const DWORD current = ::GetCurrentProcessId();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0U);
    if (snapshot == INVALID_HANDLE_VALUE) return 0U;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t count{};
    if (::Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == current) ++count;
        } while (::Thread32Next(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return count;
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

[[nodiscard]] std::vector<Domain::GpuMetrics> collectGpus(std::string& status)
{
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
            result.push_back(std::move(metrics));
        }
        adapter->Release();
    }
    factory->Release();
    status = result.empty()
        ? "DXGI found no hardware adapters; GPU measurements are unsupported."
        : "DXGI adapter identity and local-memory accounting available; utilization unsupported.";
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

        Domain::SystemMetrics system;
        system.timestamp = capturedAt;
        system.host = utf8ComputerName();
        system.platform = "Windows 11";
        system.architecture = "x64";
        system.cpu = std::move(cpu).value();
        system.ram = std::move(ram).value();
        std::string gpuStatus;
        system.gpus = collectGpus(gpuStatus);
        system.processes.push_back(processMetrics(capturedAt, system.cpu));

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
                history_.push_back(Domain::HistoryPoint{
                    capturedAt, *system.cpu.percent.value,
                    *system.ram.percent.value, std::nullopt,
                    0.0, 0U, Domain::TelemetryHealth::Ok});
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
            gpuStatus_ = std::move(gpuStatus);
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
                "GetSystemTimes CPU; GlobalMemoryStatusEx RAM; current-process Win32 metrics; " + gpuStatus_,
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
                wake_.wait_for(lock, stop, 1s, [] { return false; });
            }
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::ProcessMetrics processMetrics(
        const Domain::UtcTimePoint,
        const Domain::CpuMetrics& cpu)
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        DWORD handles{};
        const HANDLE process = ::GetCurrentProcess();
        const bool timesOk = ::GetProcessTimes(
            process, &creation, &exit, &kernel, &user) != FALSE;
        const bool memoryOk = ::GetProcessMemoryInfo(
            process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory)) != FALSE;
        static_cast<void>(::GetProcessHandleCount(process, &handles));

        double percent{};
        bool processMeasured{};
        const auto now = clock_.monotonicNow();
        if (timesOk && processTimes_ && processObservedAt_ && now > *processObservedAt_) {
            const auto current = fileTimeValue(kernel) + fileTimeValue(user);
            if (current >= *processTimes_) {
                const auto elapsed100ns = std::chrono::duration_cast<
                    std::chrono::duration<long double, std::ratio<1, 10'000'000>>>(
                        now - *processObservedAt_).count();
                const auto logical = cpu.logicalProcessorCount.value.value_or(1U);
                if (elapsed100ns > 0.0L && logical > 0U) {
                    percent = static_cast<double>(
                        static_cast<long double>(current - *processTimes_) /
                        elapsed100ns * 100.0L / logical);
                    percent = std::clamp(percent, 0.0, 100.0);
                    processMeasured = true;
                }
            }
        }
        if (timesOk) {
            processTimes_ = fileTimeValue(kernel) + fileTimeValue(user);
            processObservedAt_ = now;
        }
        return Domain::ProcessMetrics{
            static_cast<std::uint32_t>(::GetCurrentProcessId()),
            "ForgeConductor.Manager",
            percent,
            memoryOk ? static_cast<std::uint64_t>(memory.WorkingSetSize) : 0U,
            memoryOk ? static_cast<std::uint64_t>(memory.PrivateUsage) : 0U,
            currentThreadCount(),
            static_cast<std::uint32_t>(handles),
            timesOk
                ? std::string{processMeasured ? "measured; " : "warming_up; "} +
                    "GetProcessTimes/GetProcessMemoryInfo; creation=" +
                    std::to_string(fileTimeValue(creation))
                : "Win32 process metrics unavailable"};
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
    std::optional<std::uint64_t> processTimes_;
    std::optional<Domain::MonotonicTimePoint> processObservedAt_;
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
