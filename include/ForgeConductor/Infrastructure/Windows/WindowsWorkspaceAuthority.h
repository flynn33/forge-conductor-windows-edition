#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {

struct WindowsWorkspaceAuthorityPolicy final {
    Domain::AuthorityId authorityId;
    Domain::ProjectId projectId;
    Domain::ClientId callerId;
    std::vector<Domain::PathText> trustedRoots;
    Domain::FileAccess intent{Domain::FileAccess::Read};
    std::vector<Domain::FileAccess> grants;
    std::vector<Domain::FileAccess> denials;
    bool shellEnabled{};
    std::uint64_t generation{1U};
};

class WindowsWorkspaceAuthority final : public Contracts::IWorkspaceAuthority {
public:
    static constexpr std::size_t MaximumPolicies = 32U;
    static constexpr std::size_t MaximumTrustedRootsPerPolicy = 32U;

    explicit WindowsWorkspaceAuthority(
        std::vector<WindowsWorkspaceAuthorityPolicy> policies) noexcept;

    WindowsWorkspaceAuthority(const WindowsWorkspaceAuthority&) = delete;
    WindowsWorkspaceAuthority& operator=(const WindowsWorkspaceAuthority&) = delete;
    WindowsWorkspaceAuthority(WindowsWorkspaceAuthority&&) = delete;
    WindowsWorkspaceAuthority& operator=(WindowsWorkspaceAuthority&&) = delete;

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
    const Domain::Result<std::vector<WindowsWorkspaceAuthorityPolicy>> policies_;
};

} // namespace ForgeConductor::Infrastructure::Windows
