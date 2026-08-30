#include "ForgeConductor/Telemetry/Windows/WindowsRamMetricsCollector.h"

#include "Detail/IRamMetricsPlatform.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>
#include <Psapi.h>

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Telemetry::Windows {
namespace {

using Availability = Domain::TelemetryMetricAvailability;

constexpr std::string_view PhysicalTotalSource =
    "GlobalMemoryStatusEx.ullTotalPhys";
constexpr std::string_view PhysicalAvailableSource =
    "GlobalMemoryStatusEx.ullAvailPhys";
constexpr std::string_view PhysicalUsedSource =
    "GlobalMemoryStatusEx total minus available";
constexpr std::string_view PhysicalPercentSource =
    "GlobalMemoryStatusEx physical utilization";
constexpr std::string_view CommitPressureSource =
    "GetPerformanceInfo.CommitTotal / CommitLimit";
constexpr std::string_view CommittedBytesSource =
    "GetPerformanceInfo.CommitTotal * PageSize";
constexpr std::string_view PagedPoolSource =
    "GetPerformanceInfo.KernelPaged * PageSize";
constexpr std::string_view ActiveBytesSource =
    "Windows memory model: no active-byte equivalent";
constexpr std::string_view WiredBytesSource =
    "Windows memory model: no wired-byte equivalent";
constexpr std::string_view CompressedBytesSource =
    "PDH \\Memory\\Compressed Page Count";
constexpr std::string_view SwapSource =
    "Windows memory model: no equivalent swap metric";

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock,
    const std::string_view action)
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The RAM telemetry collection was cancelled before " +
                std::string{action} + "."));
    }
    if (context.isExpired(clock.monotonicNow())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The RAM telemetry collection deadline expired before " +
                std::string{action} + "."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Availability availabilityFor(const Domain::Error& error) noexcept
{
    if (error.code == Domain::ErrorCodes::Unauthorized) {
        return Availability::AccessDenied;
    }
    if (error.code == Domain::ErrorCodes::HostCapabilityUnavailable) {
        return Availability::Unsupported;
    }
    return Availability::TemporarilyUnavailable;
}

[[nodiscard]] std::string boundedReason(
    const Domain::Error& error,
    const std::string_view fallback)
{
    if (!error.message.empty() &&
        error.message.size() <= Domain::TelemetryMetricReasonBytesMaximum &&
        error.message.find('\0') == std::string::npos &&
        Domain::isValidUtf8(error.message)) {
        return error.message;
    }
    return std::string{fallback};
}

template <typename T>
[[nodiscard]] Domain::TelemetryMetric<T> failedMetric(
    const Domain::TelemetryMetric<T>* previous,
    const Availability availability,
    const Domain::UtcTimePoint observedAt,
    const std::string_view source,
    const std::string& reason)
{
    if (previous != nullptr && previous->value.has_value()) {
        return Domain::makeStaleTelemetryMetric(
            *previous,
            availability,
            observedAt,
            reason);
    }
    return Domain::makeUnavailableTelemetryMetric<T>(
        availability,
        observedAt,
        std::string{source},
        reason);
}

template <typename T>
[[nodiscard]] const Domain::TelemetryMetric<T>* priorMetric(
    const std::optional<Domain::RamMetrics>& previous,
    const Domain::TelemetryMetric<T> Domain::RamMetrics::* member) noexcept
{
    return previous ? &(previous.value().*member) : nullptr;
}

[[nodiscard]] bool checkedMultiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& product) noexcept
{
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

[[nodiscard]] double percent(
    const std::uint64_t numerator,
    const std::uint64_t denominator) noexcept
{
    const auto value =
        static_cast<long double>(numerator) * 100.0L /
        static_cast<long double>(denominator);
    return static_cast<double>(value);
}

template <typename T>
[[nodiscard]] Domain::Result<T> boundaryFailure(
    const std::string_view message,
    const std::exception* exception = nullptr)
{
    try {
        std::string detail{message};
        if (exception != nullptr) {
            detail += ": ";
            detail += exception->what();
        }
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::move(detail)));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Windows RAM collector failed at its native boundary."));
    }
}

