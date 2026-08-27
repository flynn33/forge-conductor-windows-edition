#pragma once

#include <chrono>
#include <cstddef>

namespace ForgeConductor::Manager {

struct ManagerTransportLimits final {
    static constexpr std::size_t DefaultMaximumFrameBytes =
        2U * 1024U * 1024U;
    static constexpr auto DefaultMaximumRequestLifetime =
        std::chrono::minutes{5};
    static constexpr auto DefaultConnectTimeout = std::chrono::seconds{2};
    static constexpr auto DefaultShutdownDrainTimeout = std::chrono::seconds{5};
    static constexpr std::size_t DefaultMaximumConcurrentClientRequests = 16U;
    static constexpr std::size_t DefaultMaximumActiveRegularOperations = 3U;

    std::size_t maximumFrameBytes{DefaultMaximumFrameBytes};
    std::chrono::milliseconds maximumRequestLifetime{
        DefaultMaximumRequestLifetime};
    std::chrono::milliseconds connectTimeout{DefaultConnectTimeout};
    std::chrono::milliseconds shutdownDrainTimeout{
        DefaultShutdownDrainTimeout};
    std::size_t maximumConcurrentClientRequests{
        DefaultMaximumConcurrentClientRequests};
    std::size_t maximumActiveRegularOperations{
        DefaultMaximumActiveRegularOperations};
};

} // namespace ForgeConductor::Manager
