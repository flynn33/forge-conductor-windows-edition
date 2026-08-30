#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"

#include <cstdint>
#include <vector>

namespace ForgeConductor::Composition::Windows {

// Adapts the two independently issued Manager LM Studio capabilities to the
// single authority dependency required by the deployment service. Both
// issuers are borrowed and must outlive this router.
class ManagerLmStudioAuthorityRouter final
    : public Contracts::IWorkspaceAuthority {
public:
    ManagerLmStudioAuthorityRouter(
        Infrastructure::Windows::WindowsWorkspaceAuthority& readIssuer,
        Contracts::WorkspaceAuthority readAuthority,
        Infrastructure::Windows::WindowsWorkspaceAuthority& writeIssuer,
        Contracts::WorkspaceAuthority writeAuthority);
    ~ManagerLmStudioAuthorityRouter() noexcept override = default;

    ManagerLmStudioAuthorityRouter(
        const ManagerLmStudioAuthorityRouter&) = delete;
    ManagerLmStudioAuthorityRouter& operator=(
        const ManagerLmStudioAuthorityRouter&) = delete;
    ManagerLmStudioAuthorityRouter(
        ManagerLmStudioAuthorityRouter&&) = delete;
    ManagerLmStudioAuthorityRouter& operator=(
        ManagerLmStudioAuthorityRouter&&) = delete;

    [[nodiscard]] const Contracts::WorkspaceAuthority& readAuthority()
        const noexcept
    {
        return readAuthority_;
    }

    [[nodiscard]] const Contracts::WorkspaceAuthority& writeAuthority()
        const noexcept
    {
        return writeAuthority_;
    }

    // Both injected issuers intentionally bind the same maintenance project.
    // Issuing through the aggregate would therefore be ambiguous and is
    // rejected; callers issue each capability through its concrete owner.
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
    Infrastructure::Windows::WindowsWorkspaceAuthority& readIssuer_;
    Infrastructure::Windows::WindowsWorkspaceAuthority& writeIssuer_;
    const Contracts::WorkspaceAuthority readAuthority_;
    const Contracts::WorkspaceAuthority writeAuthority_;
};

} // namespace ForgeConductor::Composition::Windows
