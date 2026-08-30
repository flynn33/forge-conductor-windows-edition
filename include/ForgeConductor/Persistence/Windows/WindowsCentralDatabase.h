#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Persistence/Windows/DatabaseModels.h"

#include <memory>

namespace ForgeConductor::Persistence::Windows {

namespace Detail {
class WindowsDatabaseStore;
}

class WindowsLegacyMemoryRepository;
class WindowsAgentSessionRepository;
class WindowsLegacyContinuityRepository;
class WindowsAuditRepository;
class WindowsForgeStatusRepository;
class WindowsClientPresenceRepository;
class WindowsDashboardOperationalRepository;

class WindowsCentralDatabase final {
public:
    ~WindowsCentralDatabase() noexcept;

    WindowsCentralDatabase(const WindowsCentralDatabase&) = delete;
    WindowsCentralDatabase& operator=(const WindowsCentralDatabase&) = delete;
    WindowsCentralDatabase(WindowsCentralDatabase&&) = delete;
    WindowsCentralDatabase& operator=(WindowsCentralDatabase&&) = delete;

    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsCentralDatabase>> open(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IClock> clock,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<DatabaseSchemaSnapshot> schemaSnapshot(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<DatabaseBackupReport> createOnlineBackup(
        const Domain::OperationContext& context) noexcept;
    [[nodiscard]] Domain::Result<void> close(
        const Domain::OperationContext& context) noexcept;

private:
    struct Impl;

    friend class WindowsLegacyMemoryRepository;
    friend class WindowsAgentSessionRepository;
    friend class WindowsLegacyContinuityRepository;
    friend class WindowsAuditRepository;
    friend class WindowsForgeStatusRepository;
    friend class WindowsClientPresenceRepository;
    friend class WindowsDashboardOperationalRepository;

    explicit WindowsCentralDatabase(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] Detail::WindowsDatabaseStore* repositoryStore() noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
