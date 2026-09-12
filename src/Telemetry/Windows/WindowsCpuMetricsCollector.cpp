#include "ForgeConductor/Telemetry/Windows/WindowsCpuMetricsCollector.h"

#include "Detail/ICpuMetricsPlatform.h"
#include "Detail/SynchronousCollectorGate.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Telemetry::Windows {
namespace {

using Availability = Domain::TelemetryMetricAvailability;
constexpr std::string_view AggregateSource{"GetSystemTimes cumulative delta"};
constexpr std::string_view LogicalSource{"PDH Processor Information utilization"};
constexpr std::string_view TopologySource{"GetActiveProcessorCount and GetLogicalProcessorInformationEx"};
constexpr std::string_view FrequencySource{"PDH Processor Information Actual Frequency"};
constexpr std::string_view BrandSource{"HKLM ProcessorNameString"};
constexpr std::string_view LoadSource{"Windows scheduling model: no 1/5/15 load-average equivalent"};

[[nodiscard]] Availability availabilityFor(const Domain::Error& error) noexcept
{
    if (error.code == Domain::ErrorCodes::Unauthorized) return Availability::AccessDenied;
    if (error.code == Domain::ErrorCodes::HostCapabilityUnavailable) return Availability::Unsupported;
    return Availability::TemporarilyUnavailable;
}

[[nodiscard]] std::string reasonFor(
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
    if (previous != nullptr && previous->value) {
        return Domain::makeStaleTelemetryMetric(
            *previous, availability, observedAt, reason);
    }
    return Domain::makeUnavailableTelemetryMetric<T>(
        availability, observedAt, std::string{source}, reason);
}

template <typename T>
[[nodiscard]] const Domain::TelemetryMetric<T>* prior(
    const std::optional<Domain::CpuMetrics>& previous,
    const Domain::TelemetryMetric<T> Domain::CpuMetrics::* member) noexcept
{
    return previous ? &(previous.value().*member) : nullptr;
}

[[nodiscard]] bool validIdentitySet(
    const Detail::CpuTopologyObservation& topology) noexcept
{
    if (topology.logicalProcessorCount == 0U ||
        topology.physicalCoreCount == 0U ||
        topology.physicalCoreCount > topology.logicalProcessorCount ||
        topology.logicalProcessors.size() != topology.logicalProcessorCount) {
        return false;
    }
    std::set<std::pair<std::uint16_t, std::uint16_t>> identities;
    for (const auto& item : topology.logicalProcessors) {
        identities.emplace(item.group, item.processor);
    }
    return identities.size() == topology.logicalProcessors.size();
}

template <typename Observation>
[[nodiscard]] bool sameIdentities(
    const std::vector<Observation>& values,
    const Detail::CpuTopologyObservation& topology) noexcept
{
    if (values.size() != topology.logicalProcessors.size()) return false;
    std::set<std::pair<std::uint16_t, std::uint16_t>> expected;
    std::set<std::pair<std::uint16_t, std::uint16_t>> actual;
    for (const auto& item : topology.logicalProcessors) {
        expected.emplace(item.group, item.processor);
    }
    for (const auto& item : values) actual.emplace(item.group, item.processor);
    return actual.size() == values.size() && actual == expected;
}

[[nodiscard]] Domain::Error win32Error(
    const std::string_view operation,
    const DWORD error)
{
    std::string message{operation};
    message += " failed with Win32 error ";
    message += std::to_string(error);
    message += '.';
    if (error == ERROR_ACCESS_DENIED) {
        return Domain::makeError(Domain::ErrorCodes::Unauthorized, std::move(message));
    }
    if (error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED ||
        error == ERROR_PROC_NOT_FOUND) {
        return Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable, std::move(message));
    }
    return Domain::makeError(Domain::ErrorCodes::InternalFailure, std::move(message), true);
}

[[nodiscard]] std::uint64_t fileTimeValue(const FILETIME value) noexcept
{
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

template <typename ResultType>
[[nodiscard]] Domain::Result<ResultType> boundaryFailure(
    const std::string_view message) noexcept
{
    return Domain::Result<ResultType>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, std::string{message}));
}

} // namespace

