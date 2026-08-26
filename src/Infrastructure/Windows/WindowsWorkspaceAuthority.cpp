#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"

#include "Detail/OperationContextGuard.h"
#include "Detail/UniqueHandle.h"
#include "Detail/UtfConversion.h"
#include "Detail/Win32Error.h"
#include "Detail/WindowsPathResolver.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Policy = WindowsWorkspaceAuthorityPolicy;

[[nodiscard]] bool validAccess(const Domain::FileAccess access) noexcept
{
    switch (access) {
    case Domain::FileAccess::Read:
    case Domain::FileAccess::Write:
    case Domain::FileAccess::Create:
    case Domain::FileAccess::Delete:
    case Domain::FileAccess::Execute:
        return true;
    }
    return false;
}

template <typename T>
[[nodiscard]] bool containsValue(const std::vector<T>& values, const T& candidate) noexcept
{
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

template <typename T>
[[nodiscard]] bool containsDuplicates(const std::vector<T>& values) noexcept
{
    for (auto current = values.begin(); current != values.end(); ++current) {
        if (std::find(std::next(current), values.end(), *current) != values.end()) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool equalPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
           ::CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool isWithin(
    const std::wstring_view candidate,
    const std::wstring_view root) noexcept
{
    if (candidate.size() < root.size() ||
        ::CompareStringOrdinal(
            candidate.data(), static_cast<int>(root.size()),
            root.data(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return candidate.size() == root.size() || root.back() == L'\\' ||
           candidate[root.size()] == L'\\';
}

[[nodiscard]] bool pathsOverlap(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return isWithin(left, right) || isWithin(right, left);
}

[[nodiscard]] std::wstring extendedPath(const std::wstring_view path)
{
    std::wstring result{L"\\\\?\\"};
    result.append(path);
    return result;
}

[[nodiscard]] Domain::Result<void> inspectOpenedPath(
    const std::wstring_view canonicalPath,
    const bool mustExist,
    const bool mustBeDirectory) noexcept
{
    try {
        const std::wstring nativePath = extendedPath(canonicalPath);
        Detail::UniqueHandle handle{::CreateFileW(
            nativePath.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!handle) {
            const DWORD nativeError = ::GetLastError();
            if (!mustExist &&
                (nativeError == ERROR_FILE_NOT_FOUND ||
                 nativeError == ERROR_PATH_NOT_FOUND)) {
                return Domain::Result<void>::success();
            }
            if (nativeError == ERROR_FILE_NOT_FOUND ||
                nativeError == ERROR_PATH_NOT_FOUND) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "A configured workspace root does not exist."));
            }
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "open a workspace authority path for validation", nativeError));
        }

        FILE_ATTRIBUTE_TAG_INFO tagInformation{};
        if (::GetFileInformationByHandleEx(
                handle.get(), FileAttributeTagInfo, &tagInformation,
                sizeof(tagInformation)) == FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "inspect a workspace authority path", ::GetLastError()));
        }
        if ((tagInformation.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "Workspace authority cannot traverse a reparse point."));
        }

        const bool isDirectory =
            (tagInformation.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
        if (mustBeDirectory && !isDirectory) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A configured workspace root must be an existing directory."));
        }
        if (isDirectory) {
            FILE_CASE_SENSITIVE_INFO caseSensitivity{};
            if (::GetFileInformationByHandleEx(
                    handle.get(), FileCaseSensitiveInfo, &caseSensitivity,
                    sizeof(caseSensitivity)) == FALSE) {
                return Domain::Result<void>::failure(Detail::makeWin32Error(
                    "inspect workspace directory case-sensitivity",
                    ::GetLastError()));
            }
            auto supported = Detail::WindowsPathResolver::
                validateDirectoryCaseSensitivityFlags(caseSensitivity.Flags);
            if (!supported) {
                return supported;
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The workspace authority path could not be inspected."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> resolveExistingRoot(
    const Domain::PathText& root) noexcept
{
    try {
        auto resolved = Detail::WindowsPathResolver::resolveAppOwnedRoot(root.value());
        if (!resolved) {
            return resolved;
        }
        auto inspected = inspectOpenedPath(resolved.value(), true, true);
        if (!inspected) {
            return Domain::Result<std::wstring>::failure(
                std::move(inspected).error());
        }

        // The second opened-handle resolution occurs after the exact root was
        // proven to exist and rejects any parent substitution during admission.
        auto revalidated =
            Detail::WindowsPathResolver::resolveAppOwnedRoot(root.value());
        if (!revalidated) {
            return revalidated;
        }
        return revalidated;
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The configured workspace root could not be resolved."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> resolveRequestedPath(
    const Domain::PathText& requestedPath) noexcept
{
    try {
        auto resolved =
            Detail::WindowsPathResolver::resolveAppOwnedRoot(requestedPath.value());
        if (!resolved) {
            return resolved;
        }
        auto inspected = inspectOpenedPath(resolved.value(), false, false);
        if (!inspected) {
            return Domain::Result<std::wstring>::failure(
                std::move(inspected).error());
        }
        return resolved;
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The requested workspace path could not be resolved."));
    }
}

[[nodiscard]] Domain::Result<void> validatePolicyAccess(const Policy& policy) noexcept
{
    try {
        if (policy.generation == 0U || policy.trustedRoots.empty() ||
            policy.trustedRoots.size() >
                WindowsWorkspaceAuthority::MaximumTrustedRootsPerPolicy ||
            policy.grants.empty() || !validAccess(policy.intent) ||
            !std::all_of(policy.grants.begin(), policy.grants.end(), validAccess) ||
            !std::all_of(policy.denials.begin(), policy.denials.end(), validAccess) ||
            containsDuplicates(policy.trustedRoots) ||
            containsDuplicates(policy.grants) ||
            containsDuplicates(policy.denials) ||
            !containsValue(policy.grants, policy.intent) ||
            containsValue(policy.denials, policy.intent) ||
            std::any_of(
                policy.grants.begin(), policy.grants.end(),
                [&](const Domain::FileAccess access) {
                    return containsValue(policy.denials, access);
                })) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A workspace authority policy has invalid bounds, access, or generation."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The workspace authority policy could not be validated."));
    }
}

[[nodiscard]] Domain::Result<std::vector<Policy>> resolvePolicies(
    std::vector<Policy> policies) noexcept
{
    try {
        if (policies.empty()) {
            return Domain::Result<std::vector<Policy>>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "At least one explicit workspace authority policy is required."));
        }
        if (policies.size() > WindowsWorkspaceAuthority::MaximumPolicies) {
            return Domain::Result<std::vector<Policy>>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Workspace authority policy count exceeds 32."));
        }

        std::vector<std::wstring> admittedRoots;
        admittedRoots.reserve(WindowsWorkspaceAuthority::MaximumPolicies);
        for (std::size_t index = 0U; index < policies.size(); ++index) {
            Policy& policy = policies[index];
            auto accessValidation = validatePolicyAccess(policy);
            if (!accessValidation) {
                return Domain::Result<std::vector<Policy>>::failure(
                    std::move(accessValidation).error());
            }

            for (std::size_t previous = 0U; previous < index; ++previous) {
                if (policies[previous].projectId == policy.projectId ||
                    policies[previous].authorityId == policy.authorityId) {
                    return Domain::Result<std::vector<Policy>>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::InvalidRequest,
                            "Workspace authority project and authority identifiers must be unique."));
                }
            }

            std::vector<Domain::PathText> canonicalRoots;
            canonicalRoots.reserve(policy.trustedRoots.size());
            for (const auto& configuredRoot : policy.trustedRoots) {
                auto resolved = resolveExistingRoot(configuredRoot);
                if (!resolved) {
                    return Domain::Result<std::vector<Policy>>::failure(
                        std::move(resolved).error());
                }
                if (std::any_of(
                        admittedRoots.begin(), admittedRoots.end(),
                        [&](const std::wstring& admitted) {
                            return pathsOverlap(resolved.value(), admitted);
                        })) {
                    return Domain::Result<std::vector<Policy>>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::InvalidRequest,
                            "Workspace authority roots must not duplicate or overlap."));
                }
                auto canonical =
                    Detail::WindowsPathResolver::toPathText(resolved.value());
                if (!canonical) {
                    return Domain::Result<std::vector<Policy>>::failure(
                        std::move(canonical).error());
                }
                admittedRoots.push_back(std::move(resolved).value());
                canonicalRoots.push_back(std::move(canonical).value());
            }
            policy.trustedRoots = std::move(canonicalRoots);
        }
        return Domain::Result<std::vector<Policy>>::success(std::move(policies));
    } catch (...) {
        return Domain::Result<std::vector<Policy>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Workspace authority policies could not be admitted."));
    }
}

