#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/ManagerModels.h"

#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] bool isLoopbackHost(const std::string_view host) noexcept
{
    return host == "127.0.0.1" || host == "::1";
}

[[nodiscard]] Result<void> requirePositive(
    const std::chrono::seconds value,
    const std::string_view name)
{
    if (value.count() <= 0) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            std::string{name} + " must be positive."));
    }
    return Result<void>::success();
}

} // namespace

AppConfig defaultAppConfig()
{
    return AppConfig{
        LogLevel::Info,
        {},
        ShellConfig{false, std::chrono::seconds{30}},
        DashboardConfig{
            "127.0.0.1",
            DefaultManagerDashboardPort,
            std::chrono::seconds{8}},
        ManagerConfig{true, std::chrono::seconds{3}, false},
        McpRole::Primary,
        SessionConfig{std::chrono::seconds{14'400}},
        CoordinatorConfig{
            true,
            std::chrono::seconds{60},
            std::chrono::seconds{30}}};
}

Result<void> validateAppConfig(const AppConfig& config)
{
    if (config.allowedRoots.size() > MaximumAppConfigAllowedRootCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Application configuration allowed roots exceed 32 entries."));
    }
    if (!isLoopbackHost(config.dashboard.host)) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Dashboard host must be 127.0.0.1 or ::1."));
    }
    if (config.dashboard.port == 0) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Dashboard port must be within 1 through 65535."));
    }
    if (config.shell.defaultTimeout.count() <= 0 ||
        config.shell.defaultTimeout > std::chrono::seconds{120}) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Shell timeout must be within 1 through 120 seconds."));
    }

    for (const auto [value, name] : {
             std::pair{config.dashboard.refreshInterval, "Dashboard refresh interval"},
             std::pair{config.manager.watchdogInterval, "Manager watchdog interval"},
             std::pair{config.sessions.idleTimeToLive, "Session idle TTL"},
             std::pair{config.coordinator.leaseTimeToLive, "Coordinator lease TTL"},
             std::pair{config.coordinator.presenceTimeToLive, "Coordinator presence TTL"}}) {
        auto valid = requirePositive(value, name);
        if (!valid) {
            return valid;
        }
    }
    return Result<void>::success();
}

Result<AppConfig> applyConfigPatch(const AppConfig& config, const AppConfigPatch& patch)
{
    AppConfig updated = config;
    if (patch.logLevel) updated.logLevel = *patch.logLevel;
    if (patch.allowedRoots) updated.allowedRoots = *patch.allowedRoots;
    if (patch.shellEnabled) updated.shell.enabled = *patch.shellEnabled;
    if (patch.shellTimeout) updated.shell.defaultTimeout = *patch.shellTimeout;
    if (patch.dashboardHost) updated.dashboard.host = *patch.dashboardHost;
    if (patch.dashboardPort) updated.dashboard.port = *patch.dashboardPort;
    if (patch.dashboardRefreshInterval) {
        updated.dashboard.refreshInterval = *patch.dashboardRefreshInterval;
    }
    if (patch.managerAutoRestart) updated.manager.autoRestart = *patch.managerAutoRestart;
    if (patch.managerWatchdogInterval) {
        updated.manager.watchdogInterval = *patch.managerWatchdogInterval;
    }
    if (patch.managerOpenBrowserOnStart) {
        updated.manager.openBrowserOnStart = *patch.managerOpenBrowserOnStart;
    }
    if (patch.mcpRole) updated.mcpRole = *patch.mcpRole;
    if (patch.sessionIdleTimeToLive) {
        updated.sessions.idleTimeToLive = *patch.sessionIdleTimeToLive;
    }
    if (patch.coordinatorEnabled) updated.coordinator.enabled = *patch.coordinatorEnabled;
    if (patch.coordinatorLeaseTimeToLive) {
        updated.coordinator.leaseTimeToLive = *patch.coordinatorLeaseTimeToLive;
    }
    if (patch.coordinatorPresenceTimeToLive) {
        updated.coordinator.presenceTimeToLive = *patch.coordinatorPresenceTimeToLive;
    }

    auto valid = validateAppConfig(updated);
    if (!valid) {
        return Result<AppConfig>::failure(std::move(valid).error());
    }
    return Result<AppConfig>::success(std::move(updated));
}

std::string_view wireName(const LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Critical: return "critical";
    }
    return "info";
}

std::string_view wireName(const McpRole role) noexcept
{
    return role == McpRole::Fallback ? "fallback" : "primary";
}

} // namespace ForgeConductor::Domain
