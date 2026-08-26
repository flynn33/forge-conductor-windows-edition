#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryArtifactStore.h"

#include <memory>

namespace ForgeConductor::Persistence::Windows {

// Owns the Windows filesystem boundary for project-memory snapshot artifacts.
// All file opens are relative to a retained, verified exports-directory handle.
class WindowsProjectMemoryArtifactStore final
    : public Contracts::IProjectMemoryArtifactStore {
public:
    WindowsProjectMemoryArtifactStore(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator);

    ~WindowsProjectMemoryArtifactStore() noexcept override;

    WindowsProjectMemoryArtifactStore(
        const WindowsProjectMemoryArtifactStore&) = delete;
    WindowsProjectMemoryArtifactStore& operator=(
        const WindowsProjectMemoryArtifactStore&) = delete;
    WindowsProjectMemoryArtifactStore(
        WindowsProjectMemoryArtifactStore&&) = delete;
    WindowsProjectMemoryArtifactStore& operator=(
        WindowsProjectMemoryArtifactStore&&) = delete;

    [[nodiscard]] Domain::Result<Domain::PathText> publish(
        const Domain::ProjectId& projectId,
        std::span<const std::byte> content,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryArtifactDocument> read(
        const Domain::ProjectId& projectId,
        const Domain::PathText& artifact,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::PathText> quarantineOversized(
        const Domain::ProjectId& projectId,
        const Domain::PathText& artifact,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::PathText> quarantineCorrupt(
        const Domain::ProjectId& projectId,
        const Domain::ProjectMemoryArtifactDocument& retainedDocument,
        const Domain::OperationContext& context) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
