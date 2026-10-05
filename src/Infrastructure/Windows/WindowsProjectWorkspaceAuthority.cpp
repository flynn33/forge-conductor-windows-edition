#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/UtfConversion.h"
#include <Windows.h>
#include <algorithm>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::uint64_t InitialGeneration = 1U;

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

[[nodiscard]] Domain::Result<void> validateDescriptor(
    const Domain::ProjectMemoryDescriptor& descriptor,
    const Domain::ProjectId& requestedProjectId) noexcept
{
    if (descriptor.id != requestedProjectId) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::ProjectScopeMismatch,
            "The project registry returned a descriptor for a different project."));
    }
    if (descriptor.aliases.empty()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "A registered project must retain at least one workspace alias."));
    }
    if (descriptor.aliases.size() >
        WindowsWorkspaceAuthority::MaximumTrustedRootsPerPolicy) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "A registered project cannot exceed 32 workspace aliases."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] WindowsWorkspaceAuthorityPolicy policyFor(
    const Domain::ProjectMemoryDescriptor& descriptor,
    const Domain::AuthorityId& authorityId,
    const Domain::ClientId& serveClientId,
    const bool shellEnabled)
{
    std::vector<Domain::FileAccess> grants{
        Domain::FileAccess::Read,
        Domain::FileAccess::Write,
        Domain::FileAccess::Create,
        Domain::FileAccess::Delete};
    std::vector<Domain::FileAccess> denials;
    if (shellEnabled) {
        grants.push_back(Domain::FileAccess::Execute);
    } else {
        denials.push_back(Domain::FileAccess::Execute);
    }
    return WindowsWorkspaceAuthorityPolicy{
        authorityId,
        descriptor.id,
        serveClientId,
        descriptor.aliases,
        Domain::FileAccess::Write,
        std::move(grants),
        std::move(denials),
        shellEnabled,
        InitialGeneration};
}

[[nodiscard]] std::unique_ptr<WindowsWorkspaceAuthority> makeDelegate(
    const Domain::ProjectMemoryDescriptor& descriptor,
    const Domain::AuthorityId& authorityId,
    const Domain::ClientId& serveClientId,
    const bool shellEnabled)
{
    std::vector<WindowsWorkspaceAuthorityPolicy> policies;
    policies.push_back(policyFor(
        descriptor, authorityId, serveClientId, shellEnabled));
    return std::make_unique<WindowsWorkspaceAuthority>(std::move(policies));
}

template <typename T>
[[nodiscard]] Domain::Result<T> internalFailure(const std::string_view message) noexcept
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::string{message}));
}

} // namespace

WindowsProjectWorkspaceAuthority::WindowsProjectWorkspaceAuthority(
    Contracts::IProjectRegistryRepository& projectRegistry,
    Contracts::IUuidGenerator& uuidGenerator,
    Domain::ClientId serveClientId,
    const bool shellEnabled,
    std::vector<Domain::PathText> configuredRoots) noexcept
    : projectRegistry_{projectRegistry},
      uuidGenerator_{uuidGenerator},
      serveClientId_{std::move(serveClientId)},
      shellEnabled_{shellEnabled},
      configuredRoots_{std::move(configuredRoots)}
{
}

Domain::Result<Domain::ProjectMemoryDescriptor>
WindowsProjectWorkspaceAuthority::currentDescriptor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto contextValidation =
            validateContext(context, "resolve a registered project workspace");
        if (!contextValidation) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                std::move(contextValidation).error());
        }
        auto descriptor = projectRegistry_.descriptor(projectId, context);
        if (!descriptor) {
            return descriptor;
        }
        auto descriptorValidation = validateDescriptor(descriptor.value(), projectId);
        if (!descriptorValidation) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                std::move(descriptorValidation).error());
        }
        {
            std::lock_guard lock{authorityIdsMutex_};
            if (const auto roots = boundRoots_.find(projectId); roots != boundRoots_.end()) {
                for (const auto& root : roots->second) {
                    if (std::find(descriptor.value().aliases.begin(), descriptor.value().aliases.end(), root)
                        == descriptor.value().aliases.end()) descriptor.value().aliases.push_back(root);
                }
            }
        }
        return descriptor;
    } catch (...) {
        return internalFailure<Domain::ProjectMemoryDescriptor>(
            "The registered project workspace descriptor could not be read.");
    }
}

