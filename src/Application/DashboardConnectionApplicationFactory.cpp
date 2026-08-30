#include "ForgeConductor/Application/DashboardConnectionApplicationFactory.h"

#include "ForgeConductor/Application/DashboardConnectionApplication.h"
#include "ForgeConductor/Dashboard/DashboardRequestPolicy.h"
#include "ForgeConductor/Domain/Error.h"

#include <chrono>
#include <memory>
#include <utility>

namespace ForgeConductor::Application {

DashboardConnectionApplicationFactory::DashboardConnectionApplicationFactory(
    const Domain::ResourceBudgets budgets,
    Dashboard::DashboardApplicationIdentity identity,
    Domain::Sha256Digest bearerToken,
    Dashboard::IDashboardAssetStore& assetStore,
    Dashboard::IDashboardTelemetrySource& telemetrySource,
    Dashboard::IDashboardOperationalService& operationalService,
    Contracts::IManagerClient& managerClient) noexcept
    : budgets_{budgets},
      identity_{std::move(identity)},
      bearerToken_{std::move(bearerToken)},
      assetStore_{assetStore},
      telemetrySource_{telemetrySource},
      operationalService_{operationalService},
      managerClient_{managerClient}
{
}

Domain::Result<std::shared_ptr<Dashboard::IDashboardConnectionApplication>>
DashboardConnectionApplicationFactory::create(
    const Domain::DashboardConfig& configuration) noexcept
{
    try {
        if (configuration.refreshInterval <= std::chrono::seconds::zero()) {
            return Domain::Result<std::shared_ptr<
                Dashboard::IDashboardConnectionApplication>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard refresh interval must be positive."));
        }

        auto policy = Dashboard::DashboardRequestPolicy::create(
            configuration.host,
            configuration.port,
            bearerToken_);
        if (!policy) {
            return Domain::Result<std::shared_ptr<
                Dashboard::IDashboardConnectionApplication>>::failure(
                std::move(policy).error());
        }

        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application =
                std::make_shared<DashboardConnectionApplication>(
                    std::move(policy).value(),
                    budgets_,
                    identity_,
                    configuration.host,
                    configuration.port,
                    assetStore_,
                    telemetrySource_,
                    operationalService_,
                    managerClient_);
        return Domain::Result<std::shared_ptr<
            Dashboard::IDashboardConnectionApplication>>::success(
            std::move(application));
    } catch (...) {
        return Domain::Result<std::shared_ptr<
            Dashboard::IDashboardConnectionApplication>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard connection application construction failed."));
    }
}

} // namespace ForgeConductor::Application
