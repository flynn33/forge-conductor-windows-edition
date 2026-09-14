#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/Utf8.h"

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
        ShellConfig{true, std::chrono::seconds{30}},
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
            std::chrono::seconds{30}},
        LocalModelConfig{}};
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
    if (!isLoopbackHost(config.localModel.host) ||
        config.localModel.port == 0U) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Local model endpoint must use 127.0.0.1 or ::1 and a non-zero port."));
    }
    if (config.localModel.model &&
        (config.localModel.model->empty() ||
         config.localModel.model->size() > 256U ||
         config.localModel.model->find('\0') != std::string::npos ||
         !isValidUtf8(*config.localModel.model))) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Local model selection is invalid."));
    }
    const auto capacity = config.localModel.effectiveContextCapacity;
    const auto responseReserve = config.localModel.nextResponseReserve;
    const auto handoffReserve = config.localModel.handoffReserve;
    const auto margin = config.localModel.estimationSafetyMargin;
    if (capacity < 4'096U || capacity > 1'048'576U ||
        responseReserve == 0U || handoffReserve == 0U || margin == 0U ||
        static_cast<std::uint64_t>(responseReserve) + handoffReserve + margin >=
            capacity) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Local model context capacity and reserves are inconsistent."));
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
    if (patch.localModelHost) updated.localModel.host = *patch.localModelHost;
    if (patch.localModelPort) updated.localModel.port = *patch.localModelPort;
    if (patch.localModelSecure) updated.localModel.secure = *patch.localModelSecure;
    if (patch.localModelName) {
        updated.localModel.model = patch.localModelName->empty()
            ? std::nullopt
            : std::optional<std::string>{*patch.localModelName};
    }
    if (patch.effectiveContextCapacity) {
        updated.localModel.effectiveContextCapacity = *patch.effectiveContextCapacity;
    }
    if (patch.nextResponseReserve) {
        updated.localModel.nextResponseReserve = *patch.nextResponseReserve;
    }
    if (patch.handoffReserve) {
        updated.localModel.handoffReserve = *patch.handoffReserve;
    }
    if (patch.estimationSafetyMargin) {
        updated.localModel.estimationSafetyMargin = *patch.estimationSafetyMargin;
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
    switch (role) {
    case McpRole::Primary: return "primary";
    case McpRole::Fallback: return "fallback";
    case McpRole::Clu: return "clu";
    }
    return "primary";
}

} // namespace ForgeConductor::Domain
