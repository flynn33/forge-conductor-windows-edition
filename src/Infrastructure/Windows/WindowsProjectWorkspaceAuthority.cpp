#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/UtfConversion.h"
#include "Detail/WindowsPathResolver.h"
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

[[nodiscard]] bool within(const std::wstring_view candidate, const std::wstring_view root) noexcept
{
    return candidate.size() >= root.size() &&
        ::CompareStringOrdinal(candidate.data(), static_cast<int>(root.size()),
            root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
        (candidate.size() == root.size() || root.back() == L'\\' || candidate[root.size()] == L'\\');
}

[[nodiscard]] Domain::Result<std::vector<Domain::PathText>> hostVolumeRoots(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto live = Detail::validateOperationContext(context, std::chrono::steady_clock::now(),
            "enumerate available local host volumes");
        if (!live) return Domain::Result<std::vector<Domain::PathText>>::failure(live.error());
        const DWORD drives = ::GetLogicalDrives();
        std::vector<std::wstring> canonical;
        for (unsigned index = 0U; index < 26U; ++index) {
            if ((drives & (1UL << index)) == 0U) continue;
            const wchar_t root[]{static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
            const auto type = ::GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) continue;
            const DWORD attributes = ::GetFileAttributesW(root);
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) continue;
            auto text = Detail::strictUtf16ToUtf8(root);
            if (!text) return Domain::Result<std::vector<Domain::PathText>>::failure(text.error());
            auto resolved = Detail::WindowsPathResolver::resolveWorkspacePath(text.value());
            if (resolved) canonical.push_back(std::move(resolved).value());
        }
        std::sort(canonical.begin(), canonical.end(), [](const auto& left, const auto& right) {
            return left.size() < right.size() || (left.size() == right.size() && left < right);
        });
        std::vector<std::wstring> retained;
        std::vector<Domain::PathText> roots;
        for (const auto& root : canonical) {
            if (std::any_of(retained.begin(), retained.end(), [&](const auto& parent) { return within(root, parent); })) continue;
            auto path = Detail::WindowsPathResolver::toPathText(root);
            if (!path) return Domain::Result<std::vector<Domain::PathText>>::failure(path.error());
            retained.push_back(root);
            roots.push_back(std::move(path).value());
        }
        if (roots.empty()) return Domain::Result<std::vector<Domain::PathText>>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable, "Host filesystem access has no available local volume roots."));
        return Domain::Result<std::vector<Domain::PathText>>::success(std::move(roots));
    } catch (...) {
        return Domain::Result<std::vector<Domain::PathText>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Available local host volumes could not be enumerated."));
    }
}

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
    std::vector<Domain::PathText> configuredRoots,
    const Domain::FileSystemAccessMode fileSystemAccess) noexcept
    : projectRegistry_{projectRegistry},
      uuidGenerator_{uuidGenerator},
      serveClientId_{std::move(serveClientId)},
      shellEnabled_{shellEnabled},
      configuredRoots_{std::move(configuredRoots)},
      fileSystemAccess_{fileSystemAccess}
{
}

Domain::Result<void> WindowsProjectWorkspaceAuthority::updateOwnerPolicy(
    const bool shellEnabled,
    std::vector<Domain::PathText> configuredRoots,
    const Domain::FileSystemAccessMode fileSystemAccess,
    const Domain::OperationContext& context) noexcept
{
    try {
        const std::unique_lock policyLock{policyMutex_};
        auto valid = validateContext(context, "apply owner workspace policy");
        if (!valid) return valid;
        if (fileSystemAccess != Domain::FileSystemAccessMode::Workspace &&
            fileSystemAccess != Domain::FileSystemAccessMode::Host)
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "Filesystem access mode is invalid."));
        if (configuredRoots.size() > WindowsWorkspaceAuthority::MaximumTrustedRootsPerPolicy)
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded, "Configured workspace roots exceed 32 entries."));
        if (shellEnabled_ == shellEnabled && configuredRoots_ == configuredRoots &&
            fileSystemAccess_.load() == fileSystemAccess)
            return Domain::Result<void>::success();
        const std::lock_guard bindingLock{authorityIdsMutex_};
        shellEnabled_ = shellEnabled;
        configuredRoots_ = std::move(configuredRoots);
        fileSystemAccess_.store(fileSystemAccess);
        // New owner policy requires a fresh issuer binding. Previously frozen
        // worker scopes keep their recorded roots and cannot acquire new rights.
        authorityIds_.clear();
        boundRoots_.clear();
        return Domain::Result<void>::success();
    } catch (...) {
        return internalFailure<void>("The owner workspace policy could not be applied.");
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsProjectWorkspaceAuthority::authorityFor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        const std::shared_lock policyLock{policyMutex_};
        return authorityForLocked(projectId, context);
    } catch (...) {
        return internalFailure<Contracts::WorkspaceAuthority>(
            "The registered project workspace authority could not be issued.");
    }
}

