#include "ForgeConductor/Domain/ManagerModels.h"

#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] bool isLoopback(const std::string_view host) noexcept
{
    return host == "127.0.0.1" || host == "::1";
}

} // namespace

Result<void> validateManagerSettings(const ManagerSettings& settings)
{
    if (!isLoopback(settings.dashboardHost) || settings.dashboardPort == 0 ||
        settings.dashboardRefreshInterval < std::chrono::seconds{2} ||
        settings.dashboardRefreshInterval > std::chrono::seconds{300} ||
        settings.watchdogInterval < std::chrono::seconds{1} ||
        settings.watchdogInterval > std::chrono::seconds{60} ||
        settings.sessionIdleTtl < std::chrono::seconds{60} ||
        settings.shellTimeout <= std::chrono::seconds::zero() ||
        settings.shellTimeout > std::chrono::seconds{120}) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Manager settings require loopback binding, dashboard refresh "
            "within 2 through 300 seconds, watchdog within 1 through 60 "
            "seconds, session idle TTL of at least 60 seconds, and shell "
            "timeout within 1 through 120 seconds."));
    }
    return Result<void>::success();
}

Result<ManagerSettings> applyManagerSettingsPatch(
    ManagerSettings settings,
    const ManagerSettingsPatch& patch)
{
    if (patch.dashboardHost) settings.dashboardHost = *patch.dashboardHost;
    if (patch.dashboardPort) settings.dashboardPort = *patch.dashboardPort;
    if (patch.dashboardRefreshInterval) {
        settings.dashboardRefreshInterval = *patch.dashboardRefreshInterval;
    }
    if (patch.autoRestart) settings.autoRestart = *patch.autoRestart;
    if (patch.watchdogInterval) settings.watchdogInterval = *patch.watchdogInterval;
    if (patch.openBrowserOnStart) settings.openBrowserOnStart = *patch.openBrowserOnStart;
    if (patch.sessionIdleTtl) settings.sessionIdleTtl = *patch.sessionIdleTtl;
    if (patch.shellTimeout) settings.shellTimeout = *patch.shellTimeout;
    if (patch.logLevel) settings.logLevel = *patch.logLevel;
    auto validated = validateManagerSettings(settings);
    if (!validated) {
        return Result<ManagerSettings>::failure(std::move(validated).error());
    }
    return Result<ManagerSettings>::success(std::move(settings));
}

} // namespace ForgeConductor::Domain
