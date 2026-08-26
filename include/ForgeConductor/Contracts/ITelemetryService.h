#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/TelemetryModels.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace ForgeConductor::Contracts {

class ICpuMetricsCollector {
public:
    virtual ~ICpuMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<Domain::CpuMetrics> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IRamMetricsCollector {
public:
    virtual ~IRamMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<Domain::RamMetrics> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IDiskVolumeCollector {
public:
    virtual ~IDiskVolumeCollector() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::DiskVolume>> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IDiskIoMetricsCollector {
public:
    virtual ~IDiskIoMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<Domain::DiskIoMetrics> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IGpuMetricsCollector {
public:
    virtual ~IGpuMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::GpuMetrics>> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IProcessMetricsCollector {
public:
    virtual ~IProcessMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::ProcessMetrics>> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IPowerMetricsCollector {
public:
    virtual ~IPowerMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<Domain::PowerMetrics> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class ISystemMetricsCollector {
public:
    virtual ~ISystemMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<Domain::SystemMetrics> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IForgeMetricsCollector {
public:
    virtual ~IForgeMetricsCollector() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ForgeSnapshot> collect(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class ITelemetryService {
public:
    using Snapshot = std::shared_ptr<const Domain::TelemetrySnapshot>;
    using Consumer = std::function<void(Snapshot)>;

    virtual ~ITelemetryService() = default;

    [[nodiscard]] virtual Domain::Result<void> start(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Snapshot> sample(
        bool forceForgeComposition,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> setConsumer(
        Consumer consumer) noexcept = 0;

    [[nodiscard]] virtual Snapshot latest() const noexcept = 0;
    [[nodiscard]] virtual std::size_t pendingCount() const noexcept = 0;

    virtual void stop() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
