#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

enum class LMStudioConnectorRole { Primary, Fallback };
enum class LMStudioConnectionState { Ready, PrimaryOnly, FallbackPromoted, Unavailable };

struct LMStudioConnectorHealth final {
    LMStudioConnectorRole role{LMStudioConnectorRole::Primary};
    bool ready{};
    std::optional<std::string> protocolVersion;
    std::size_t toolCount{};
    std::string detail;
};

struct LMStudioConnectionHealth final {
    std::vector<LMStudioConnectorHealth> roles;
    LMStudioConnectionState state{LMStudioConnectionState::Unavailable};
};

struct LMStudioEnvironmentStatus final {
    bool lmStudioPresent{};
    std::optional<PathText> installationRoot;
    std::optional<PathText> configurationPath;
    std::optional<std::string> version;
};

struct LMStudioPluginStatus final {
    bool primaryPluginInstalled{};
    bool fallbackPluginInstalled{};
    bool mcpConfigurationRegistered{};
    PathText binaryPath;
    bool binaryExecutable{};
    bool lmStudioPresent{};
    PathText primaryPluginPath;
    PathText fallbackPluginPath;
    PathText mcpConfigurationPath;
    std::optional<DeploymentId> deploymentId;
    std::string detail;
};

struct LMStudioDeploymentRequest final {
    std::optional<PathText> preferredBinary;
    bool preserveForeignEntries{true};
};

struct LMStudioInstallResult final {
    bool ok{};
    PathText binaryPath;
    std::vector<PathText> pluginsWritten;
    PathText mcpConfigurationPath;
    DeploymentId deploymentId;
    std::string message;
};

struct LMStudioHostActivationRequest final {
    DeploymentId deploymentId;
    std::chrono::milliseconds timeout;
};

struct LMStudioHostActivationResult final {
    DeploymentId deploymentId;
    bool runningBeforeDeploy{};
    bool launched{};
    bool restarted{};
    bool configurationSynchronized{};
    std::vector<LMStudioConnectorRole> readyRoles;
    std::string detail;
};

[[nodiscard]] LMStudioConnectionState deriveLMStudioConnectionState(
    const std::vector<LMStudioConnectorHealth>& roles) noexcept;

} // namespace ForgeConductor::Domain
