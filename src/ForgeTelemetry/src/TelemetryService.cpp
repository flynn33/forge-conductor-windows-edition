// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeTelemetry/TelemetryService.h"

#include "ForgeDomain/Version.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#include <pdh.h>
#include <psapi.h>
#include <winioctl.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

namespace Forge::Telemetry {
namespace {

double fileTimeToDouble(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart);
}

} // namespace

typedef LONG (WINAPI* NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

struct ProcessorTimes {
    ULONGLONG idle{0};
    ULONGLONG kernel{0};
    ULONGLONG user{0};
};

struct SystemCollector::Impl {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    bool haveCpu{false};
    std::vector<ProcessorTimes> lastCores;
    std::string cpuBrand;
    DWORD cpuMhz{0};
    PDH_HQUERY pdh{nullptr};
    PDH_HCOUNTER diskRead{nullptr};
    PDH_HCOUNTER diskWrite{nullptr};
    PDH_HCOUNTER gpuUtil{nullptr};
    bool pdhReady{false};
    ULONGLONG lastIoRead{0};
    ULONGLONG lastIoWrite{0};
    ULONGLONG lastIoReadOps{0};
    ULONGLONG lastIoWriteOps{0};
    std::chrono::steady_clock::time_point lastIoAt{};
    bool haveIo{false};
    std::unordered_map<DWORD, ULONGLONG> lastProcTimes;
    std::chrono::steady_clock::time_point lastProcAt{};
    bool haveProcTimes{false};
};

SystemCollector::SystemCollector() : impl_(std::make_unique<Impl>()) {
    wchar_t brand[256]{};
    DWORD brandSize = sizeof(brand);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString",
            RRF_RT_REG_SZ,
            nullptr,
            brand,
            &brandSize) == ERROR_SUCCESS) {
        char utf8[256]{};
        WideCharToMultiByte(CP_UTF8, 0, brand, -1, utf8, sizeof(utf8), nullptr, nullptr);
        impl_->cpuBrand = utf8;
        while (!impl_->cpuBrand.empty() && impl_->cpuBrand.front() == ' ') {
            impl_->cpuBrand.erase(impl_->cpuBrand.begin());
        }
    }
    DWORD mhz = 0;
    DWORD mhzSize = sizeof(mhz);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"~MHz",
            RRF_RT_REG_DWORD,
            nullptr,
            &mhz,
            &mhzSize) == ERROR_SUCCESS) {
        impl_->cpuMhz = mhz;
    }
    if (PdhOpenQueryW(nullptr, 0, &impl_->pdh) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(
            impl_->pdh, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &impl_->diskRead);
        PdhAddEnglishCounterW(
            impl_->pdh, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &impl_->diskWrite);
        PdhAddEnglishCounterW(
            impl_->pdh, L"\\GPU Engine(*)\\Utilization Percentage", 0, &impl_->gpuUtil);
        impl_->pdhReady = PdhCollectQueryData(impl_->pdh) == ERROR_SUCCESS;
    }
}

SystemCollector::~SystemCollector() {
    if (impl_ && impl_->pdh) {
        PdhCloseQuery(impl_->pdh);
    }
}

