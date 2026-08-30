#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Contracts/IManagerMaintenanceService.h"
#include "ForgeConductor/Domain/FileSystemModels.h"

#include <atomic>
#include <string_view>

namespace ForgeConductor::Contracts {

class IAgentSessionService;
class IClock;
class IContinuityCoordinator;
class ILMStudioDeploymentService;
class IToolAuthorizer;
class IUuidGenerator;

} // namespace ForgeConductor::Contracts

namespace ForgeConductor::Composition::Windows {

struct ManagerMaintenanceServiceConfiguration final {
    Domain::PathText preferredForgeBinary;
    Contracts::WorkspaceAuthority lmStudioReadAuthority;
    Contracts::WorkspaceAuthority lmStudioWriteAuthority;
};

// One platform-neutral maintenance pass borrowed by the process-owned worker.
// The service serializes direct callers, performs each independent task even
// when an earlier task fails, and never creates its own thread, timer, queue,
// callback, or retry schedule.
class ManagerMaintenanceService final
    : public Contracts::IManagerMaintenanceService {
public:
    static constexpr std::string_view DeploymentToolName =
        "install-lmstudio-plugin";
    static constexpr std::string_view ProtocolVersion = "2025-11-25";
    static constexpr std::string_view CanonicalDeploymentArguments =
        "{\"preserve_foreign_entries\":true}";

    ManagerMaintenanceService(
        Contracts::IAgentSessionService& agentSessions,
        Contracts::IContinuityCoordinator& continuity,
        Contracts::ILMStudioDeploymentService& lmStudioDeployment,
        Contracts::IToolAuthorizer& toolAuthorizer,
        Contracts::IUuidGenerator& uuidGenerator,
        const Contracts::IClock& clock,
        ManagerMaintenanceServiceConfiguration configuration);

    ~ManagerMaintenanceService() noexcept override = default;

    ManagerMaintenanceService(const ManagerMaintenanceService&) = delete;
    ManagerMaintenanceService& operator=(const ManagerMaintenanceService&) =
        delete;
    ManagerMaintenanceService(ManagerMaintenanceService&&) = delete;
    ManagerMaintenanceService& operator=(ManagerMaintenanceService&&) =
        delete;

    [[nodiscard]] Domain::Result<void> reconcile(
        const Domain::OperationContext& context) noexcept override;

private:
    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context,
        std::string_view action) const noexcept;

    [[nodiscard]] Domain::Result<void> reconcileLmStudio(
        const Domain::OperationContext& context) noexcept;

    Contracts::IAgentSessionService& agentSessions_;
    Contracts::IContinuityCoordinator& continuity_;
    Contracts::ILMStudioDeploymentService& lmStudioDeployment_;
    Contracts::IToolAuthorizer& toolAuthorizer_;
    Contracts::IUuidGenerator& uuidGenerator_;
    const Contracts::IClock& clock_;
    const ManagerMaintenanceServiceConfiguration configuration_;
    std::atomic_flag operationAdmitted_ = ATOMIC_FLAG_INIT;
};

} // namespace ForgeConductor::Composition::Windows