class AdmissionRelease final {
public:
    AdmissionRelease(
        std::mutex& lifecycleMutex,
        bool& active,
        std::condition_variable& changed) noexcept
        : lifecycleMutex_{lifecycleMutex},
          active_{active},
          changed_{changed}
    {
    }

    ~AdmissionRelease() noexcept
    {
        {
            const std::lock_guard lock{lifecycleMutex_};
            active_ = false;
        }
        changed_.notify_all();
    }

    AdmissionRelease(const AdmissionRelease&) = delete;
    AdmissionRelease& operator=(const AdmissionRelease&) = delete;

private:
    std::mutex& lifecycleMutex_;
    bool& active_;
    std::condition_variable& changed_;
};

} // namespace

namespace Detail {
namespace {

[[nodiscard]] Domain::Error win32ProbeError(
    const std::string_view operation,
    const DWORD code)
{
    std::string message{operation};
    message += " failed with Win32 error ";
    message += std::to_string(code);
    message += '.';

    if (code == ERROR_ACCESS_DENIED) {
        return Domain::makeError(
            Domain::ErrorCodes::Unauthorized,
            std::move(message));
    }
    if (code == ERROR_CALL_NOT_IMPLEMENTED ||
        code == ERROR_PROC_NOT_FOUND ||
        code == ERROR_NOT_SUPPORTED) {
        return Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            std::move(message));
    }
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::move(message),
        true);
}

class WindowsRamMetricsPlatform final : public IRamMetricsPlatform {
public:
    [[nodiscard]] Domain::Result<PhysicalMemoryObservation>
    queryPhysicalMemory() noexcept override
    {
        try {
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (::GlobalMemoryStatusEx(&status) == FALSE) {
                return Domain::Result<PhysicalMemoryObservation>::failure(
                    win32ProbeError("GlobalMemoryStatusEx", ::GetLastError()));
            }
            return Domain::Result<PhysicalMemoryObservation>::success(
                PhysicalMemoryObservation{
                    static_cast<std::uint64_t>(status.ullTotalPhys),
                    static_cast<std::uint64_t>(status.ullAvailPhys)});
        } catch (const std::exception& exception) {
            return Domain::Result<PhysicalMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    std::string{"GlobalMemoryStatusEx mapping failed: "} +
                        exception.what()));
        } catch (...) {
            return Domain::Result<PhysicalMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "GlobalMemoryStatusEx mapping failed."));
        }
    }

    [[nodiscard]] Domain::Result<PerformanceMemoryObservation>
    queryPerformanceMemory() noexcept override
    {
        try {
            PERFORMANCE_INFORMATION information{};
            information.cb = sizeof(information);
            if (::GetPerformanceInfo(
                    &information,
                    static_cast<DWORD>(sizeof(information))) == FALSE) {
                return Domain::Result<PerformanceMemoryObservation>::failure(
                    win32ProbeError("GetPerformanceInfo", ::GetLastError()));
            }
            return Domain::Result<PerformanceMemoryObservation>::success(
                PerformanceMemoryObservation{
                    static_cast<std::uint64_t>(information.PageSize),
                    static_cast<std::uint64_t>(information.CommitTotal),
                    static_cast<std::uint64_t>(information.CommitLimit),
                    static_cast<std::uint64_t>(information.KernelPaged)});
        } catch (const std::exception& exception) {
            return Domain::Result<PerformanceMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    std::string{"GetPerformanceInfo mapping failed: "} +
                        exception.what()));
        } catch (...) {
            return Domain::Result<PerformanceMemoryObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "GetPerformanceInfo mapping failed."));
        }
    }
};

} // namespace

std::shared_ptr<IRamMetricsPlatform> createWindowsRamMetricsPlatform()
{
    return std::make_shared<WindowsRamMetricsPlatform>();
}

} // namespace Detail

