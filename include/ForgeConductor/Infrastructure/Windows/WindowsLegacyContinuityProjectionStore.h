#pragma once

#include "ForgeConductor/Contracts/IContinuityProjectionStore.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

// Atomic, human-readable projection of the authoritative central continuity
// rows. Callers inject one authority rooted at memoryRoot and the existing
// storage abstractions; this class never manufactures filesystem authority.
class WindowsLegacyContinuityProjectionStore final
    : public Contracts::IContinuityProjectionStore {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsLegacyContinuityProjectionStore>> create(
        Domain::PathText memoryRoot,
        Domain::PathText handoffsRoot,
        Contracts::WorkspaceAuthority authority,
        std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
        std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
        std::shared_ptr<Contracts::IFileSystem> fileSystem,
        std::shared_ptr<Contracts::IClock> clock) noexcept;

    ~WindowsLegacyContinuityProjectionStore() noexcept override;

    WindowsLegacyContinuityProjectionStore(
        const WindowsLegacyContinuityProjectionStore&) = delete;
    WindowsLegacyContinuityProjectionStore& operator=(
        const WindowsLegacyContinuityProjectionStore&) = delete;
    WindowsLegacyContinuityProjectionStore(
        WindowsLegacyContinuityProjectionStore&&) = delete;
    WindowsLegacyContinuityProjectionStore& operator=(
        WindowsLegacyContinuityProjectionStore&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityProjectionReceipt>
    write(
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        Domain::LegacyContinuityProjectionRepairOutcome> repair(
        const std::vector<Domain::LegacyContinuityRecord>& records,
        const Domain::LegacyContinuityPointerRepairOutcome& pointers,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::size_t> reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsLegacyContinuityProjectionStore(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
