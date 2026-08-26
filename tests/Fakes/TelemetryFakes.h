#pragma once

#include "BoundedLatestValueMailboxFake.h"
#include "DeterministicResult.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

template <typename Value, typename Interface>
class RecordingTelemetryCollector final : public Interface {
public:
    DeterministicResult<Value> collectResult;

    [[nodiscard]] Domain::Result<Value> collect(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++calls_;
            lastOperationId_ = context.operationId;
            if (shutdown_ || context.isCancellationRequested()) {
                return Domain::Result<Value>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic telemetry collection was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<Value>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic telemetry deadline expired."));
            }
            return collectResult.get();
        } catch (...) {
            return Domain::Result<Value>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic telemetry collection could not be recorded."));
        }
    }

    void shutdown() noexcept override
    {
        shutdown_ = true;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        now_ = now;
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastOperationId() const noexcept
    {
        return lastOperationId_;
    }

private:
    std::optional<Domain::OperationId> lastOperationId_;
    Domain::MonotonicTimePoint now_{};
    std::size_t calls_{};
    bool shutdown_{};
};

using CpuMetricsCollectorFake =
    RecordingTelemetryCollector<Domain::CpuMetrics, Contracts::ICpuMetricsCollector>;
using RamMetricsCollectorFake =
    RecordingTelemetryCollector<Domain::RamMetrics, Contracts::IRamMetricsCollector>;
using DiskVolumeCollectorFake = RecordingTelemetryCollector<
    std::vector<Domain::DiskVolume>,
    Contracts::IDiskVolumeCollector>;
using DiskIoMetricsCollectorFake = RecordingTelemetryCollector<
    Domain::DiskIoMetrics,
    Contracts::IDiskIoMetricsCollector>;
using GpuMetricsCollectorFake = RecordingTelemetryCollector<
    std::vector<Domain::GpuMetrics>,
    Contracts::IGpuMetricsCollector>;
using ProcessMetricsCollectorFake = RecordingTelemetryCollector<
    std::vector<Domain::ProcessMetrics>,
    Contracts::IProcessMetricsCollector>;
using PowerMetricsCollectorFake = RecordingTelemetryCollector<
    Domain::PowerMetrics,
    Contracts::IPowerMetricsCollector>;
using SystemMetricsCollectorFake = RecordingTelemetryCollector<
    Domain::SystemMetrics,
    Contracts::ISystemMetricsCollector>;
using ForgeMetricsCollectorFake = RecordingTelemetryCollector<
    Domain::ForgeSnapshot,
    Contracts::IForgeMetricsCollector>;

class RecordingTelemetryService final
    : public Contracts::ITelemetryService {
public:
    DeterministicResult<void> startResult;
    DeterministicResult<Snapshot> sampleResult;
    DeterministicResult<Domain::TelemetryHealthReport> healthResult;

    [[nodiscard]] Domain::Result<void> start(
        const Domain::OperationContext& context) noexcept override
    {
        ++startCalls_;
        auto result = finish(context, startResult, false);
        if (result) {
            running_ = true;
        }
        return result;
    }

    [[nodiscard]] Domain::Result<Snapshot> sample(
        const bool forceForgeComposition,
        const Domain::OperationContext& context) noexcept override
    {
        ++sampleCalls_;
        lastForceForgeComposition_ = forceForgeComposition;
        auto result = finish(context, sampleResult, true);
        if (result) {
            mailbox_.publish(result.value());
        }
        return result;
    }

    [[nodiscard]] Domain::Result<Domain::TelemetryHealthReport> health(
        const Domain::OperationContext& context) noexcept override
    {
        ++healthCalls_;
        return finish(context, healthResult, true);
    }

    [[nodiscard]] Domain::Result<void> setConsumer(
        Consumer consumer) noexcept override
    {
        return mailbox_.setConsumer(std::move(consumer));
    }

    [[nodiscard]] Snapshot latest() const noexcept override
    {
        return mailbox_.latest();
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return mailbox_.pendingCount();
    }

    void stop() noexcept override
    {
        running_ = false;
        mailbox_.shutdown();
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        now_ = now;
    }

    [[nodiscard]] bool drainOne() noexcept
    {
        return mailbox_.drainOne();
    }

    [[nodiscard]] std::size_t startCalls() const noexcept { return startCalls_; }
    [[nodiscard]] std::size_t sampleCalls() const noexcept { return sampleCalls_; }
    [[nodiscard]] std::size_t healthCalls() const noexcept { return healthCalls_; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] bool lastForceForgeComposition() const noexcept
    {
        return lastForceForgeComposition_;
    }

private:
    template <typename T>
    [[nodiscard]] Domain::Result<T> finish(
        const Domain::OperationContext& context,
        const DeterministicResult<T>& result,
        const bool requiresRunning) noexcept
    {
        try {
            lastOperationId_ = context.operationId;
            if (requiresRunning && !running_) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic telemetry service is not running."));
            }
            if (context.isCancellationRequested()) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic telemetry operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic telemetry deadline expired."));
            }
            return result.get();
        } catch (...) {
            return Domain::Result<T>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic telemetry call could not be recorded."));
        }
    }

    BoundedLatestValueMailboxFake<Domain::TelemetrySnapshot> mailbox_;
    std::optional<Domain::OperationId> lastOperationId_;
    Domain::MonotonicTimePoint now_{};
    std::size_t startCalls_{};
    std::size_t sampleCalls_{};
    std::size_t healthCalls_{};
    bool running_{};
    bool lastForceForgeComposition_{};
};

} // namespace ForgeConductor::Tests::Fakes