namespace Detail {
namespace {

class WindowsCpuMetricsPlatform final : public ICpuMetricsPlatform {
public:
    [[nodiscard]] Domain::Result<CpuTimesObservation> querySystemTimes() noexcept override
    {
        FILETIME idle{}, kernel{}, user{};
        if (::GetSystemTimes(&idle, &kernel, &user) == FALSE) {
            return Domain::Result<CpuTimesObservation>::failure(
                win32Error("GetSystemTimes", ::GetLastError()));
        }
        return Domain::Result<CpuTimesObservation>::success(
            CpuTimesObservation{fileTimeValue(idle), fileTimeValue(kernel), fileTimeValue(user)});
    }

    [[nodiscard]] Domain::Result<CpuTopologyObservation> queryTopology() noexcept override
    {
        try {
            CpuTopologyObservation result;
            const WORD groupCount = ::GetActiveProcessorGroupCount();
            if (groupCount == 0U) {
                return Domain::Result<CpuTopologyObservation>::failure(
                    win32Error("GetActiveProcessorGroupCount", ::GetLastError()));
            }
            for (WORD group{}; group < groupCount; ++group) {
                const DWORD count = ::GetActiveProcessorCount(group);
                if (count == 0U || count > 64U) {
                    return Domain::Result<CpuTopologyObservation>::failure(
                        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                            "Windows returned an invalid processor group size."));
                }
                for (DWORD processor{}; processor < count; ++processor) {
                    result.logicalProcessors.push_back(LogicalProcessorIdentity{
                        static_cast<std::uint16_t>(group),
                        static_cast<std::uint16_t>(processor)});
                }
            }
            result.logicalProcessorCount = static_cast<std::uint32_t>(
                result.logicalProcessors.size());

            DWORD bytes{};
            ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
            if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0U) {
                return Domain::Result<CpuTopologyObservation>::failure(
                    win32Error("GetLogicalProcessorInformationEx(size)", ::GetLastError()));
            }
            std::vector<std::byte> buffer(bytes);
            if (::GetLogicalProcessorInformationEx(
                    RelationProcessorCore,
                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                    &bytes) == FALSE) {
                return Domain::Result<CpuTopologyObservation>::failure(
                    win32Error("GetLogicalProcessorInformationEx", ::GetLastError()));
            }
            std::size_t offset{};
            while (offset < bytes) {
                const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    buffer.data() + offset);
                if (entry->Size == 0U || offset + entry->Size > bytes) {
                    return Domain::Result<CpuTopologyObservation>::failure(
                        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                            "Windows returned malformed processor topology data."));
                }
                ++result.physicalCoreCount;
                offset += entry->Size;
            }
            return Domain::Result<CpuTopologyObservation>::success(std::move(result));
        } catch (...) {
            return boundaryFailure<CpuTopologyObservation>(
                "The Windows processor topology probe failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<std::string> queryBrand() noexcept override
    {
        try {
            wchar_t value[256]{};
            DWORD bytes = sizeof(value);
            const LSTATUS status = ::RegGetValueW(
                HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                L"ProcessorNameString",
                RRF_RT_REG_SZ,
                nullptr,
                value,
                &bytes);
            if (status != ERROR_SUCCESS) {
                return Domain::Result<std::string>::failure(
                    win32Error("RegGetValueW(ProcessorNameString)", static_cast<DWORD>(status)));
            }
            const int length = static_cast<int>(wcsnlen_s(value, std::size(value)));
            const int required = ::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
            if (required <= 0) {
                return Domain::Result<std::string>::failure(
                    win32Error("WideCharToMultiByte(ProcessorNameString)", ::GetLastError()));
            }
            std::string utf8(static_cast<std::size_t>(required), '\0');
            if (::WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                    utf8.data(), required, nullptr, nullptr) != required) {
                return boundaryFailure<std::string>("The processor brand conversion was incomplete.");
            }
            return Domain::Result<std::string>::success(std::move(utf8));
        } catch (...) {
            return boundaryFailure<std::string>("The processor brand probe failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<CpuPerformanceObservation>
    queryProcessorPerformance() noexcept override
    {
        CpuPerformanceObservation observation;
        observation.utilizationReady = false;
        observation.utilizationFailure = Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "Per-logical PDH processor utilization is unavailable on this host.");
        observation.aggregateFrequencyFailure = Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "The PDH aggregate Actual Frequency counter is unavailable on this host.");
        observation.perLogicalFrequencyFailure = Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "Per-logical PDH Actual Frequency counters are unavailable on this host.");
        return Domain::Result<CpuPerformanceObservation>::success(std::move(observation));
    }

    void shutdown() noexcept override {}
};

