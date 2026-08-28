#pragma once

#include "ForgeConductor/Contracts/IManagerServices.h"
#include "ForgeConductor/Dashboard/DashboardApplicationModels.h"
#include "ForgeConductor/Dashboard/IDashboardAssetStore.h"
#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Dashboard/IDashboardOperationalService.h"
#include "ForgeConductor/Dashboard/IDashboardTelemetrySource.h"
#include "ForgeConductor/Dashboard/DashboardRequestPolicy.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <cstdint>
#include <string>
#include <utility>

namespace ForgeConductor::Application {

// Transport-neutral application boundary for one parsed dashboard request.
// The composition root owns every injected service; this adapter owns only
// immutable listener-generation policy and identity snapshots.
class DashboardConnectionApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    DashboardConnectionApplication(
        Dashboard::DashboardRequestPolicy policy,
        Domain::ResourceBudgets budgets,
        Dashboard::DashboardApplicationIdentity identity,
        std::string dashboardHost,
        std::uint16_t dashboardPort,
        Dashboard::IDashboardAssetStore& assetStore,
        Dashboard::IDashboardTelemetrySource& telemetrySource,
        Dashboard::IDashboardOperationalService& operationalService,
        Contracts::IManagerClient& managerClient) noexcept
        : policy_{std::move(policy)},
          budgets_{budgets},
          identity_{std::move(identity)},
          dashboardHost_{std::move(dashboardHost)},
          dashboardPort_{dashboardPort},
          assetStore_{assetStore},
          telemetrySource_{telemetrySource},
          operationalService_{operationalService},
          managerClient_{managerClient}
    {
    }

    DashboardConnectionApplication(const DashboardConnectionApplication&) =
        delete;
    DashboardConnectionApplication& operator=(
        const DashboardConnectionApplication&) = delete;
    DashboardConnectionApplication(DashboardConnectionApplication&&) = delete;
    DashboardConnectionApplication& operator=(
        DashboardConnectionApplication&&) = delete;

    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange> prepare(
        Dashboard::DashboardHttpRequest request,
        bool operationalServiceActive,
        Domain::OperationContext context) noexcept override;

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction action,
        Domain::OperationContext context) noexcept override;

private:
    Dashboard::DashboardRequestPolicy policy_;
    Domain::ResourceBudgets budgets_;
    Dashboard::DashboardApplicationIdentity identity_;
    std::string dashboardHost_;
    std::uint16_t dashboardPort_{};
    Dashboard::IDashboardAssetStore& assetStore_;
    Dashboard::IDashboardTelemetrySource& telemetrySource_;
    Dashboard::IDashboardOperationalService& operationalService_;
    Contracts::IManagerClient& managerClient_;
};

} // namespace ForgeConductor::Application
