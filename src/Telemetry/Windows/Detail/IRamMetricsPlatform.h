#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <memory>

namespace ForgeConductor::Telemetry::Windows::Detail {

struct PhysicalMemoryObservation final {
    std::uint64_t totalBytes{};
    std::uint64_t availableBytes{};
};

struct PerformanceMemoryObservation final {
    std::uint64_t pageSizeBytes{};
    std::uint64_t committedPages{};
    std::uint64_t commitLimitPages{};
    std::uint64_t pagedPoolPages{};
};

class IRamMetricsPlatform {
public:
    virtual ~IRamMetricsPlatform() = default;

    [[nodiscard]] virtual Domain::Result<PhysicalMemoryObservation>
    queryPhysicalMemory() noexcept = 0;

    [[nodiscard]] virtual Domain::Result<PerformanceMemoryObservation>
    queryPerformanceMemory() noexcept = 0;
};

[[nodiscard]] std::shared_ptr<IRamMetricsPlatform>
createWindowsRamMetricsPlatform();

} // namespace ForgeConductor::Telemetry::Windows::Detail