[[nodiscard]] Domain::Result<std::uint16_t> parseUnsigned16(
    const std::wstring_view text)
{
    if (text.empty()) {
        return Domain::Result<std::uint16_t>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "A PDH processor identity component is empty."));
    }
    std::uint32_t value{};
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return Domain::Result<std::uint16_t>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                    "A PDH processor identity component is not numeric."));
        }
        value = value * 10U + static_cast<std::uint32_t>(character - L'0');
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            return Domain::Result<std::uint16_t>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                    "A PDH processor identity component is out of range."));
        }
    }
    return Domain::Result<std::uint16_t>::success(static_cast<std::uint16_t>(value));
}

} // namespace

Domain::Result<PdhProcessorInstance> parsePdhProcessorInstanceName(
    const std::wstring_view name)
{
    if (name == L"_Total") {
        return Domain::Result<PdhProcessorInstance>::success(
            PdhProcessorInstance{PdhProcessorInstanceKind::SystemTotal, 0U, 0U});
    }
    const auto comma = name.find(L',');
    if (comma == std::wstring_view::npos || comma == 0U ||
        name.find(L',', comma + 1U) != std::wstring_view::npos) {
        return Domain::Result<PdhProcessorInstance>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "A PDH processor instance name is malformed."));
    }
    auto group = parseUnsigned16(name.substr(0U, comma));
    if (!group) return Domain::Result<PdhProcessorInstance>::failure(group.error());
    const auto processor = name.substr(comma + 1U);
    if (processor == L"_Total") {
        return Domain::Result<PdhProcessorInstance>::success(
            PdhProcessorInstance{PdhProcessorInstanceKind::GroupTotal, group.value(), 0U});
    }
    auto logical = parseUnsigned16(processor);
    if (!logical) return Domain::Result<PdhProcessorInstance>::failure(logical.error());
    return Domain::Result<PdhProcessorInstance>::success(
        PdhProcessorInstance{PdhProcessorInstanceKind::LogicalProcessor,
            group.value(), logical.value()});
}

std::shared_ptr<ICpuMetricsPlatform> createWindowsCpuMetricsPlatform()
{
    return std::make_shared<WindowsCpuMetricsPlatform>();
}

} // namespace Detail

class WindowsCpuMetricsCollector::Impl final {
public:
    Impl(Contracts::IClock& clock, std::shared_ptr<Detail::ICpuMetricsPlatform> platform)
        : clock_{clock}, lifecycle_{clock, "CPU telemetry collection"},
          platform_{std::move(platform)} {}

    [[nodiscard]] Domain::Result<Domain::CpuMetrics> collect(
        const Domain::OperationContext& context)
    {
        auto admission = lifecycle_.tryAcquire(context);
        if (!admission) return Domain::Result<Domain::CpuMetrics>::failure(admission.error());
        auto lease = std::move(admission).value();
        if (!platform_) {
            return Domain::Result<Domain::CpuMetrics>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                    "The CPU collector requires a Windows platform."));
        }

        auto times = platform_->querySystemTimes();
        auto topology = platform_->queryTopology();
        auto brand = platform_->queryBrand();
        auto performance = platform_->queryProcessorPerformance();
        auto ready = lifecycle_.checkpoint(context, "map CPU observations");
        if (!ready) return Domain::Result<Domain::CpuMetrics>::failure(ready.error());

        const auto observedAt = clock_.utcNow();
        Domain::CpuMetrics metrics;
        mapTimes(metrics, times, observedAt);
        const bool topologyValid = mapTopology(metrics, topology, observedAt);
        mapBrand(metrics, brand, observedAt);
        mapPerformance(metrics, performance, topology, topologyValid, observedAt);
        metrics.loadAverage = Domain::makeUnavailableTelemetryMetric<Domain::LoadAverage>(
            Availability::Unsupported, observedAt, std::string{LoadSource},
            "Windows does not expose a verified 1/5/15 load-average equivalent.");

        const auto valid = Domain::validateCpuMetrics(metrics);
        if (!valid) {
            return Domain::Result<Domain::CpuMetrics>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                    "The Windows CPU collector produced invalid metric state: " +
                    valid.error().message));
        }
        ready = lifecycle_.publish(context, "publish CPU metrics", [this, &metrics]() {
            previous_ = metrics;
        });
        if (!ready) return Domain::Result<Domain::CpuMetrics>::failure(ready.error());
        return Domain::Result<Domain::CpuMetrics>::success(std::move(metrics));
    }

    void shutdown()
    {
        lifecycle_.shutdownAndDrain();
        if (platform_) platform_->shutdown();
    }