[[nodiscard]] const Policy* policyForProject(
    const std::vector<Policy>& policies,
    const Domain::ProjectId& projectId) noexcept
{
    const auto match = std::find_if(
        policies.begin(), policies.end(), [&](const Policy& policy) {
            return policy.projectId == projectId;
        });
    return match == policies.end() ? nullptr : &*match;
}

[[nodiscard]] const Policy* policyForAuthority(
    const std::vector<Policy>& policies,
    const Domain::AuthorityId& authorityId) noexcept
{
    const auto match = std::find_if(
        policies.begin(), policies.end(), [&](const Policy& policy) {
            return policy.authorityId == authorityId;
        });
    return match == policies.end() ? nullptr : &*match;
}

[[nodiscard]] Domain::Result<const Policy*> validateAuthority(
    const std::vector<Policy>& policies,
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    try {
        const Policy* const policy =
            policyForAuthority(policies, authority.authorityId());
        if (policy == nullptr) {
            return Domain::Result<const Policy*>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The workspace authority identifier is not configured."));
        }
        if (authority.projectId() != policy->projectId) {
            return Domain::Result<const Policy*>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The workspace authority belongs to a different project."));
        }
        if (authority.callerId() != policy->callerId ||
            authority.generation() < policy->generation ||
            authority.intent() != policy->intent ||
            authority.trustedRoots().empty() || authority.grants().empty() ||
            containsDuplicates(authority.trustedRoots()) ||
            containsDuplicates(authority.grants()) ||
            containsDuplicates(authority.denials()) ||
            !std::all_of(
                authority.trustedRoots().begin(), authority.trustedRoots().end(),
                [&](const Domain::PathText& root) {
                    return containsValue(policy->trustedRoots, root);
                }) ||
            !std::all_of(
                authority.grants().begin(), authority.grants().end(),
                [&](const Domain::FileAccess access) {
                    return validAccess(access) && containsValue(policy->grants, access) &&
                           !containsValue(authority.denials(), access);
                }) ||
            !std::all_of(
                policy->denials.begin(), policy->denials.end(),
                [&](const Domain::FileAccess access) {
                    return containsValue(authority.denials(), access);
                }) ||
            !std::all_of(
                authority.denials().begin(), authority.denials().end(), validAccess) ||
            !containsValue(authority.grants(), authority.intent()) ||
            containsValue(authority.denials(), authority.intent()) ||
            (authority.shellEnabled() && !policy->shellEnabled)) {
            return Domain::Result<const Policy*>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The workspace authority widens or predates its configured policy."));
        }
        return Domain::Result<const Policy*>::success(policy);
    } catch (...) {
        return Domain::Result<const Policy*>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The workspace authority binding could not be validated."));
    }
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

[[nodiscard]] bool destructiveAccess(const Domain::FileAccess access) noexcept
{
    return access == Domain::FileAccess::Write ||
           access == Domain::FileAccess::Create ||
           access == Domain::FileAccess::Delete;
}

} // namespace