class WindowsRamMetricsCollector::Impl final {
public:
    Impl(
        Contracts::IClock& clock,
        std::shared_ptr<Detail::IRamMetricsPlatform> platform)
        : clock_{clock}, platform_{std::move(platform)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::RamMetrics> collect(
        const Domain::OperationContext& context)
    {
        const auto initial = validateContext(context, clock_, "collector admission");
        if (!initial) {
            return Domain::Result<Domain::RamMetrics>::failure(initial.error());
        }
        {
            const std::lock_guard lock{lifecycleMutex_};
            if (closed_) {
                return closedCollector();
            }
            if (active_) {
                return Domain::Result<Domain::RamMetrics>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The synchronous RAM collector already has an active operation."));
            }
            active_ = true;
        }
        AdmissionRelease release{
            lifecycleMutex_, active_, lifecycleChanged_};

        if (!platform_) {
            return Domain::Result<Domain::RamMetrics>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The RAM collector requires a Windows memory platform."));
        }

        auto ready = validateContinuation(context, "query physical memory");
        if (!ready) {
            return Domain::Result<Domain::RamMetrics>::failure(
                std::move(ready).error());
        }
        auto physical = platform_->queryPhysicalMemory();
        ready = validateContinuation(context, "map physical memory");
        if (!ready) {
            return Domain::Result<Domain::RamMetrics>::failure(
                std::move(ready).error());
        }

        ready = validateContinuation(context, "query performance memory");
        if (!ready) {
            return Domain::Result<Domain::RamMetrics>::failure(
                std::move(ready).error());
        }
        auto performance = platform_->queryPerformanceMemory();
        ready = validateContinuation(context, "map performance memory");
        if (!ready) {
            return Domain::Result<Domain::RamMetrics>::failure(
                std::move(ready).error());
        }

        const auto observedAt = clock_.utcNow();
        Domain::RamMetrics metrics;
        mapPhysical(metrics, physical, observedAt);
        mapPerformance(metrics, performance, observedAt);
        mapUnsupported(metrics, observedAt);

        const auto valid = Domain::validateRamMetrics(metrics);
        if (!valid) {
            return Domain::Result<Domain::RamMetrics>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The Windows RAM collector produced an invalid metric state: " +
                        valid.error().message));
        }

        const auto committed = commit(context, metrics);
        if (!committed) {
            return Domain::Result<Domain::RamMetrics>::failure(
                committed.error());
        }
        return Domain::Result<Domain::RamMetrics>::success(std::move(metrics));
    }

    void shutdown()
    {
        std::unique_lock lock{lifecycleMutex_};
        closed_ = true;
        lifecycleChanged_.wait(lock, [this]() noexcept {
            return !active_;
        });
    }