private:
    void setTimeFailure(
        Domain::CpuMetrics& metrics,
        const Availability availability,
        const Domain::UtcTimePoint observedAt,
        const std::string& reason) const
    {
        metrics.percent = failedMetric(prior(previous_, &Domain::CpuMetrics::percent), availability, observedAt, AggregateSource, reason);
        metrics.userPercent = failedMetric(prior(previous_, &Domain::CpuMetrics::userPercent), availability, observedAt, AggregateSource, reason);
        metrics.systemPercent = failedMetric(prior(previous_, &Domain::CpuMetrics::systemPercent), availability, observedAt, AggregateSource, reason);
        metrics.idlePercent = failedMetric(prior(previous_, &Domain::CpuMetrics::idlePercent), availability, observedAt, AggregateSource, reason);
    }

    void mapTimes(
        Domain::CpuMetrics& metrics,
        const Domain::Result<Detail::CpuTimesObservation>& observation,
        const Domain::UtcTimePoint observedAt)
    {
        if (!observation) {
            setTimeFailure(metrics, availabilityFor(observation.error()), observedAt,
                reasonFor(observation.error(), "The system CPU time probe failed."));
            return;
        }
        const auto current = observation.value();
        if (!lastTimes_) {
            lastTimes_ = current;
            setTimeFailure(metrics, Availability::WarmingUp, observedAt,
                "A second GetSystemTimes sample is required for utilization deltas.");
            return;
        }
        const auto priorTimes = *lastTimes_;
        lastTimes_ = current;
        if (current.idleTime100Nanoseconds < priorTimes.idleTime100Nanoseconds ||
            current.kernelTime100Nanoseconds < priorTimes.kernelTime100Nanoseconds ||
            current.userTime100Nanoseconds < priorTimes.userTime100Nanoseconds) {
            setTimeFailure(metrics, Availability::TemporarilyUnavailable, observedAt,
                "GetSystemTimes counters regressed and were rebased.");
            return;
        }
        const auto idle = current.idleTime100Nanoseconds - priorTimes.idleTime100Nanoseconds;
        const auto kernel = current.kernelTime100Nanoseconds - priorTimes.kernelTime100Nanoseconds;
        const auto user = current.userTime100Nanoseconds - priorTimes.userTime100Nanoseconds;
        if (kernel > std::numeric_limits<std::uint64_t>::max() - user ||
            kernel < idle || kernel + user == 0U) {
            setTimeFailure(metrics, Availability::TemporarilyUnavailable, observedAt,
                "GetSystemTimes produced an invalid or zero-length delta.");
            return;
        }
        const auto total = kernel + user;
        const auto ratio = [total](const std::uint64_t value) {
            return static_cast<double>(static_cast<long double>(value) * 100.0L /
                static_cast<long double>(total));
        };
        metrics.percent = Domain::makeAvailableTelemetryMetric(ratio(total - idle), observedAt, std::string{AggregateSource});
        metrics.userPercent = Domain::makeAvailableTelemetryMetric(ratio(user), observedAt, std::string{AggregateSource});
        metrics.systemPercent = Domain::makeAvailableTelemetryMetric(ratio(kernel - idle), observedAt, std::string{AggregateSource});
        metrics.idlePercent = Domain::makeAvailableTelemetryMetric(ratio(idle), observedAt, std::string{AggregateSource});
    }

    bool mapTopology(
        Domain::CpuMetrics& metrics,
        const Domain::Result<Detail::CpuTopologyObservation>& observation,
        const Domain::UtcTimePoint observedAt) const
    {
        if (observation && validIdentitySet(observation.value())) {
            metrics.logicalProcessorCount = Domain::makeAvailableTelemetryMetric(
                observation.value().logicalProcessorCount, observedAt, std::string{TopologySource});
            metrics.physicalCoreCount = Domain::makeAvailableTelemetryMetric(
                observation.value().physicalCoreCount, observedAt, std::string{TopologySource});
            return true;
        }
        const auto availability = observation
            ? Availability::TemporarilyUnavailable : availabilityFor(observation.error());
        const auto reason = observation
            ? std::string{"Windows returned an incoherent processor topology."}
            : reasonFor(observation.error(), "The processor topology probe failed.");
        metrics.logicalProcessorCount = failedMetric(prior(previous_, &Domain::CpuMetrics::logicalProcessorCount), availability, observedAt, TopologySource, reason);
        metrics.physicalCoreCount = failedMetric(prior(previous_, &Domain::CpuMetrics::physicalCoreCount), availability, observedAt, TopologySource, reason);
        return false;
    }

    void mapBrand(
        Domain::CpuMetrics& metrics,
        const Domain::Result<std::string>& observation,
        const Domain::UtcTimePoint observedAt) const
    {
        if (observation && !observation.value().empty() &&
            observation.value().size() <= Domain::CpuBrandBytesMaximum &&
            Domain::isValidUtf8(observation.value()) &&
            observation.value().find('\0') == std::string::npos) {
            metrics.brand = Domain::makeAvailableTelemetryMetric(
                observation.value(), observedAt, std::string{BrandSource});
            return;
        }
        const auto availability = observation
            ? Availability::TemporarilyUnavailable : availabilityFor(observation.error());
        const auto reason = observation
            ? std::string{"Windows returned an invalid processor brand."}
            : reasonFor(observation.error(), "The processor brand probe failed.");
        metrics.brand = failedMetric(prior(previous_, &Domain::CpuMetrics::brand), availability, observedAt, BrandSource, reason);
    }

    void mapPerformance(
        Domain::CpuMetrics& metrics,
        const Domain::Result<Detail::CpuPerformanceObservation>& observation,
        const Domain::Result<Detail::CpuTopologyObservation>& topology,
        const bool topologyValid,
        const Domain::UtcTimePoint observedAt) const
    {
        if (!observation) {
            const auto availability = availabilityFor(observation.error());
            const auto reason = reasonFor(observation.error(), "The processor performance probe failed.");
            metrics.perLogicalProcessor = failedMetric(prior(previous_, &Domain::CpuMetrics::perLogicalProcessor), availability, observedAt, LogicalSource, reason);
            metrics.frequencyMhz = failedMetric(prior(previous_, &Domain::CpuMetrics::frequencyMhz), availability, observedAt, FrequencySource, reason);
            metrics.perCoreFrequencyMhz = failedMetric(prior(previous_, &Domain::CpuMetrics::perCoreFrequencyMhz), availability, observedAt, FrequencySource, reason);
            return;
        }
        const auto& value = observation.value();
        if (!value.utilizationReady && !value.utilizationFailure) {
            metrics.perLogicalProcessor = failedMetric(prior(previous_, &Domain::CpuMetrics::perLogicalProcessor), Availability::WarmingUp, observedAt, LogicalSource, "A second PDH sample is required for per-logical utilization.");
        } else if (value.utilizationFailure) {
            metrics.perLogicalProcessor = failedMetric(prior(previous_, &Domain::CpuMetrics::perLogicalProcessor), availabilityFor(*value.utilizationFailure), observedAt, LogicalSource, reasonFor(*value.utilizationFailure, "Per-logical utilization failed."));
        } else if (topologyValid && sameIdentities(value.perLogicalUtilization, topology.value()) &&
            std::all_of(value.perLogicalUtilization.begin(), value.perLogicalUtilization.end(), [](const auto& item) { return std::isfinite(item.percent) && item.percent >= 0.0 && item.percent <= 100.0; })) {
            auto sorted = value.perLogicalUtilization;
            std::ranges::sort(sorted, {}, [](const auto& item) { return std::pair{item.group, item.processor}; });
            std::vector<double> mapped;
            mapped.reserve(sorted.size());
            for (const auto& item : sorted) mapped.push_back(item.percent);
            metrics.perLogicalProcessor = Domain::makeAvailableTelemetryMetric(std::move(mapped), observedAt, std::string{LogicalSource});
        } else {
            metrics.perLogicalProcessor = failedMetric(prior(previous_, &Domain::CpuMetrics::perLogicalProcessor), Availability::TemporarilyUnavailable, observedAt, LogicalSource, "Per-logical utilization identities or values were invalid.");
        }

        if (value.aggregateFrequencyMhz && std::isfinite(*value.aggregateFrequencyMhz) &&
            *value.aggregateFrequencyMhz > 0.0 &&
            *value.aggregateFrequencyMhz <= std::numeric_limits<std::uint32_t>::max()) {
            metrics.frequencyMhz = Domain::makeAvailableTelemetryMetric(
                static_cast<std::uint32_t>(std::llround(*value.aggregateFrequencyMhz)), observedAt, std::string{FrequencySource});
        } else {
            const auto availability = value.aggregateFrequencyFailure
                ? availabilityFor(*value.aggregateFrequencyFailure) : Availability::TemporarilyUnavailable;
            const auto reason = value.aggregateFrequencyFailure
                ? reasonFor(*value.aggregateFrequencyFailure, "Aggregate frequency failed.")
                : std::string{"The aggregate Actual Frequency value was absent or invalid."};
            metrics.frequencyMhz = failedMetric(prior(previous_, &Domain::CpuMetrics::frequencyMhz), availability, observedAt, FrequencySource, reason);
        }

        if (value.perLogicalFrequencyFailure) {
            metrics.perCoreFrequencyMhz = failedMetric(prior(previous_, &Domain::CpuMetrics::perCoreFrequencyMhz), availabilityFor(*value.perLogicalFrequencyFailure), observedAt, FrequencySource, reasonFor(*value.perLogicalFrequencyFailure, "Per-logical frequency failed."));
        } else if (topologyValid && sameIdentities(value.perLogicalFrequencyMhz, topology.value()) &&
            std::all_of(value.perLogicalFrequencyMhz.begin(), value.perLogicalFrequencyMhz.end(), [](const auto& item) { return std::isfinite(item.megahertz) && item.megahertz > 0.0 && item.megahertz <= std::numeric_limits<std::uint32_t>::max(); })) {
            auto sorted = value.perLogicalFrequencyMhz;
            std::ranges::sort(sorted, {}, [](const auto& item) { return std::pair{item.group, item.processor}; });
            std::vector<std::uint32_t> mapped;
            mapped.reserve(sorted.size());
            for (const auto& item : sorted) mapped.push_back(static_cast<std::uint32_t>(std::llround(item.megahertz)));
            metrics.perCoreFrequencyMhz = Domain::makeAvailableTelemetryMetric(std::move(mapped), observedAt, std::string{FrequencySource});
        } else {
            metrics.perCoreFrequencyMhz = failedMetric(prior(previous_, &Domain::CpuMetrics::perCoreFrequencyMhz), Availability::TemporarilyUnavailable, observedAt, FrequencySource, "Per-logical frequency identities or values were invalid.");
        }
    }

    Contracts::IClock& clock_;
    Detail::SynchronousCollectorGate lifecycle_;
    std::shared_ptr<Detail::ICpuMetricsPlatform> platform_;
    std::optional<Detail::CpuTimesObservation> lastTimes_;
    std::optional<Domain::CpuMetrics> previous_;
};

