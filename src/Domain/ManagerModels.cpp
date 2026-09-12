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
    auto provider = defaultAppConfig();
    provider.localModel.host = settings.localModelHost;
    provider.localModel.port = settings.localModelPort;
    provider.localModel.secure = settings.localModelSecure;
    provider.localModel.model = settings.localModelName.empty()
        ? std::nullopt
        : std::optional<std::string>{settings.localModelName};
    provider.localModel.effectiveContextCapacity =
        settings.effectiveContextCapacity;
    provider.localModel.nextResponseReserve = settings.nextResponseReserve;
    provider.localModel.handoffReserve = settings.handoffReserve;
    provider.localModel.estimationSafetyMargin =
        settings.estimationSafetyMargin;
    auto validProvider = validateAppConfig(provider);
    if (!validProvider) {
        return Result<void>::failure(makeError(
            validProvider.error().code,
            "Manager provider settings are invalid: " +
                validProvider.error().message));
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
    if (patch.localModelHost) settings.localModelHost = *patch.localModelHost;
    if (patch.localModelPort) settings.localModelPort = *patch.localModelPort;
    if (patch.localModelSecure) settings.localModelSecure = *patch.localModelSecure;
    if (patch.localModelName) settings.localModelName = *patch.localModelName;
    if (patch.effectiveContextCapacity) {
        settings.effectiveContextCapacity = *patch.effectiveContextCapacity;
    }
    if (patch.nextResponseReserve) {
        settings.nextResponseReserve = *patch.nextResponseReserve;
    }
    if (patch.handoffReserve) settings.handoffReserve = *patch.handoffReserve;
    if (patch.estimationSafetyMargin) {
        settings.estimationSafetyMargin = *patch.estimationSafetyMargin;
    }
    if (patch.shellEnabled) settings.shellEnabled = *patch.shellEnabled;
    auto validated = validateManagerSettings(settings);
    if (!validated) {
        return Result<ManagerSettings>::failure(std::move(validated).error());
    }
    return Result<ManagerSettings>::success(std::move(settings));
}

} // namespace ForgeConductor::Domain
