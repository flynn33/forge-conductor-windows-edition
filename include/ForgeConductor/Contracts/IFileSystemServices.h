#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace ForgeConductor::Contracts {

class IApplicationPaths {
public:
    virtual ~IApplicationPaths() = default;

    [[nodiscard]] virtual Domain::Result<Domain::PathText> dataRoot(
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::PathText> configurationRoot(
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::PathText> diagnosticsRoot(
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::PathText> projectRoot(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;
};

class IWorkspaceAuthority {
public:
    virtual ~IWorkspaceAuthority() = default;

    [[nodiscard]] virtual Domain::Result<WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<WorkspaceAuthority> narrow(
        const WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& trustedRoots,
        const std::vector<Domain::FileAccess>& grants,
        bool shellEnabled,
        std::uint64_t generation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<AuthorizedPath> authorize(
        const WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

protected:
    [[nodiscard]] static Domain::Result<WorkspaceAuthority> issueAuthority(
        Domain::AuthorityId authorityId,
        Domain::ProjectId projectId,
        Domain::ClientId callerId,
        std::vector<Domain::PathText> trustedRoots,
        const Domain::FileAccess intent,
        std::vector<Domain::FileAccess> grants,
        std::vector<Domain::FileAccess> denials,
        const bool shellEnabled,
        const std::uint64_t generation)
    {
        if (trustedRoots.empty() ||
            !contains(grants, intent) || contains(denials, intent) ||
            std::any_of(grants.begin(), grants.end(), [&](const Domain::FileAccess access) {
                return contains(denials, access);
            })) {
            return Domain::Result<WorkspaceAuthority>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Workspace authority has invalid roots, intent, grants, or denials."));
        }
        return Domain::Result<WorkspaceAuthority>::success(WorkspaceAuthority{
            std::move(authorityId),
            std::move(projectId),
            std::move(callerId),
            std::move(trustedRoots),
            intent,
            std::move(grants),
            std::move(denials),
            shellEnabled,
            generation});
    }

    [[nodiscard]] static Domain::Result<WorkspaceAuthority> narrowAuthority(
        const WorkspaceAuthority& authority,
        std::vector<Domain::PathText> trustedRoots,
        std::vector<Domain::FileAccess> grants,
        const bool shellEnabled,
        const std::uint64_t generation)
    {
        if (generation <= authority.generation() ||
            (shellEnabled && !authority.shellEnabled()) ||
            trustedRoots.empty() ||
            !std::all_of(trustedRoots.begin(), trustedRoots.end(),
                         [&](const Domain::PathText& root) {
                             return contains(authority.trustedRoots(), root);
                         }) ||
            !std::all_of(grants.begin(), grants.end(),
                         [&](const Domain::FileAccess access) {
                             return contains(authority.grants(), access) &&
                                    !contains(authority.denials(), access);
                         })) {
            return Domain::Result<WorkspaceAuthority>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Authority narrowing attempted to widen scope or reuse a generation."));
        }
        return issueAuthority(
            authority.authorityId(),
            authority.projectId(),
            authority.callerId(),
            std::move(trustedRoots),
            authority.intent(),
            std::move(grants),
            authority.denials(),
            shellEnabled,
            generation);
    }

    [[nodiscard]] static Domain::Result<AuthorizedPath> issueAuthorizedPath(
        const WorkspaceAuthority& authority,
        Domain::PathText canonicalPath,
        Domain::PathText authorityRoot,
        const Domain::FileAccess access)
    {
        if (!contains(authority.trustedRoots(), authorityRoot) ||
            !contains(authority.grants(), access) ||
            contains(authority.denials(), access)) {
            return Domain::Result<AuthorizedPath>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Authorized path is not bound to a trusted root and granted access mode."));
        }
        return Domain::Result<AuthorizedPath>::success(AuthorizedPath{
            authority.authorityId(),
            std::move(canonicalPath),
            std::move(authorityRoot),
            access});
    }

private:
    template <typename T>
    [[nodiscard]] static bool contains(
        const std::vector<T>& values,
        const T& candidate) noexcept
    {
        return std::find(values.begin(), values.end(), candidate) != values.end();
    }
};

class IAtomicFileStore {
public:
    static constexpr std::size_t MaximumBytes = 32U * 1024U * 1024U;

    virtual ~IAtomicFileStore() = default;

    // This abstraction owns bounded bytes in a regular app-data file. Filesystem-object
    // metadata and alternate streams are outside its contract; an implementation must
    // fail closed when an unsupported feature would otherwise be silently discarded.

    [[nodiscard]] virtual Domain::Result<std::vector<std::byte>> read(
        const AuthorizedPath& path,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> replace(
        const AuthorizedPath& path,
        std::span<const std::byte> content,
        bool retainBackup,
        const Domain::OperationContext& context) noexcept = 0;
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<std::byte>> readFile(
        const AuthorizedPath& path,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> writeFile(
        const AuthorizedPath& path,
        std::span<const std::byte> content,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::DirectoryListing> list(
        const AuthorizedPath& directory,
        std::size_t maximumEntries,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> createDirectory(
        const AuthorizedPath& directory,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> remove(
        const AuthorizedPath& path,
        bool recursive,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> move(
        const AuthorizedPath& source,
        const AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
