#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumAppConfigAllowedRootCount = 32U;

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

enum class McpRole {
    Primary,
    Fallback
};

struct ShellConfig final {
    bool enabled{};
    std::chrono::seconds defaultTimeout{30};

    bool operator==(const ShellConfig&) const = default;
};

struct DashboardConfig final {
    std::string host;
    std::uint16_t port{};
    std::chrono::seconds refreshInterval{8};

    bool operator==(const DashboardConfig&) const = default;
};

struct ManagerConfig final {
    bool autoRestart{true};
    std::chrono::seconds watchdogInterval{3};
    bool openBrowserOnStart{};

    bool operator==(const ManagerConfig&) const = default;
};

struct SessionConfig final {
    std::chrono::seconds idleTimeToLive{14'400};

    bool operator==(const SessionConfig&) const = default;
};

struct CoordinatorConfig final {
    bool enabled{true};
    std::chrono::seconds leaseTimeToLive{60};
    std::chrono::seconds presenceTimeToLive{30};

    bool operator==(const CoordinatorConfig&) const = default;
};

struct AppConfig final {
    LogLevel logLevel{LogLevel::Info};
    std::vector<PathText> allowedRoots;
    ShellConfig shell;
    DashboardConfig dashboard;
    ManagerConfig manager;
    McpRole mcpRole{McpRole::Primary};
    SessionConfig sessions;
    CoordinatorConfig coordinator;

    bool operator==(const AppConfig&) const = default;
};

struct AppConfigPatch final {
    std::optional<LogLevel> logLevel;
    std::optional<std::vector<PathText>> allowedRoots;
    std::optional<bool> shellEnabled;
    std::optional<std::chrono::seconds> shellTimeout;
    std::optional<std::string> dashboardHost;
    std::optional<std::uint16_t> dashboardPort;
    std::optional<std::chrono::seconds> dashboardRefreshInterval;
    std::optional<bool> managerAutoRestart;
    std::optional<std::chrono::seconds> managerWatchdogInterval;
    std::optional<bool> managerOpenBrowserOnStart;
    std::optional<McpRole> mcpRole;
    std::optional<std::chrono::seconds> sessionIdleTimeToLive;
    std::optional<bool> coordinatorEnabled;
    std::optional<std::chrono::seconds> coordinatorLeaseTimeToLive;
    std::optional<std::chrono::seconds> coordinatorPresenceTimeToLive;
};

[[nodiscard]] AppConfig defaultAppConfig();
[[nodiscard]] Result<void> validateAppConfig(const AppConfig& config);
[[nodiscard]] Result<AppConfig> applyConfigPatch(
    const AppConfig& config,
    const AppConfigPatch& patch);

[[nodiscard]] std::string_view wireName(LogLevel level) noexcept;
[[nodiscard]] std::string_view wireName(McpRole role) noexcept;

} // namespace ForgeConductor::Domain
