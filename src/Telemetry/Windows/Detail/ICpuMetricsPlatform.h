#pragma once

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Telemetry::Windows::Detail {

struct CpuTimesObservation final {
    std::uint64_t idleTime100Nanoseconds{};
    std::uint64_t kernelTime100Nanoseconds{};
    std::uint64_t userTime100Nanoseconds{};
};

struct LogicalProcessorIdentity final {
    std::uint16_t group{};
    std::uint16_t processor{};

    [[nodiscard]] bool operator==(
        const LogicalProcessorIdentity&) const noexcept = default;
};

struct CpuTopologyObservation final {
    std::uint32_t logicalProcessorCount{};
    std::uint32_t physicalCoreCount{};
    std::vector<LogicalProcessorIdentity> logicalProcessors;
};

struct LogicalProcessorUtilizationObservation final {
    std::uint16_t group{};
    std::uint16_t processor{};
    double percent{};
};

struct LogicalProcessorFrequencyObservation final {
    std::uint16_t group{};
    std::uint16_t processor{};
    double megahertz{};
};

enum class PdhProcessorInstanceKind {
    LogicalProcessor,
    GroupTotal,
    SystemTotal
};

struct PdhProcessorInstance final {
    PdhProcessorInstanceKind kind{PdhProcessorInstanceKind::LogicalProcessor};
    std::uint16_t group{};
    std::uint16_t processor{};
};

struct CpuPerformanceObservation final {
    bool utilizationReady{};
    std::vector<LogicalProcessorUtilizationObservation> perLogicalUtilization;
    std::optional<Domain::Error> utilizationFailure;
    std::optional<double> aggregateFrequencyMhz;
    std::optional<Domain::Error> aggregateFrequencyFailure;
    std::vector<LogicalProcessorFrequencyObservation> perLogicalFrequencyMhz;
    std::optional<Domain::Error> perLogicalFrequencyFailure;
};

[[nodiscard]] Domain::Result<PdhProcessorInstance>
parsePdhProcessorInstanceName(std::wstring_view name);

class ICpuMetricsPlatform {
public:
    virtual ~ICpuMetricsPlatform() = default;

    [[nodiscard]] virtual Domain::Result<CpuTimesObservation>
    querySystemTimes() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<CpuTopologyObservation>
    queryTopology() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::string>
    queryBrand() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<CpuPerformanceObservation>
    queryProcessorPerformance() noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

[[nodiscard]] std::shared_ptr<ICpuMetricsPlatform>
createWindowsCpuMetricsPlatform();

} // namespace ForgeConductor::Telemetry::Windows::Detail