WindowsCpuMetricsCollector::WindowsCpuMetricsCollector(Contracts::IClock& clock)
    : WindowsCpuMetricsCollector(clock, Detail::createWindowsCpuMetricsPlatform()) {}

WindowsCpuMetricsCollector::WindowsCpuMetricsCollector(
    Contracts::IClock& clock,
    std::shared_ptr<Detail::ICpuMetricsPlatform> platform)
    : implementation_{std::make_shared<Impl>(clock, std::move(platform))} {}

WindowsCpuMetricsCollector::~WindowsCpuMetricsCollector()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        try { implementation->shutdown(); } catch (...) {}
    }
}

Domain::Result<Domain::CpuMetrics> WindowsCpuMetricsCollector::collect(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->collect(context)
            : boundaryFailure<Domain::CpuMetrics>("The Windows CPU collector is unavailable.");
    } catch (...) {
        return boundaryFailure<Domain::CpuMetrics>("The Windows CPU collector failed safely.");
    }
}

void WindowsCpuMetricsCollector::shutdown() noexcept
{
    try {
        const auto implementation = implementation_;
        if (implementation) implementation->shutdown();
    } catch (...) {}
}

std::unique_ptr<Contracts::ICpuMetricsCollector>
createWindowsCpuMetricsCollector(Contracts::IClock& clock)
{
    return std::make_unique<WindowsCpuMetricsCollector>(clock);
}

} // namespace ForgeConductor::Telemetry::Windows
