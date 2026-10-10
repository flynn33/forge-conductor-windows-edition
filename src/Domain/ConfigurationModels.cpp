#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <utility>
#include <algorithm>
#include <charconv>

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

Result<void> validateImageProviderConfig(const ImageProviderConfig& config)
{
    const auto invalid = [] {
        return Result<void>::failure(makeError(ErrorCodes::InvalidRequest,
            "Image provider requires an explicit loopback HTTP(S) endpoint with port, sd1 profile, and safe .safetensors checkpoint basename."));
    };
    if (config.enabled && (config.endpoint.empty() || config.profile.empty() || config.checkpoint.empty()))
        return invalid();
    if (!config.endpoint.empty()) {
        auto endpoint = std::string_view{config.endpoint};
        if (endpoint.starts_with("http://")) endpoint.remove_prefix(7U);
        else if (endpoint.starts_with("https://")) endpoint.remove_prefix(8U);
        else return invalid();
        if (endpoint.starts_with("127.0.0.1:")) endpoint.remove_prefix(10U);
        else if (endpoint.starts_with("[::1]:")) endpoint.remove_prefix(6U);
        else return invalid();
        unsigned port{};
        const auto parsed = std::from_chars(endpoint.data(), endpoint.data() + endpoint.size(), port);
        if (endpoint.empty() || endpoint.size() > 5U || parsed.ec != std::errc{} ||
            parsed.ptr != endpoint.data() + endpoint.size() || port == 0U || port > 65535U ||
            std::to_string(port) != endpoint) return invalid();
    }
    if (!config.profile.empty() && config.profile != "sd1") return invalid();
    if (!config.checkpoint.empty() && (config.checkpoint.size() > 128U ||
        !config.checkpoint.ends_with(".safetensors") || config.checkpoint.starts_with('.') ||
        !std::all_of(config.checkpoint.begin(), config.checkpoint.end(), [](const unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        }))) return invalid();
    return Result<void>::success();
}

Result<void> validateComfyUiConfig(const ComfyUiConfig& config)
{
    ImageProviderConfig endpoint;
    endpoint.endpoint = config.endpoint;
    if (config.endpoint.empty() || !validateImageProviderConfig(endpoint)) {
        return Result<void>::failure(makeError(ErrorCodes::InvalidRequest,
            "ComfyUI endpoint must be an explicit loopback HTTP(S) address with port."));
    }
    const auto absoluteDirectory = [](const std::string& path) {
        if (path.empty()) return true;
        if (path.size() > PathText::MaximumBytes || path.find_first_of("\0\r\n", 0U, 3U) != std::string::npos || !isValidUtf8(path)) return false;
        const bool drive = path.size() >= 3U &&
            ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && (path[2] == '\\' || path[2] == '/');
        const auto serverEnd = path.find('\\', 2U);
        const bool unc = path.starts_with("\\\\") && !path.starts_with("\\\\?\\") && !path.starts_with("\\\\.\\") &&
            serverEnd != std::string::npos && serverEnd > 2U && serverEnd + 1U < path.size() && path[serverEnd + 1U] != '\\';
        return drive || unc;
    };
    if (!absoluteDirectory(config.installationPath) || !absoluteDirectory(config.modelStoragePath)) {
        return Result<void>::failure(makeError(ErrorCodes::InvalidRequest,
            "Configured ComfyUI installation and model storage must be absolute Windows directory paths."));
    }
    if (config.downloadBudgetBytes == 0U || config.downloadBudgetBytes > 10'000'000'000'000ULL ||
        config.freeSpaceReserveBytes > 10'000'000'000'000ULL ||
        config.generationTimeoutSeconds == 0U || config.generationTimeoutSeconds > 7'200U ||
        (config.qualityPreference != "balanced" && config.qualityPreference != "quality" && config.qualityPreference != "preview")) {
        return Result<void>::failure(makeError(ErrorCodes::InvalidRequest,
            "ComfyUI requires a download budget within 1 byte through 10 TB, a reserve up to 10 TB, "
            "a timeout within 1 through 7200 seconds, and balanced, quality or preview quality."));
    }
    return Result<void>::success();
}

Result<void> validateAppConfig(const AppConfig& config)
{
    if (auto valid = validateImageProviderConfig(config.imageProvider); !valid) return valid;
    if (auto valid = validateComfyUiConfig(config.comfyUi); !valid) return valid;
    if (config.fileSystemAccess != FileSystemAccessMode::Workspace &&
        config.fileSystemAccess != FileSystemAccessMode::Host) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Filesystem access must be workspace or host."));
    }
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
    if (patch.fileSystemAccess) updated.fileSystemAccess = *patch.fileSystemAccess;
    if (patch.imageProvider) updated.imageProvider = *patch.imageProvider;
    if (patch.comfyUi) updated.comfyUi = *patch.comfyUi;
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

std::string_view wireName(const FileSystemAccessMode mode) noexcept
{
    switch (mode) {
    case FileSystemAccessMode::Workspace: return "workspace";
    case FileSystemAccessMode::Host: return "host";
    }
    return "";
}

} // namespace ForgeConductor::Domain
