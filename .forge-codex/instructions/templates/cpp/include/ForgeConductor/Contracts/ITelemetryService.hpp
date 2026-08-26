#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forge
{
    template <typename T>
    struct Metric final
    {
        std::optional<T> value;
        bool stale{};
        std::string source;
        std::optional<std::string> unavailableReason;
    };

    struct TelemetrySnapshot final
    {
        std::chrono::system_clock::time_point capturedAt;
        Metric<double> cpuUtilizationPercent;
        Metric<double> gpuUtilizationPercent;
        Metric<std::uint64_t> ramUsedBytes;
        Metric<std::uint64_t> ramTotalBytes;
        Metric<std::uint64_t> gpuDedicatedUsedBytes;
        Metric<std::uint64_t> gpuDedicatedBudgetBytes;
    };

    class ITelemetryService
    {
    public:
        virtual ~ITelemetryService() = default;
        virtual void start() = 0;
        virtual void setConsumer(std::function<void(std::shared_ptr<TelemetrySnapshot const>)> consumer) = 0;
        virtual std::shared_ptr<TelemetrySnapshot const> latest() const = 0;
        virtual void stop() noexcept = 0;
    };
}
