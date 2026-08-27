#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsProjectWorkspaceAuthority final
    : public Contracts::IWorkspaceAuthority {
public:
    static constexpr std::size_t MaximumProjects = 1'024U;

    WindowsProjectWorkspaceAuthority(
        Contracts::IProjectRegistryRepository& projectRegistry,
        Contracts::IUuidGenerator& uuidGenerator,
        Domain::ClientId serveClientId,
        bool shellEnabled) noexcept;

    WindowsProjectWorkspaceAuthority(const WindowsProjectWorkspaceAuthority&) = delete;
    WindowsProjectWorkspaceAuthority& operator=(
        const WindowsProjectWorkspaceAuthority&) = delete;
    WindowsProjectWorkspaceAuthority(WindowsProjectWorkspaceAuthority&&) = delete;
    WindowsProjectWorkspaceAuthority& operator=(WindowsProjectWorkspaceAuthority&&) = delete;

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& trustedRoots,
        const std::vector<Domain::FileAccess>& grants,
        bool shellEnabled,
        std::uint64_t generation,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept override;

private:
    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> currentDescriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<Domain::AuthorityId> cachedAuthorityId(
        const Contracts::WorkspaceAuthority& authority) noexcept;

    Contracts::IProjectRegistryRepository& projectRegistry_;
    Contracts::IUuidGenerator& uuidGenerator_;
    const Domain::ClientId serveClientId_;
    const bool shellEnabled_;
    std::mutex authorityIdsMutex_;
    std::map<Domain::ProjectId, Domain::AuthorityId> authorityIds_;
};

} // namespace ForgeConductor::Infrastructure::Windows