Domain::SystemMetrics SystemCollector::collect() {
    Domain::SystemMetrics metrics;

    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (impl_->haveCpu) {
            const auto idleDelta = fileTimeToDouble(idle) - fileTimeToDouble(impl_->idle);
            const auto kernelDelta = fileTimeToDouble(kernel) - fileTimeToDouble(impl_->kernel);
            const auto userDelta = fileTimeToDouble(user) - fileTimeToDouble(impl_->user);
            const auto total = kernelDelta + userDelta;
            if (total > 0) {
                metrics.cpu.totalPercent = (1.0 - (idleDelta / total)) * 100.0;
            }
        }
        impl_->idle = idle;
        impl_->kernel = kernel;
        impl_->user = user;
        impl_->haveCpu = true;
    }
    metrics.cpu.brand = impl_->cpuBrand;
    metrics.cpu.frequencyMhz = impl_->cpuMhz;

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    struct CoreSnap {
        ULONGLONG idle;
        ULONGLONG kernel;
        ULONGLONG user;
    };
    std::vector<CoreSnap> cores(sys.dwNumberOfProcessors);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto query = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
        if (query) {
            std::vector<BYTE> buffer(sys.dwNumberOfProcessors * 48);
            ULONG got = 0;
            if (query(8 /*SystemProcessorPerformanceInformation*/, buffer.data(),
                    static_cast<ULONG>(buffer.size()), &got) >= 0) {
                cores.resize(sys.dwNumberOfProcessors);
                for (DWORD i = 0; i < sys.dwNumberOfProcessors; ++i) {
                    const auto* row = reinterpret_cast<const LARGE_INTEGER*>(buffer.data() + i * 48);
                    cores[i].idle = static_cast<ULONGLONG>(row[0].QuadPart);
                    cores[i].kernel = static_cast<ULONGLONG>(row[1].QuadPart);
                    cores[i].user = static_cast<ULONGLONG>(row[2].QuadPart);
                }
            }
        }
    }
    metrics.cpu.perCore.resize(sys.dwNumberOfProcessors);
    if (impl_->lastCores.size() == cores.size()) {
        for (size_t i = 0; i < cores.size(); ++i) {
            const auto idleDelta = cores[i].idle - impl_->lastCores[i].idle;
            const auto kernelDelta = cores[i].kernel - impl_->lastCores[i].kernel;
            const auto userDelta = cores[i].user - impl_->lastCores[i].user;
            const auto total = kernelDelta + userDelta;
            metrics.cpu.perCore[i] = total > 0
                ? (1.0 - (static_cast<double>(idleDelta) / static_cast<double>(total))) * 100.0
                : 0.0;
        }
    } else {
        metrics.cpu.perCore.assign(cores.size(), metrics.cpu.totalPercent);
    }
    impl_->lastCores.resize(cores.size());
    for (size_t i = 0; i < cores.size(); ++i) {
        impl_->lastCores[i] = ProcessorTimes{cores[i].idle, cores[i].kernel, cores[i].user};
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        metrics.ram.totalBytes = memory.ullTotalPhys;
        metrics.ram.usedBytes = memory.ullTotalPhys - memory.ullAvailPhys;
        metrics.ram.usedPercent = memory.dwMemoryLoad;
    }

    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            Domain::GPUMetrics gpu;
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            gpu.name = name;
            gpu.dedicatedMemoryBytes = desc.DedicatedVideoMemory;
            gpu.utilizationPercent = 0;
            metrics.gpus.push_back(std::move(gpu));
            adapter->Release();
        }
        factory->Release();
    }

    ULONGLONG readBytes = 0;
    ULONGLONG writeBytes = 0;
    ULONGLONG readOps = 0;
    ULONGLONG writeOps = 0;
    bool gotIo = false;
    if (HMODULE ntdllIo = GetModuleHandleW(L"ntdll.dll")) {
        auto query = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdllIo, "NtQuerySystemInformation"));
        if (query) {
            BYTE perf[1024]{};
            ULONG got = 0;
            if (query(2 /*SystemPerformanceInformation*/, perf, sizeof(perf), &got) >= 0) {
                const auto* counters = reinterpret_cast<const ULONGLONG*>(perf);
                readBytes = counters[1];
                writeBytes = counters[2];
                const auto* ops = reinterpret_cast<const ULONG*>(perf + 32);
                readOps = ops[0];
                writeOps = ops[1];
                gotIo = true;
            }
        }
    }
    if (!gotIo) {
        HANDLE volume = CreateFileW(L"\\\\.\\C:", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (volume != INVALID_HANDLE_VALUE) {
            DISK_PERFORMANCE diskPerf{};
            DWORD returned = 0;
            if (DeviceIoControl(volume, IOCTL_DISK_PERFORMANCE, nullptr, 0, &diskPerf, sizeof(diskPerf),
                    &returned, nullptr)) {
                readBytes = static_cast<ULONGLONG>(diskPerf.BytesRead.QuadPart);
                writeBytes = static_cast<ULONGLONG>(diskPerf.BytesWritten.QuadPart);
                readOps = static_cast<ULONGLONG>(diskPerf.ReadCount);
                writeOps = static_cast<ULONGLONG>(diskPerf.WriteCount);
                gotIo = true;
            }
            CloseHandle(volume);
        }
    }
    if (gotIo) {
        const auto now = std::chrono::steady_clock::now();
        if (impl_->haveIo) {
            const double sec = std::chrono::duration<double>(now - impl_->lastIoAt).count();
            if (sec > 0.001) {
                metrics.diskIO.readMBps =
                    (static_cast<double>(readBytes - impl_->lastIoRead) / sec) / (1024.0 * 1024.0);
                metrics.diskIO.writeMBps =
                    (static_cast<double>(writeBytes - impl_->lastIoWrite) / sec) / (1024.0 * 1024.0);
                metrics.diskIO.readIops = static_cast<double>(readOps - impl_->lastIoReadOps) / sec;
                metrics.diskIO.writeIops = static_cast<double>(writeOps - impl_->lastIoWriteOps) / sec;
            }
        }
        impl_->lastIoRead = readBytes;
        impl_->lastIoWrite = writeBytes;
        impl_->lastIoReadOps = readOps;
        impl_->lastIoWriteOps = writeOps;
        impl_->lastIoAt = now;
        impl_->haveIo = true;
    }

    if (impl_->pdhReady && impl_->pdh && PdhCollectQueryData(impl_->pdh) == ERROR_SUCCESS) {
        if (!gotIo) {
            PDH_FMT_COUNTERVALUE value{};
            if (impl_->diskRead &&
                PdhGetFormattedCounterValue(impl_->diskRead, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS) {
                metrics.diskIO.readMBps = value.doubleValue / (1024.0 * 1024.0);
            }
            if (impl_->diskWrite &&
                PdhGetFormattedCounterValue(impl_->diskWrite, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS) {
                metrics.diskIO.writeMBps = value.doubleValue / (1024.0 * 1024.0);
            }
        }
        if (impl_->gpuUtil) {
            DWORD bufSize = 0;
            DWORD itemCount = 0;
            const PDH_STATUS more = PdhGetFormattedCounterArrayW(
                impl_->gpuUtil, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
            constexpr PDH_STATUS kPdhMoreData = static_cast<PDH_STATUS>(0x800007D2);
            if ((more == kPdhMoreData || more == ERROR_SUCCESS) && bufSize > 0) {
                std::vector<BYTE> buffer(bufSize);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                if (PdhGetFormattedCounterArrayW(
                        impl_->gpuUtil, PDH_FMT_DOUBLE, &bufSize, &itemCount, items) == ERROR_SUCCESS) {
                    double best = 0.0;
                    double util3d = 0.0;
                    double utilCompute = 0.0;
                    double utilCopy = 0.0;
                    for (DWORD i = 0; i < itemCount; ++i) {
                        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) {
                            continue;
                        }
                        const double value = items[i].FmtValue.doubleValue;
                        best = (std::max)(best, value);
                        const wchar_t* name = items[i].szName ? items[i].szName : L"";
                        if (wcsstr(name, L"engtype_3D") || wcsstr(name, L"3D")) {
                            util3d = (std::max)(util3d, value);
                        } else if (wcsstr(name, L"Compute") || wcsstr(name, L"engtype_Compute")) {
                            utilCompute = (std::max)(utilCompute, value);
                        } else if (wcsstr(name, L"Copy") || wcsstr(name, L"engtype_Copy")) {
                            utilCopy = (std::max)(utilCopy, value);
                        }
                    }
                    if (!metrics.gpus.empty()) {
                        metrics.gpus.front().utilizationPercent = best;
                        metrics.gpus.front().util3d = util3d;
                        metrics.gpus.front().utilCompute = utilCompute;
                        metrics.gpus.front().utilCopy = utilCopy;
                    }
                }
            }
        }
    }

    DWORD mask = GetLogicalDrives();
    char letter = 'A';
    while (mask) {
        if (mask & 1) {
            const std::string root = std::string(1, letter) + ":\\";
            const UINT driveType = GetDriveTypeA(root.c_str());
            ULARGE_INTEGER freeBytes{}, totalBytes{};
            if ((driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) &&
                GetDiskFreeSpaceExA(root.c_str(), &freeBytes, &totalBytes, nullptr) &&
                totalBytes.QuadPart >= (8ull * 1024ull * 1024ull * 1024ull)) {
                Domain::DiskVolume volume;
                volume.name = root;
                volume.mount = root;
                volume.totalBytes = totalBytes.QuadPart;
                volume.freeBytes = freeBytes.QuadPart;
                metrics.volumes.push_back(volume);
            }
        }
        mask >>= 1;
        ++letter;
    }

    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        metrics.power.onAc = power.ACLineStatus == 1;
        metrics.power.batteryPercent = power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent;
    }

    DWORD needed = 0;
    DWORD pids[1024]{};
    std::vector<Domain::ProcessMetrics> procs;
    std::unordered_map<DWORD, ULONGLONG> nextProcTimes;
    const auto nowProc = std::chrono::steady_clock::now();
    const double wallSec = impl_->haveProcTimes
        ? std::chrono::duration<double>(nowProc - impl_->lastProcAt).count()
        : 0.0;
    const double ncpu = sys.dwNumberOfProcessors > 0 ? static_cast<double>(sys.dwNumberOfProcessors) : 1.0;
    if (EnumProcesses(pids, sizeof(pids), &needed)) {
        const auto count = needed / sizeof(DWORD);
        for (unsigned i = 0; i < count; ++i) {
            if (pids[i] == 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
            if (!process) {
                continue;
            }
            wchar_t image[MAX_PATH]{};
            DWORD imageChars = MAX_PATH;
            Domain::ProcessMetrics row;
            row.pid = static_cast<std::int32_t>(pids[i]);
            if (QueryFullProcessImageNameW(process, 0, image, &imageChars)) {
                const wchar_t* slash = image;
                for (const wchar_t* p = image; *p; ++p) {
                    if (*p == L'\\' || *p == L'/') {
                        slash = p + 1;
                    }
                }
                char name[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, slash, -1, name, MAX_PATH, nullptr, nullptr);
                row.name = name;
            }
            PROCESS_MEMORY_COUNTERS mem{};
            if (GetProcessMemoryInfo(process, &mem, sizeof(mem))) {
                row.workingSetBytes = mem.WorkingSetSize;
            }
            FILETIME created{}, exited{}, kernelT{}, userT{};
            if (GetProcessTimes(process, &created, &exited, &kernelT, &userT)) {
                ULARGE_INTEGER k{}, u{};
                k.LowPart = kernelT.dwLowDateTime;
                k.HighPart = kernelT.dwHighDateTime;
                u.LowPart = userT.dwLowDateTime;
                u.HighPart = userT.dwHighDateTime;
                const ULONGLONG total = k.QuadPart + u.QuadPart;
                nextProcTimes[pids[i]] = total;
                const auto prior = impl_->lastProcTimes.find(pids[i]);
                if (impl_->haveProcTimes && prior != impl_->lastProcTimes.end() && wallSec > 0.001) {
                    const double delta = static_cast<double>(total - prior->second);
                    row.cpuPercent = (delta / (wallSec * 10000000.0)) * 100.0 / ncpu;
                    if (row.cpuPercent < 0) {
                        row.cpuPercent = 0;
                    }
                }
            }
            if (!row.name.empty()) {
                procs.push_back(std::move(row));
            }
            CloseHandle(process);
        }
    }
    impl_->lastProcTimes = std::move(nextProcTimes);
    impl_->lastProcAt = nowProc;
    impl_->haveProcTimes = true;
    std::sort(procs.begin(), procs.end(), [](const Domain::ProcessMetrics& a, const Domain::ProcessMetrics& b) {
        const auto score = [](const Domain::ProcessMetrics& p) {
            double s = p.cpuPercent * 10.0 + static_cast<double>(p.workingSetBytes) / (1024.0 * 1024.0);
            if (p.name.find("Forge") != std::string::npos ||
                p.name.find("lmstudio") != std::string::npos ||
                p.name.find("LM Studio") != std::string::npos ||
                p.name.find("llama") != std::string::npos) {
                s += 1000.0;
            }
            return s;
        };
        return score(a) > score(b);
    });
    if (procs.size() > 16) {
        procs.resize(16);
    }
    metrics.processes = std::move(procs);
    return metrics;
}

TelemetryService::TelemetryService(
    Persistence::SQLiteStore& store,
    std::function<std::vector<std::string>()> toolNames)
    : store_(store)
    , toolNames_(std::move(toolNames)) {}

TelemetryService::~TelemetryService() { stop(); }

Domain::TelemetrySnapshot TelemetryService::compose() {
    Domain::TelemetrySnapshot frame;
    frame.system = collector_.collect();
    frame.version = Domain::kVersion;
    frame.product = Domain::kProductName;
#ifdef _WIN32
    frame.pid = static_cast<std::int32_t>(GetCurrentProcessId());
#endif
    try {
        frame.presence = store_.presenceRecords();
        frame.recentAudit = store_.auditRecent(20);
    } catch (...) {
    }
    for (const auto& row : frame.presence) {
        if (row.hostKind == "mcp-stdio") frame.primaryAlive = true;
        if (row.hostKind == "mcp-stdio-fallback") frame.fallbackAlive = true;
    }
    if (toolNames_) {
        frame.tools = toolNames_();
    }
    return frame;
}

Domain::TelemetrySnapshot TelemetryService::currentFrame() {
    std::lock_guard lock(mutex_);
    if (latest_.version.empty()) {
        latest_ = compose();
    }
    return latest_;
}

void TelemetryService::start(double intervalSec) {
    stop();
    running_ = true;
    worker_ = std::thread([this, intervalSec] {
        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        int samples = 0;
        auto windowStart = last;
        while (running_) {
            auto frame = compose();
            {
                std::lock_guard lock(mutex_);
                latest_ = frame;
                for (auto& [_, listener] : listeners_) {
                    listener(frame);
                }
            }
            ++samples;
            const auto now = clock::now();
            if (now - windowStart >= std::chrono::seconds(1)) {
                measuredHz_ = samples;
                samples = 0;
                windowStart = now;
            }
            const auto period = std::chrono::duration<double>(intervalSec <= 0 ? 0.033 : intervalSec);
            std::this_thread::sleep_until(last + std::chrono::duration_cast<clock::duration>(period));
            last = clock::now();
        }
    });
}

void TelemetryService::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::uint64_t TelemetryService::addListener(std::function<void(const Domain::TelemetrySnapshot&)> listener) {
    std::lock_guard lock(mutex_);
    const auto id = nextId_++;
    listeners_[id] = std::move(listener);
    return id;
}

void TelemetryService::removeListener(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    listeners_.erase(id);
}

} // namespace Forge::Telemetry
