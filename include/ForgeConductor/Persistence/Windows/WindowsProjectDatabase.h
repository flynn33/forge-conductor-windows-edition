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

class WindowsProjectMemoryRepository;

struct WindowsProjectDatabaseOptions final {
    bool enableFts5{true};
};

class WindowsProjectDatabase final {
public:
    ~WindowsProjectDatabase() noexcept;

    WindowsProjectDatabase(const WindowsProjectDatabase&) = delete;
    WindowsProjectDatabase& operator=(const WindowsProjectDatabase&) = delete;
    WindowsProjectDatabase(WindowsProjectDatabase&&) = delete;
    WindowsProjectDatabase& operator=(WindowsProjectDatabase&&) = delete;

    [[nodiscard]] static Domain::Result<std::unique_ptr<WindowsProjectDatabase>> open(
        const Domain::ProjectId& projectId,
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IClock> clock,
        WindowsProjectDatabaseOptions options,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept;
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

    friend class WindowsProjectMemoryRepository;

    explicit WindowsProjectDatabase(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] Detail::WindowsDatabaseStore* repositoryStore() noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows

