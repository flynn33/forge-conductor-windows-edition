#pragma once

#include "ForgeConductor/Domain/ManagerRuntimeModels.h"

#include <chrono>
#include <cstdint>

namespace ForgeConductor::Hosts::Manager {

enum class ManagerWatchdogAction : std::uint8_t {
    None,
    Repair,
};

// Pure policy for converting one atomic controller observation into at most
// one reconciliation request. The transition worker owns all scheduling.
class ManagerWatchdogPolicy final {
public:
    static constexpr std::chrono::seconds DefaultInterval{3};
    static constexpr std::chrono::seconds MinimumInterval{1};
    static constexpr std::chrono::seconds MaximumInterval{60};

    [[nodiscard]] static ManagerWatchdogAction decide(
        const Domain::ManagerControllerSnapshot& snapshot) noexcept;

    [[nodiscard]] static std::chrono::seconds normalizedInterval(
        const Domain::ManagerControllerSnapshot& snapshot) noexcept;

    [[nodiscard]] static std::chrono::seconds normalizedInterval(
        const Domain::ManagerStatus& status) noexcept;
};

} // namespace ForgeConductor::Hosts::Manager
