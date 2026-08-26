#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace ForgeConductor::Contracts {

class IWorkspaceAuthority;

class WorkspaceAuthority final {
public:
    WorkspaceAuthority(const WorkspaceAuthority&) = default;
    WorkspaceAuthority(WorkspaceAuthority&&) = default;
    WorkspaceAuthority& operator=(const WorkspaceAuthority&) = delete;
    WorkspaceAuthority& operator=(WorkspaceAuthority&&) = delete;

    [[nodiscard]] const Domain::AuthorityId& authorityId() const noexcept
    {
        return authorityId_;
    }

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept
    {
        return projectId_;
    }

    [[nodiscard]] const Domain::ClientId& callerId() const noexcept
    {
        return callerId_;
    }

    [[nodiscard]] const std::vector<Domain::PathText>& trustedRoots() const noexcept
    {
        return trustedRoots_;
    }

    [[nodiscard]] Domain::FileAccess intent() const noexcept { return intent_; }

    [[nodiscard]] const std::vector<Domain::FileAccess>& grants() const noexcept
    {
        return grants_;
    }

    [[nodiscard]] const std::vector<Domain::FileAccess>& denials() const noexcept
    {
        return denials_;
    }

    [[nodiscard]] bool shellEnabled() const noexcept { return shellEnabled_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    friend class IWorkspaceAuthority;

    WorkspaceAuthority(
        Domain::AuthorityId authorityId,
        Domain::ProjectId projectId,
        Domain::ClientId callerId,
        std::vector<Domain::PathText> trustedRoots,
        const Domain::FileAccess intent,
        std::vector<Domain::FileAccess> grants,
        std::vector<Domain::FileAccess> denials,
        const bool shellEnabled,
        const std::uint64_t generation)
        : authorityId_{std::move(authorityId)},
          projectId_{std::move(projectId)},
          callerId_{std::move(callerId)},
          trustedRoots_{std::move(trustedRoots)},
          intent_{intent},
          grants_{std::move(grants)},
          denials_{std::move(denials)},
          shellEnabled_{shellEnabled},
          generation_{generation}
    {
    }

    const Domain::AuthorityId authorityId_;
    const Domain::ProjectId projectId_;
    const Domain::ClientId callerId_;
    const std::vector<Domain::PathText> trustedRoots_;
    const Domain::FileAccess intent_;
    const std::vector<Domain::FileAccess> grants_;
    const std::vector<Domain::FileAccess> denials_;
    const bool shellEnabled_;
    const std::uint64_t generation_;
};

class AuthorizedPath final {
public:
    AuthorizedPath(const AuthorizedPath&) = default;
    AuthorizedPath(AuthorizedPath&&) = default;
    AuthorizedPath& operator=(const AuthorizedPath&) = delete;
    AuthorizedPath& operator=(AuthorizedPath&&) = delete;

    [[nodiscard]] const Domain::AuthorityId& authorityId() const noexcept
    {
        return authorityId_;
    }

    [[nodiscard]] const Domain::PathText& canonicalPath() const noexcept
    {
        return canonicalPath_;
    }

    [[nodiscard]] const Domain::PathText& authorityRoot() const noexcept
    {
        return authorityRoot_;
    }

    [[nodiscard]] Domain::FileAccess access() const noexcept { return access_; }

private:
    friend class IWorkspaceAuthority;

    AuthorizedPath(
        Domain::AuthorityId authorityId,
        Domain::PathText canonicalPath,
        Domain::PathText authorityRoot,
        const Domain::FileAccess access)
        : authorityId_{std::move(authorityId)},
          canonicalPath_{std::move(canonicalPath)},
          authorityRoot_{std::move(authorityRoot)},
          access_{access}
    {
    }

    const Domain::AuthorityId authorityId_;
    const Domain::PathText canonicalPath_;
    const Domain::PathText authorityRoot_;
    const Domain::FileAccess access_;
};

} // namespace ForgeConductor::Contracts
