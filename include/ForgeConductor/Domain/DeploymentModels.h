#pragma once

#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

enum class DeploymentAction { Install, Repair, Upgrade, Uninstall, Purge };
enum class DeploymentState { NotInstalled, Installed, RepairRequired, Updating, Failed };

struct DeploymentRequest final {
    DeploymentAction action{DeploymentAction::Install};
    std::optional<std::string> requestedVersion;
    bool preserveUserData{true};
    std::optional<DestructiveConfirmation> confirmation;
};

struct DeploymentStatus final {
    DeploymentState state{DeploymentState::NotInstalled};
    std::optional<std::string> installedVersion;
    std::optional<PathText> installLocation;
    bool packageRegistered{};
    bool startupRegistered{};
    bool repairAvailable{};
};

struct DeploymentReport final {
    DeploymentAction action{DeploymentAction::Install};
    bool ok{};
    DeploymentStatus status;
    std::vector<std::string> operations;
    std::optional<std::string> rebootReason;
};

[[nodiscard]] Result<void> validateDeploymentRequest(
    const DeploymentRequest& request,
    std::string_view expectedPurgeScope,
    std::string_view expectedPurgeToken);

} // namespace ForgeConductor::Domain
