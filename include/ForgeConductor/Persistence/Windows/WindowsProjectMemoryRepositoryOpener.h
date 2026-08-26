#pragma once

#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"

#include <atomic>
#include <memory>

namespace ForgeConductor::Persistence::Windows {

class WindowsProjectMemoryRepositoryOpener final
    : public Contracts::IProjectMemoryRepositoryOpener {
public:
    WindowsProjectMemoryRepositoryOpener(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IRedactor> redactor,
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IClock> clock,
        WindowsProjectMemoryRepositoryOptions options);

    ~WindowsProjectMemoryRepositoryOpener() noexcept override;

    WindowsProjectMemoryRepositoryOpener(
        const WindowsProjectMemoryRepositoryOpener&) = delete;
    WindowsProjectMemoryRepositoryOpener& operator=(
        const WindowsProjectMemoryRepositoryOpener&) = delete;

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IProjectRepository>>
    openUncached(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    std::shared_ptr<Contracts::IApplicationPaths> applicationPaths_;
    std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore_;
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics_;
    std::shared_ptr<Contracts::IRedactor> redactor_;
    std::shared_ptr<Contracts::IHasher> hasher_;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    std::shared_ptr<Contracts::IClock> clock_;
    WindowsProjectMemoryRepositoryOptions options_;
    std::atomic_bool shutdownRequested_{};
};

} // namespace ForgeConductor::Persistence::Windows