WindowsWorkspaceAuthority::WindowsWorkspaceAuthority(
    std::vector<WindowsWorkspaceAuthorityPolicy> policies) noexcept
    : policies_{resolvePolicies(std::move(policies))}
{
}

Domain::Result<Contracts::WorkspaceAuthority>
WindowsWorkspaceAuthority::authorityFor(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto contextValidation =
            validateContext(context, "issue workspace authority");
        if (!contextValidation) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(contextValidation).error());
        }
        if (!policies_) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                policies_.error());
        }
        const Policy* const policy = policyForProject(policies_.value(), projectId);
        if (policy == nullptr) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::ProjectNotFound,
                    "No workspace authority policy is configured for the project."));
        }
        return issueAuthority(
            policy->authorityId, policy->projectId, policy->callerId,
            policy->trustedRoots, policy->intent, policy->grants,
            policy->denials, policy->shellEnabled, policy->generation);
    } catch (...) {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The workspace authority could not be issued."));
    }
}

Domain::Result<Contracts::WorkspaceAuthority> WindowsWorkspaceAuthority::narrow(
    const Contracts::WorkspaceAuthority& authority,
    const std::vector<Domain::PathText>& trustedRoots,
    const std::vector<Domain::FileAccess>& grants,
    const bool shellEnabled,
    const std::uint64_t generation,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto contextValidation =
            validateContext(context, "narrow workspace authority");
        if (!contextValidation) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(contextValidation).error());
        }
        if (!policies_) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                policies_.error());
        }
        auto policyValidation = validateAuthority(policies_.value(), authority);
        if (!policyValidation) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(policyValidation).error());
        }
        if (trustedRoots.size() > MaximumTrustedRootsPerPolicy ||
            containsDuplicates(trustedRoots) || containsDuplicates(grants) ||
            !std::all_of(grants.begin(), grants.end(), validAccess)) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "Authority narrowing contains non-canonical or excessive scope."));
        }
        return narrowAuthority(
            authority, std::vector<Domain::PathText>{trustedRoots},
            std::vector<Domain::FileAccess>{grants}, shellEnabled, generation);
    } catch (...) {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The workspace authority could not be narrowed."));
    }
}

