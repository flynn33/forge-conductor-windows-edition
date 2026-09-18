#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IManagerRuntime.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"
#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/IClientPresenceRepository.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/ITelemetryService.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include "ForgeConductor/Dashboard/IDashboardOperationalService.h"
#include "ForgeConductor/Manager/ManagerProtocolCodec.h"
#include "ForgeConductor/Manager/ManagerTransportLimits.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace ForgeConductor::Manager {

struct ManagerTelemetrySources final {
    Contracts::ITelemetryService* telemetry{};
    Dashboard::IDashboardOperationalService* operational{};
    Contracts::IProjectRegistryRepository* projects{};
    Contracts::IProjectMemoryService* projectMemory{};
    Contracts::IToolCatalog* tools{};
    Contracts::ILMStudioDeploymentService* lmStudioDeployment{};
    const Contracts::WorkspaceAuthority* lmStudioReadAuthority{};
    const Contracts::WorkspaceAuthority* lmStudioWriteAuthority{};
    Contracts::IToolAuthorizer* toolAuthorizer{};
    Contracts::IWorkspaceAuthority* projectWorkspaceAuthority{};
    Contracts::IToolRouter* toolRouter{};
    Contracts::IContinuityAutomation* continuityAutomation{};
    std::optional<Domain::PathText> preferredForgeBinary;
    bool shellEnabled{};
    Contracts::IContinuityCoordinator* continuity{};
    Contracts::IDiagnosticSink* diagnostics{};
    const Contracts::WorkspaceAuthority* lmStudioActivationAuthority{};
    Contracts::IClientPresenceRepository* clientPresence{};
    Contracts::IAuditRepository* audit{};
    Contracts::IManagedRunStore* durableManagedRunStore{};
    Contracts::IHasher* evidenceHasher{};
};

class ManagerRequestDispatcher final {
public:
    ManagerRequestDispatcher(
        std::shared_ptr<Contracts::IManagerController> controller,
        std::shared_ptr<Contracts::IClock> clock,
        ManagerTransportLimits limits = {},
        std::shared_ptr<Contracts::IManagedRunService> managedRuns = {},
        ManagerTelemetrySources telemetrySources = {});
    ~ManagerRequestDispatcher() noexcept;

    ManagerRequestDispatcher(const ManagerRequestDispatcher&) = delete;
    ManagerRequestDispatcher& operator=(const ManagerRequestDispatcher&) = delete;
    ManagerRequestDispatcher(ManagerRequestDispatcher&&) = delete;
    ManagerRequestDispatcher& operator=(ManagerRequestDispatcher&&) = delete;

    [[nodiscard]] ManagerResponse dispatch(const ManagerRequest& request) noexcept;

    // Stops regular admission and requests cancellation for every admitted
    // operation. Cancellation and shutdown control requests bypass admission.
    void beginShutdown() noexcept;
    void cancel(const Domain::OperationId& operationId) noexcept;

    [[nodiscard]] bool waitUntilIdle(
        std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] std::size_t activeOperationCount() const noexcept;
    [[nodiscard]] bool isAccepting() const noexcept;

    // Idempotently closes admission, drains for the configured bounded interval,
    // and closes the injected controller after no dispatcher callback is active.
    void shutdown() noexcept;

private:
    class Implementation;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace ForgeConductor::Manager
