#pragma once

#include "DashboardConnectionRuntimeServices.h"

#include <atomic>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Process-composition-owned source for the operational API admission bit.
class ManagerDashboardOperationalState final
    : public IDashboardOperationalStateSource {
public:
    explicit ManagerDashboardOperationalState(
        bool operationalServiceActive = false) noexcept;

    ~ManagerDashboardOperationalState() noexcept override = default;

    ManagerDashboardOperationalState(
        const ManagerDashboardOperationalState&) = delete;
    ManagerDashboardOperationalState& operator=(
        const ManagerDashboardOperationalState&) = delete;
    ManagerDashboardOperationalState(
        ManagerDashboardOperationalState&&) = delete;
    ManagerDashboardOperationalState& operator=(
        ManagerDashboardOperationalState&&) = delete;

    void setOperationalServiceActive(bool active) noexcept;

    [[nodiscard]] bool operationalServiceActive() const noexcept override;

private:
    std::atomic_bool operationalServiceActive_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