Domain::Result<Domain::AuthorityId>
WindowsProjectWorkspaceAuthority::cachedAuthorityId(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    try {
        std::lock_guard lock{authorityIdsMutex_};
        const auto match = authorityIds_.find(authority.projectId());
        if (match == authorityIds_.end() ||
            match->second != authority.authorityId()) {
            return Domain::Result<Domain::AuthorityId>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The workspace authority is not bound to this process."));
        }
        if (authority.callerId() != serveClientId_ ||
            authority.generation() < InitialGeneration) {
            return Domain::Result<Domain::AuthorityId>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The workspace authority caller or generation is not valid."));
        }
        return Domain::Result<Domain::AuthorityId>::success(match->second);
    } catch (...) {
        return internalFailure<Domain::AuthorityId>(
            "The workspace authority binding could not be read.");
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsProjectWorkspaceAuthority::storedProjectAuthorityFor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto descriptor = currentDescriptor(projectId, context);
        if (!descriptor) return Domain::Result<Contracts::WorkspaceAuthority>::failure(descriptor.error());
        auto id = uuidGenerator_.next();
        if (!id) return Domain::Result<Contracts::WorkspaceAuthority>::failure(id.error());
        // Stored-record reads do not open the workspace. This token is not entered
        // in the filesystem authority cache and cannot authorize a path.
        return issueAuthority(Domain::AuthorityId{std::move(id).value()}, projectId,
            serveClientId_, descriptor.value().aliases, Domain::FileAccess::Read,
            {Domain::FileAccess::Read}, {}, false, InitialGeneration);
    } catch (...) {
        return internalFailure<Contracts::WorkspaceAuthority>(
            "The stored project binding could not be read.");
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsProjectWorkspaceAuthority::authorityFor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto descriptor = currentDescriptor(projectId, context);
        if (!descriptor) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(descriptor).error());
        }

        std::optional<Domain::AuthorityId> existingId;
        {
            std::lock_guard lock{authorityIdsMutex_};
            const auto existing = authorityIds_.find(projectId);
            if (existing != authorityIds_.end()) {
                existingId.emplace(existing->second);
            }
        }
        if (existingId.has_value()) {
            auto delegate = makeDelegate(
                descriptor.value(), existingId.value(), serveClientId_, shellEnabled_);
            return delegate->authorityFor(projectId, context);
        }

        auto generated = uuidGenerator_.next();
        if (!generated) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(generated).error());
        }
        Domain::AuthorityId candidateId{std::move(generated).value()};
        auto candidateDelegate = makeDelegate(
            descriptor.value(), candidateId, serveClientId_, shellEnabled_);
        auto candidate = candidateDelegate->authorityFor(projectId, context);
        if (!candidate) {
            return candidate;
        }

        Domain::AuthorityId publishedId = candidateId;
        {
            std::lock_guard lock{authorityIdsMutex_};
            const auto existing = authorityIds_.find(projectId);
            if (existing != authorityIds_.end()) {
                publishedId = existing->second;
            } else {
                if (authorityIds_.size() >= MaximumProjects) {
                    return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The workspace authority project bound is exhausted."));
                }
                authorityIds_.emplace(projectId, candidateId);
            }
        }

        if (publishedId == candidateId) {
            return candidate;
        }
        auto publishedDelegate = makeDelegate(
            descriptor.value(), publishedId, serveClientId_, shellEnabled_);
        return publishedDelegate->authorityFor(projectId, context);
    } catch (...) {
        return internalFailure<Contracts::WorkspaceAuthority>(
            "The registered project workspace authority could not be issued.");
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsProjectWorkspaceAuthority::narrow(
    const Contracts::WorkspaceAuthority& authority,
    const std::vector<Domain::PathText>& trustedRoots,
    const std::vector<Domain::FileAccess>& grants,
    const bool shellEnabled,
    const std::uint64_t generation,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto descriptor = currentDescriptor(authority.projectId(), context);
        if (!descriptor) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(descriptor).error());
        }
        auto authorityId = cachedAuthorityId(authority);
        if (!authorityId) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(authorityId).error());
        }
        auto delegate = makeDelegate(
            descriptor.value(), authorityId.value(), serveClientId_, shellEnabled_);
        return delegate->narrow(
            authority, trustedRoots, grants, shellEnabled, generation, context);
    } catch (...) {
        return internalFailure<Contracts::WorkspaceAuthority>(
            "The registered project workspace authority could not be narrowed.");
    }
}

Domain::Result<Contracts::AuthorizedPath>
WindowsProjectWorkspaceAuthority::authorize(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathAuthorizationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto descriptor = currentDescriptor(authority.projectId(), context);
        if (!descriptor) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(descriptor).error());
        }
        auto authorityId = cachedAuthorityId(authority);
        if (!authorityId) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(authorityId).error());
        }
        auto delegate = makeDelegate(
            descriptor.value(), authorityId.value(), serveClientId_, shellEnabled_);
        return delegate->authorize(authority, request, context);
    } catch (...) {
        return internalFailure<Contracts::AuthorizedPath>(
            "The registered project workspace path could not be authorized.");
    }
}

