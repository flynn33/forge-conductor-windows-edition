#pragma once

#include "ForgeConductor/Contracts/IManagerServices.h"
#include "ForgeConductor/Dashboard/DashboardApplicationModels.h"
#include "ForgeConductor/Dashboard/IDashboardAssetStore.h"
#include "ForgeConductor/Dashboard/IDashboardConnectionApplicationFactory.h"
#include "ForgeConductor/Dashboard/IDashboardOperationalService.h"
#include "ForgeConductor/Dashboard/IDashboardTelemetrySource.h"
#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

namespace ForgeConductor::Application {

// Process-scoped application factory. Borrowed services must outlive every
// application returned by this object. The bearer token and identity are
// retained by value so each listener generation receives immutable snapshots.
class DashboardConnectionApplicationFactory final
    : public Dashboard::IDashboardConnectionApplicationFactory {
public:
    DashboardConnectionApplicationFactory(
        Domain::ResourceBudgets budgets,
        Dashboard::DashboardApplicationIdentity identity,
        Domain::Sha256Digest bearerToken,
        Dashboard::IDashboardAssetStore& assetStore,
        Dashboard::IDashboardTelemetrySource& telemetrySource,
        Dashboard::IDashboardOperationalService& operationalService,
        Contracts::IManagerClient& managerClient) noexcept;

    DashboardConnectionApplicationFactory(
        const DashboardConnectionApplicationFactory&) = delete;
    DashboardConnectionApplicationFactory& operator=(
        const DashboardConnectionApplicationFactory&) = delete;
    DashboardConnectionApplicationFactory(
        DashboardConnectionApplicationFactory&&) = delete;
    DashboardConnectionApplicationFactory& operator=(
        DashboardConnectionApplicationFactory&&) = delete;

    [[nodiscard]] Domain::Result<
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>>
    create(const Domain::DashboardConfig& configuration) noexcept override;

private:
    const Domain::ResourceBudgets budgets_;
    const Dashboard::DashboardApplicationIdentity identity_;
    const Domain::Sha256Digest bearerToken_;
    Dashboard::IDashboardAssetStore& assetStore_;
    Dashboard::IDashboardTelemetrySource& telemetrySource_;
    Dashboard::IDashboardOperationalService& operationalService_;
    Contracts::IManagerClient& managerClient_;
};

} // namespace ForgeConductor::Application
