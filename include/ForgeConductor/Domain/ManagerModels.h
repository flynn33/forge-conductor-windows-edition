#pragma once

#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ForgeConductor::Domain {

enum class ManagerServiceState { Stopped, Starting, Running, Restarting, Stopping, Failed };
enum class ManagerControlAction { Start, Stop, Restart, Repair };
inline constexpr std::uint16_t DefaultManagerDashboardPort = 7788;

struct ManagerStatus final {
    bool ok{};
    bool isManager{};
    ManagerServiceState state{ManagerServiceState::Stopped};
    bool desiredRunning{};
    bool httpListening{};
    bool serviceActive{};
    std::uint32_t processId{};
    std::optional<UtcTimePoint> startedAt;
    std::optional<std::chrono::seconds> uptime;
    std::uint32_t restartCount{};
    std::optional<std::string> lastError;
    bool autoRestart{true};
    std::chrono::seconds watchdogInterval{3};
    bool openBrowserOnStart{};
    std::string dashboardHost{"127.0.0.1"};
    std::uint16_t dashboardPort{DefaultManagerDashboardPort};
    std::chrono::seconds dashboardRefreshInterval{8};
    PathText home;
    std::string version;
};

struct ManagerSettings final {
    std::string dashboardHost{"127.0.0.1"};
    std::uint16_t dashboardPort{DefaultManagerDashboardPort};
    std::chrono::seconds dashboardRefreshInterval{8};
    bool autoRestart{true};
    std::chrono::seconds watchdogInterval{3};
    bool openBrowserOnStart{};
    std::chrono::seconds sessionIdleTtl{14'400};
    std::chrono::seconds shellTimeout{30};
    LogLevel logLevel{LogLevel::Info};
};

struct ManagerSettingsPatch final {
    std::optional<std::string> dashboardHost;
    std::optional<std::uint16_t> dashboardPort;
    std::optional<std::chrono::seconds> dashboardRefreshInterval;
    std::optional<bool> autoRestart;
    std::optional<std::chrono::seconds> watchdogInterval;
    std::optional<bool> openBrowserOnStart;
    std::optional<std::chrono::seconds> sessionIdleTtl;
    std::optional<std::chrono::seconds> shellTimeout;
    std::optional<LogLevel> logLevel;
};

struct ManagerControlRequest final { ManagerControlAction action{ManagerControlAction::Start}; };

[[nodiscard]] Result<void> validateManagerSettings(const ManagerSettings& settings);
[[nodiscard]] Result<ManagerSettings> applyManagerSettingsPatch(
    ManagerSettings settings,
    const ManagerSettingsPatch& patch);

} // namespace ForgeConductor::Domain