Domain::Result<Contracts::AuthorizedPath> WindowsWorkspaceAuthority::authorize(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathAuthorizationRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto contextValidation =
            validateContext(context, "authorize a workspace path");
        if (!contextValidation) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(contextValidation).error());
        }
        if (!policies_) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                policies_.error());
        }
        auto policyValidation = validateAuthority(policies_.value(), authority);
        if (!policyValidation) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(policyValidation).error());
        }
        if (!validAccess(request.access) ||
            !containsValue(authority.grants(), request.access) ||
            containsValue(authority.denials(), request.access)) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "The requested path access is not granted by the authority."));
        }

        auto requested = resolveRequestedPath(request.requestedPath);
        if (!requested) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(requested).error());
        }

        const Domain::PathText* selectedRoot{};
        if (request.basePath.has_value()) {
            auto base = resolveExistingRoot(request.basePath.value());
            if (!base) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    std::move(base).error());
            }
            for (const auto& trustedRoot : authority.trustedRoots()) {
                auto root = Detail::strictUtf8ToUtf16(trustedRoot.value());
                if (!root) {
                    return Domain::Result<Contracts::AuthorizedPath>::failure(
                        std::move(root).error());
                }
                if (equalPath(base.value(), root.value())) {
                    selectedRoot = &trustedRoot;
                    break;
                }
            }
            if (selectedRoot == nullptr) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PathOutsideAuthority,
                        "The requested base path is not an exact trusted root."));
            }
        } else {
            for (const auto& trustedRoot : authority.trustedRoots()) {
                auto root = Detail::strictUtf8ToUtf16(trustedRoot.value());
                if (!root) {
                    return Domain::Result<Contracts::AuthorizedPath>::failure(
                        std::move(root).error());
                }
                if (isWithin(requested.value(), root.value())) {
                    if (selectedRoot != nullptr) {
                        return Domain::Result<Contracts::AuthorizedPath>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::Unauthorized,
                                "The requested path ambiguously matches multiple roots."));
                    }
                    selectedRoot = &trustedRoot;
                }
            }
        }

        if (selectedRoot == nullptr) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "The requested path is outside the workspace authority roots."));
        }
        auto root = resolveExistingRoot(*selectedRoot);
        if (!root) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(root).error());
        }
        if (!isWithin(requested.value(), root.value())) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "The requested path escaped its canonical workspace root."));
        }
        if (request.protectAuthorityRoot && destructiveAccess(request.access) &&
            equalPath(requested.value(), root.value())) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "The requested mutation is blocked by authority-root protection."));
        }

        contextValidation = validateContext(
            context, "complete workspace path authorization");
        if (!contextValidation) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(contextValidation).error());
        }
        auto canonicalPath =
            Detail::WindowsPathResolver::toPathText(requested.value());
        if (!canonicalPath) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                std::move(canonicalPath).error());
        }
        return issueAuthorizedPath(
            authority, std::move(canonicalPath).value(), *selectedRoot,
            request.access);
    } catch (...) {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The workspace path could not be authorized."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