Domain::Result<std::vector<Domain::PathText>>
WindowsProjectWorkspaceAuthority::configuredRootAllowlist(
    const Domain::OperationContext& context) noexcept
{
    try {
        const std::shared_lock policyLock{policyMutex_};
        return configuredRootAllowlistLocked(context);
    } catch (...) {
        return internalFailure<std::vector<Domain::PathText>>(
            "Configured workspace roots could not be read.");
    }
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

Domain::Result<Domain::ProjectMemoryDescriptor>
WindowsProjectWorkspaceAuthority::policyDescriptor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    auto descriptor = currentDescriptor(projectId, context);
    if (!descriptor || fileSystemAccess_.load() == Domain::FileSystemAccessMode::Workspace) return descriptor;
    if (fileSystemAccess_.load() != Domain::FileSystemAccessMode::Host)
        return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "Filesystem access mode is invalid."));
    auto roots = hostVolumeRoots(context);
    if (!roots) return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(roots.error());
    descriptor.value().aliases = std::move(roots).value();
    return descriptor;
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
        const std::shared_lock policyLock{policyMutex_};
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

Domain::Result<Domain::PathText> WindowsProjectWorkspaceAuthority::defaultWorkspacePath(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    try {
        const std::shared_lock policyLock{policyMutex_};
        auto id = cachedAuthorityId(authority);
        if (!id) return Domain::Result<Domain::PathText>::failure(id.error());
        auto descriptor = currentDescriptor(authority.projectId(), context);
        if (!descriptor) return Domain::Result<Domain::PathText>::failure(descriptor.error());
        return Domain::Result<Domain::PathText>::success(descriptor.value().aliases.front());
    } catch (...) {
        return internalFailure<Domain::PathText>("The registered project's default workspace path could not be read.");
    }
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsProjectWorkspaceAuthority::authorityForLocked(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto descriptor = policyDescriptor(projectId, context);
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
        const std::shared_lock policyLock{policyMutex_};
        auto descriptor = policyDescriptor(authority.projectId(), context);
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
        const std::shared_lock policyLock{policyMutex_};
        auto descriptor = policyDescriptor(authority.projectId(), context);
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
WindowsProjectWorkspaceAuthority::configuredRootAllowlistLocked(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto valid = validateContext(context, "read configured workspace roots");
        if (!valid) return Domain::Result<std::vector<Domain::PathText>>::failure(valid.error());
        if (fileSystemAccess_.load() == Domain::FileSystemAccessMode::Host) {
            auto active = hostVolumeRoots(context);
            if (!active) return active;
            for (const auto& root : configuredRoots_) {
                if (std::find(active.value().begin(), active.value().end(), root) == active.value().end())
                    active.value().push_back(root);
            }
            return active;
        }
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
        const std::shared_lock policyLock{policyMutex_};
        auto descriptor = policyDescriptor(authority.projectId(), context);
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
        auto configured = configuredRootAllowlistLocked(context);
        if (!configured) return Domain::Result<Contracts::WorkspaceAuthority>::failure(configured.error());
        for (const auto& configuredRoot : configured.value()) {
            auto candidateDescriptor = descriptor.value();
            candidateDescriptor.aliases = {configuredRoot};
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
        if (fileSystemAccess_.load() == Domain::FileSystemAccessMode::Host) {
            auto active = current->authorize(baseline.value(), {*accepted, std::nullopt, Domain::FileAccess::Read, false}, context);
            if (!active) return Domain::Result<Contracts::WorkspaceAuthority>::failure(active.error());
            return authorityForLocked(authority.projectId(), context);
        }
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
        return authorityForLocked(authority.projectId(), context);
    } catch (...) {
        return internalFailure<Contracts::WorkspaceAuthority>("The configured workspace root could not be bound.");
    }
}

Domain::Result<std::vector<Domain::PathText>>
WindowsProjectWorkspaceAuthority::boundConfiguredRoots(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        const std::shared_lock policyLock{policyMutex_};
        auto descriptor = currentDescriptor(projectId, context);
        if (!descriptor) return Domain::Result<std::vector<Domain::PathText>>::failure(descriptor.error());
        if (fileSystemAccess_.load() == Domain::FileSystemAccessMode::Host) return hostVolumeRoots(context);
        std::lock_guard lock{authorityIdsMutex_};
        const auto roots = boundRoots_.find(projectId);
        return Domain::Result<std::vector<Domain::PathText>>::success(
            roots == boundRoots_.end() ? std::vector<Domain::PathText>{} : roots->second);
    } catch (...) {
        return internalFailure<std::vector<Domain::PathText>>("Bound workspace roots could not be read.");
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