private:
    [[nodiscard]] Domain::Result<Domain::RamMetrics> closedCollector() const
    {
        return Domain::Result<Domain::RamMetrics>::failure(Domain::makeError(
            Domain::ErrorCodes::TransportClosed,
            "The Windows RAM collector is shut down."));
    }

    [[nodiscard]] Domain::Result<void> validateContinuation(
        const Domain::OperationContext& context,
        const std::string_view action)
    {
        const auto current = validateContext(context, clock_, action);
        if (!current) {
            return current;
        }
        {
            const std::lock_guard lock{lifecycleMutex_};
            if (closed_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The Windows RAM collector was shut down during collection."));
            }
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> commit(
        const Domain::OperationContext& context,
        const Domain::RamMetrics& metrics)
    {
        const std::lock_guard lock{lifecycleMutex_};
        const auto current = validateContext(
            context, clock_, "publish RAM metrics");
        if (!current) {
            return current;
        }
        if (closed_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The Windows RAM collector was shut down before publication."));
        }
        previous_ = metrics;
        return Domain::Result<void>::success();
    }

    void mapPhysical(
        Domain::RamMetrics& metrics,
        const Domain::Result<Detail::PhysicalMemoryObservation>& observation,
        const Domain::UtcTimePoint observedAt) const
    {
        if (!observation) {
            const auto availability = availabilityFor(observation.error());
            const auto reason = boundedReason(
                observation.error(),
                "The physical-memory probe failed without a usable reason.");
            setPhysicalFailure(metrics, availability, observedAt, reason);
            return;
        }

        const auto& raw = observation.value();
        if (raw.totalBytes == 0U || raw.availableBytes > raw.totalBytes) {
            setPhysicalFailure(
                metrics,
                Availability::TemporarilyUnavailable,
                observedAt,
                "GlobalMemoryStatusEx returned incoherent physical-memory values.");
            return;
        }

        const auto usedBytes = raw.totalBytes - raw.availableBytes;
        metrics.totalBytes = Domain::makeAvailableTelemetryMetric(
            raw.totalBytes, observedAt, std::string{PhysicalTotalSource});
        metrics.availableBytes = Domain::makeAvailableTelemetryMetric(
            raw.availableBytes, observedAt, std::string{PhysicalAvailableSource});
        metrics.usedBytes = Domain::makeAvailableTelemetryMetric(
            usedBytes, observedAt, std::string{PhysicalUsedSource});
        metrics.percent = Domain::makeAvailableTelemetryMetric(
            percent(usedBytes, raw.totalBytes),
            observedAt,
            std::string{PhysicalPercentSource});
    }

    void setPhysicalFailure(
        Domain::RamMetrics& metrics,
        const Availability availability,
        const Domain::UtcTimePoint observedAt,
        const std::string& reason) const
    {
        metrics.totalBytes = failedMetric(
            priorMetric(previous_, &Domain::RamMetrics::totalBytes),
            availability, observedAt, PhysicalTotalSource, reason);
        metrics.availableBytes = failedMetric(
            priorMetric(previous_, &Domain::RamMetrics::availableBytes),
            availability, observedAt, PhysicalAvailableSource, reason);
        metrics.usedBytes = failedMetric(
            priorMetric(previous_, &Domain::RamMetrics::usedBytes),
            availability, observedAt, PhysicalUsedSource, reason);
        metrics.percent = failedMetric(
            priorMetric(previous_, &Domain::RamMetrics::percent),
            availability, observedAt, PhysicalPercentSource, reason);
    }

    void mapPerformance(
        Domain::RamMetrics& metrics,
        const Domain::Result<Detail::PerformanceMemoryObservation>& observation,
        const Domain::UtcTimePoint observedAt) const
    {
        if (!observation) {
            const auto availability = availabilityFor(observation.error());
            const auto reason = boundedReason(
                observation.error(),
                "The performance-memory probe failed without a usable reason.");
            metrics.pressurePercent = failedMetric(
                priorMetric(previous_, &Domain::RamMetrics::pressurePercent),
                availability, observedAt, CommitPressureSource, reason);
            metrics.committedBytes = failedMetric(
                priorMetric(previous_, &Domain::RamMetrics::committedBytes),
                availability, observedAt, CommittedBytesSource, reason);
            metrics.pagedPoolBytes = failedMetric(
                priorMetric(previous_, &Domain::RamMetrics::pagedPoolBytes),
                availability, observedAt, PagedPoolSource, reason);
            return;
        }

        const auto& raw = observation.value();
        if (raw.commitLimitPages == 0U ||
            raw.committedPages > raw.commitLimitPages) {
            metrics.pressurePercent = failedMetric(
                priorMetric(previous_, &Domain::RamMetrics::pressurePercent),
                Availability::TemporarilyUnavailable,
                observedAt,
                CommitPressureSource,
                "GetPerformanceInfo returned an incoherent commit ratio.");
        } else {
            metrics.pressurePercent = Domain::makeAvailableTelemetryMetric(
                percent(raw.committedPages, raw.commitLimitPages),
                observedAt,
                std::string{CommitPressureSource});
        }

        std::uint64_t committedBytes{};
        if (raw.pageSizeBytes == 0U ||
            !checkedMultiply(
                raw.committedPages, raw.pageSizeBytes, committedBytes)) {
            metrics.committedBytes = failedMetric(
                priorMetric(previous_, &Domain::RamMetrics::committedBytes),
                Availability::TemporarilyUnavailable,
                observedAt,
                CommittedBytesSource,
                "GetPerformanceInfo commit-byte mapping overflowed or had a zero page size.");
        } else {
            metrics.committedBytes = Domain::makeAvailableTelemetryMetric(
                committedBytes,
                observedAt,
                std::string{CommittedBytesSource});
        }

        std::uint64_t pagedPoolBytes{};
        if (raw.pageSizeBytes == 0U ||
            !checkedMultiply(
                raw.pagedPoolPages, raw.pageSizeBytes, pagedPoolBytes)) {
            metrics.pagedPoolBytes = failedMetric(
                priorMetric(previous_, &Domain::RamMetrics::pagedPoolBytes),
                Availability::TemporarilyUnavailable,
                observedAt,
                PagedPoolSource,
                "GetPerformanceInfo paged-pool mapping overflowed or had a zero page size.");
        } else {
            metrics.pagedPoolBytes = Domain::makeAvailableTelemetryMetric(
                pagedPoolBytes,
                observedAt,
                std::string{PagedPoolSource});
        }
    }

    static void mapUnsupported(
        Domain::RamMetrics& metrics,
        const Domain::UtcTimePoint observedAt)
    {
        metrics.activeBytes = Domain::makeUnavailableTelemetryMetric<std::uint64_t>(
            Availability::Unsupported,
            observedAt,
            std::string{ActiveBytesSource},
            "Windows does not expose a verified active-byte metric equivalent.");
        metrics.wiredBytes = Domain::makeUnavailableTelemetryMetric<std::uint64_t>(
            Availability::Unsupported,
            observedAt,
            std::string{WiredBytesSource},
            "Windows does not expose a verified wired-byte metric equivalent.");
        metrics.compressedBytes =
            Domain::makeUnavailableTelemetryMetric<std::uint64_t>(
                Availability::Unsupported,
                observedAt,
                std::string{CompressedBytesSource},
                "The qualified machine does not expose the intended PDH compressed-memory counter.");
        metrics.swapTotalBytes =
            Domain::makeUnavailableTelemetryMetric<std::uint64_t>(
                Availability::Unsupported,
                observedAt,
                std::string{SwapSource},
                "Windows commit and pagefile counters are not equivalent to swap total.");
        metrics.swapUsedBytes =
            Domain::makeUnavailableTelemetryMetric<std::uint64_t>(
                Availability::Unsupported,
                observedAt,
                std::string{SwapSource},
                "Windows commit and pagefile counters are not equivalent to swap used.");
        metrics.swapPercent = Domain::makeUnavailableTelemetryMetric<double>(
            Availability::Unsupported,
            observedAt,
            std::string{SwapSource},
            "Windows commit and pagefile counters are not equivalent to swap utilization.");
    }

    Contracts::IClock& clock_;
    std::shared_ptr<Detail::IRamMetricsPlatform> platform_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    bool closed_{};
    bool active_{};
    std::optional<Domain::RamMetrics> previous_;
};

WindowsRamMetricsCollector::WindowsRamMetricsCollector(Contracts::IClock& clock)
    : WindowsRamMetricsCollector(
          clock,
          Detail::createWindowsRamMetricsPlatform())
{
}

WindowsRamMetricsCollector::WindowsRamMetricsCollector(
    Contracts::IClock& clock,
    std::shared_ptr<Detail::IRamMetricsPlatform> platform)
    : implementation_{std::make_shared<Impl>(clock, std::move(platform))}
{
}

WindowsRamMetricsCollector::~WindowsRamMetricsCollector()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        try {
            implementation->shutdown();
        } catch (...) {
        }
    }
}

Domain::Result<Domain::RamMetrics> WindowsRamMetricsCollector::collect(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->collect(context)
            : Domain::Result<Domain::RamMetrics>::failure(Domain::makeError(
                  Domain::ErrorCodes::TransportClosed,
                  "The Windows RAM collector is not available."));
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::RamMetrics>(
            "The Windows RAM collector could not collect metrics",
            &exception);
    } catch (...) {
        return boundaryFailure<Domain::RamMetrics>(
            "The Windows RAM collector could not collect metrics");
    }
}

void WindowsRamMetricsCollector::shutdown() noexcept
{
    try {
        const auto implementation = implementation_;
        if (implementation) {
            implementation->shutdown();
        }
    } catch (...) {
    }
}

std::unique_ptr<Contracts::IRamMetricsCollector>
createWindowsRamMetricsCollector(Contracts::IClock& clock)
{
    return std::make_unique<WindowsRamMetricsCollector>(clock);
}

} // namespace ForgeConductor::Telemetry::Windows