Domain::Result<std::vector<Domain::PathText>>
WindowsProjectWorkspaceAuthority::configuredRootAllowlist(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto valid = validateContext(context, "read configured workspace roots");
        if (!valid) return Domain::Result<std::vector<Domain::PathText>>::failure(valid.error());
        return Domain::Result<std::vector<Domain::PathText>>::success(configuredRoots_);
    } catch (...) {
        return internalFailure<std::vector<Domain::PathText>>("Configured workspace roots could not be read.");
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsProjectWorkspaceAuthority::bindConfiguredRoot(
    const Contracts::WorkspaceAuthority& authority, const Domain::PathText& requested,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto descriptor = currentDescriptor(authority.projectId(), context);
        if (!descriptor) return Domain::Result<Contracts::WorkspaceAuthority>::failure(descriptor.error());
        auto id = cachedAuthorityId(authority);
        if (!id) return Domain::Result<Contracts::WorkspaceAuthority>::failure(id.error());
        auto current = makeDelegate(descriptor.value(), id.value(), serveClientId_, shellEnabled_);
        auto checked = current->authorize(authority, {authority.trustedRoots().front(), std::nullopt,
            Domain::FileAccess::Write, false}, context);
        if (!checked) return Domain::Result<Contracts::WorkspaceAuthority>::failure(checked.error());
        auto baseline = current->authorityFor(authority.projectId(), context);
        if (!baseline) return baseline;
        if (authority.shellEnabled() != baseline.value().shellEnabled() ||
            authority.trustedRoots().size() != baseline.value().trustedRoots().size() ||
            !std::all_of(baseline.value().trustedRoots().begin(), baseline.value().trustedRoots().end(),
                [&](const auto& root) { return std::find(authority.trustedRoots().begin(), authority.trustedRoots().end(), root)
                    != authority.trustedRoots().end(); }) ||
            !std::all_of(baseline.value().grants().begin(), baseline.value().grants().end(),
                [&](const auto access) { return std::find(authority.grants().begin(), authority.grants().end(), access)
                    != authority.grants().end(); }))
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized, "A narrowed capability cannot bind an additional workspace root."));
        std::optional<Domain::PathText> accepted;
        for (const auto& configured : configuredRoots_) {
            auto candidateDescriptor = descriptor.value();
            candidateDescriptor.aliases = {configured};
            auto candidate = makeDelegate(candidateDescriptor, id.value(), serveClientId_, shellEnabled_);
            auto token = candidate->authorityFor(authority.projectId(), context);
            if (!token) continue;
            auto path = candidate->authorize(token.value(), {requested, std::nullopt,
                Domain::FileAccess::Read, false}, context);
            if (!path) continue;
            auto canonical = Detail::strictUtf8ToUtf16(path.value().canonicalPath().value());
            auto expected = Detail::strictUtf8ToUtf16(token.value().trustedRoots().front().value());
            if (!canonical || !expected || ::CompareStringOrdinal(canonical.value().c_str(), -1,
                    expected.value().c_str(), -1, TRUE) != CSTR_EQUAL) continue;
            const DWORD attributes = ::GetFileAttributesW(canonical.value().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) continue;
            accepted = path.value().canonicalPath();
            break;
        }
        if (!accepted) return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(
            Domain::ErrorCodes::Unauthorized,
            "The root must exactly match an existing directory in the owner's configured allowed_roots. A model request cannot grant a new root."));
        {
            std::lock_guard lock{authorityIdsMutex_};
            auto& roots = boundRoots_[authority.projectId()];
            auto combined = descriptor.value();
            for (const auto& root : roots) {
                if (std::find(combined.aliases.begin(), combined.aliases.end(), root) == combined.aliases.end())
                    combined.aliases.push_back(root);
            }
            if (std::find(combined.aliases.begin(), combined.aliases.end(), *accepted) == combined.aliases.end()) {
                if (combined.aliases.size() >= WindowsWorkspaceAuthority::MaximumTrustedRootsPerPolicy)
                    return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded, "The combined workspace root limit is 32."));
                combined.aliases.push_back(*accepted);
                auto candidate = makeDelegate(combined, id.value(), serveClientId_, shellEnabled_);
                auto valid = candidate->authorityFor(authority.projectId(), context);
                if (!valid) return Domain::Result<Contracts::WorkspaceAuthority>::failure(valid.error());
                if (std::find(roots.begin(), roots.end(), *accepted) == roots.end()) roots.push_back(*accepted);
            }
        }
        return authorityFor(authority.projectId(), context);
    } catch (...) {
        return internalFailure<Contracts::WorkspaceAuthority>("The configured workspace root could not be bound.");
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
