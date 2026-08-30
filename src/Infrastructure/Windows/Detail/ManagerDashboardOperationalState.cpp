#include "ManagerDashboardOperationalState.h"

namespace ForgeConductor::Infrastructure::Windows::Detail {

ManagerDashboardOperationalState::ManagerDashboardOperationalState(
    const bool operationalServiceActive) noexcept
    : operationalServiceActive_{operationalServiceActive}
{
}

void ManagerDashboardOperationalState::setOperationalServiceActive(
    const bool active) noexcept
{
    operationalServiceActive_.store(active, std::memory_order_release);
}

bool ManagerDashboardOperationalState::operationalServiceActive()
    const noexcept
{
    return operationalServiceActive_.load(std::memory_order_acquire);
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
