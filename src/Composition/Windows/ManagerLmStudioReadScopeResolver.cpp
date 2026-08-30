#include "ManagerLmStudioReadScopeResolver.h"

#include "../../Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "../../Infrastructure/Windows/Detail/UtfConversion.h"
#include "../../Infrastructure/Windows/Detail/WindowsPathResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Composition::Windows {
namespace {

namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace WindowsDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return WindowsDetail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

[[nodiscard]] bool validSource(
    const Domain::LMStudioDiscoverySource source) noexcept
{
    switch (source) {
    case Domain::LMStudioDiscoverySource::ExplicitConfiguration:
    case Domain::LMStudioDiscoverySource::InstalledApplication:
    case Domain::LMStudioDiscoverySource::KnownUserLocation:
    case Domain::LMStudioDiscoverySource::RunningProcess:
        return true;
    }
    return false;
}

[[nodiscard]] Domain::Result<bool> equalWindowsPath(
    const Domain::PathText& left,
    const Domain::PathText& right) noexcept
{
    try {
        auto leftWide = WindowsDetail::strictUtf8ToUtf16(left.value());
        if (!leftWide) {
            return Domain::Result<bool>::failure(
                std::move(leftWide).error());
        }
        auto rightWide = WindowsDetail::strictUtf8ToUtf16(right.value());
        if (!rightWide) {
            return Domain::Result<bool>::failure(
                std::move(rightWide).error());
        }
        return Domain::Result<bool>::success(
            leftWide.value().size() == rightWide.value().size() &&
            ::CompareStringOrdinal(
                leftWide.value().data(),
                static_cast<int>(leftWide.value().size()),
                rightWide.value().data(),
                static_cast<int>(rightWide.value().size()), TRUE) ==
                CSTR_EQUAL);
    } catch (...) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "An LM Studio maintenance path could not be compared."));
    }
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

[[nodiscard]] bool overlap(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return isWithin(left, right) || isWithin(right, left);
}

[[nodiscard]] Domain::Result<Domain::PathText> canonicalRoot(
    const Domain::PathText& root) noexcept
{
    auto resolved = WindowsDetail::WindowsPathResolver::resolveAppOwnedRoot(
        root.value());
    if (!resolved) {
        return Domain::Result<Domain::PathText>::failure(
            std::move(resolved).error());
    }
    return WindowsDetail::WindowsPathResolver::toPathText(resolved.value());
}

[[nodiscard]] Domain::Result<Domain::PathText> canonicalParent(
    const Domain::PathText& file,
    const std::string_view resourceName) noexcept
{
    try {
        auto wide = WindowsDetail::strictUtf8ToUtf16(file.value());
        if (!wide) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(wide).error());
        }
        if (wide.value().find(L'/') != std::wstring::npos ||
            wide.value().ends_with(L'\\')) {
            return Domain::Result<Domain::PathText>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    std::string{resourceName} +
                        " is not a canonical Windows file path."));
        }
        const std::size_t separator = wide.value().find_last_of(L'\\');
        if (separator == std::wstring::npos || separator < 2U ||
            separator + 1U >= wide.value().size()) {
            return Domain::Result<Domain::PathText>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    std::string{resourceName} +
                        " has no narrowly scoped parent directory."));
        }
        const std::wstring parent = wide.value().substr(0U, separator);
        auto parentUtf8 = WindowsDetail::strictUtf16ToUtf8(parent);
        if (!parentUtf8) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(parentUtf8).error());
        }
        auto parentText = Domain::PathText::create(parentUtf8.value());
        if (!parentText) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(parentText).error());
        }
        return canonicalRoot(parentText.value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::string{resourceName} +
                " parent directory could not be resolved."));
    }
}

enum class ObservedResourceKind {
    Application,
    Configuration,
};

struct ObservedResource final {
    Domain::PathText path;
    ObservedResourceKind kind;
};

[[nodiscard]] Domain::Result<void> appendResourceObservation(
    std::vector<ObservedResource>& resources,
    const Domain::PathText& resource,
    const ObservedResourceKind kind) noexcept
{
    for (const auto& existing : resources) {
        auto equal = equalWindowsPath(existing.path, resource);
        if (!equal) {
            return Domain::Result<void>::failure(
                std::move(equal).error());
        }
        if (equal.value()) {
            if (existing.kind != kind) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The LM Studio discovery snapshot classifies one path "
                    "as both an application and configuration resource."));
            }
            // Native discovery deliberately retains corroborating evidence
            // from registry, known-location, and process sources. Equal
            // resource observations do not broaden the final authority.
            return Domain::Result<void>::success();
        }
    }
    resources.push_back(ObservedResource{resource, kind});
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> appendAuthorityRoot(
    std::vector<Domain::PathText>& roots,
    std::vector<std::wstring>& wideRoots,
    Domain::PathText root) noexcept
{
    try {
        auto wide = WindowsDetail::strictUtf8ToUtf16(root.value());
        if (!wide) {
            return Domain::Result<void>::failure(std::move(wide).error());
        }
        for (std::size_t index{}; index < roots.size(); ++index) {
            if (wide.value().size() == wideRoots[index].size() &&
                ::CompareStringOrdinal(
                    wide.value().data(),
                    static_cast<int>(wide.value().size()),
                    wideRoots[index].data(),
                    static_cast<int>(wideRoots[index].size()), TRUE) ==
                    CSTR_EQUAL) {
                return Domain::Result<void>::success();
            }
            if (overlap(wide.value(), wideRoots[index])) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "LM Studio maintenance roots overlap and would broaden "
                    "the read authority."));
            }
        }
        if (roots.size() >=
            ManagerLmStudioReadScopeResolver::MaximumAuthorityRoots) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "LM Studio maintenance exceeded its four-root authority "
                "bound."));
        }
        wideRoots.push_back(std::move(wide).value());
        roots.push_back(std::move(root));
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "An LM Studio maintenance authority root could not be retained."));
    }
}

[[nodiscard]] Domain::Result<void> validateEvidenceConsistency(
    const InfrastructureWindows::WindowsLMStudioCandidateSelection& selection)
    noexcept
{
    try {
        const auto& evaluations = selection.evaluations();
        const auto& evidence = selection.status().discoveryEvidence;
        if (evidence.size() != evaluations.size()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio selected evidence does not match its candidate "
                "snapshot."));
        }
        for (std::size_t index{}; index < evaluations.size(); ++index) {
            const auto& evaluation = evaluations[index];
            const auto& candidate = evaluation.candidate();
            const auto& item = evidence[index];
            if (!validSource(candidate.source) ||
                item.source != candidate.source ||
                item.path != candidate.evidencePath ||
                item.valid != evaluation.valid() ||
                item.selected != evaluation.selected() ||
                item.detail != evaluation.detail()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "LM Studio selected evidence is internally "
                    "inconsistent."));
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio selected evidence could not be validated."));
    }
}

[[nodiscard]] bool hasExactAccess(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const std::vector<Domain::FileAccess> expectedDenials{
        Domain::FileAccess::Write,
        Domain::FileAccess::Create,
        Domain::FileAccess::Delete,
        Domain::FileAccess::Execute};
    return authority.intent() == Domain::FileAccess::Read &&
        authority.grants() ==
            std::vector<Domain::FileAccess>{Domain::FileAccess::Read} &&
        authority.denials() == expectedDenials &&
        !authority.shellEnabled();
}

} // namespace

ManagerLmStudioReadScope::ManagerLmStudioReadScope(
    std::unique_ptr<InfrastructureWindows::WindowsWorkspaceAuthority> issuer,
    Contracts::WorkspaceAuthority authority) noexcept
    : issuer_{std::move(issuer)}, authority_{std::move(authority)}
{
}

ManagerLmStudioReadScopeResolver::ManagerLmStudioReadScopeResolver(
    ManagerLmStudioReadScopeConfiguration configuration) noexcept
    : configuration_{std::move(configuration)}
{
}

Domain::Result<ManagerLmStudioReadScope>
ManagerLmStudioReadScopeResolver::resolve(
    const InfrastructureWindows::WindowsLMStudioCandidateSelection& selection,
    const Domain::OperationContext& context) const noexcept
{
    try {
        auto active = validateContext(
            context, "resolve the Manager LM Studio read scope");
        if (!active) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(active).error());
        }
        if (configuration_.selectionIdentity.generation == 0U ||
            configuration_.readScopeIdentity.generation == 0U) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Manager LM Studio authority generations must be "
                    "nonzero."));
        }
        if (configuration_.selectionIdentity.authorityId ==
            configuration_.readScopeIdentity.authorityId) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The LM Studio selection and read-scope authorities must "
                    "use distinct capability identifiers."));
        }
        if (configuration_.selectionIdentity.projectId !=
            configuration_.readScopeIdentity.projectId) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The LM Studio selection and read-scope authorities must "
                    "belong to the same maintenance project."));
        }
        if (configuration_.selectionIdentity.callerId !=
            configuration_.readScopeIdentity.callerId) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "The LM Studio selection and read-scope authorities must "
                    "belong to the same maintenance caller."));
        }
        if (selection.projectId() !=
            configuration_.selectionIdentity.projectId) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "LM Studio selection evidence belongs to a different "
                    "maintenance project."));
        }
        if (selection.authorityId() !=
                configuration_.selectionIdentity.authorityId ||
            selection.callerId() !=
                configuration_.selectionIdentity.callerId ||
            selection.authorityGeneration() !=
                configuration_.selectionIdentity.generation) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "LM Studio selection evidence is stale or belongs to a "
                    "different maintenance caller."));
        }
        if (selection.evaluations().size() > MaximumCandidateEvaluations) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "Manager LM Studio maintenance accepts at most 64 "
                    "candidate evaluations."));
        }
        auto evidenceValidation = validateEvidenceConsistency(selection);
        if (!evidenceValidation) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(evidenceValidation).error());
        }

        const InfrastructureWindows::WindowsLMStudioCandidateEvaluation*
            selectedConfiguration{};
        const InfrastructureWindows::WindowsLMStudioCandidateEvaluation*
            selectedApplication{};
        std::vector<ObservedResource> resources;
        resources.reserve(selection.evaluations().size());
        for (const auto& evaluation : selection.evaluations()) {
            active = validateContext(
                context, "validate LM Studio maintenance selection evidence");
            if (!active) {
                return Domain::Result<ManagerLmStudioReadScope>::failure(
                    std::move(active).error());
            }
            const auto& candidate = evaluation.candidate();
            const std::size_t resourceCount =
                static_cast<std::size_t>(
                    candidate.applicationExecutable.has_value()) +
                static_cast<std::size_t>(
                    candidate.configurationPath.has_value());
            if (resourceCount > 1U ||
                (candidate.valid && resourceCount != 1U)) {
                return Domain::Result<ManagerLmStudioReadScope>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The LM Studio discovery snapshot contains an "
                        "ambiguous resource candidate."));
            }
            if (candidate.applicationExecutable) {
                auto unique = appendResourceObservation(
                    resources,
                    *candidate.applicationExecutable,
                    ObservedResourceKind::Application);
                if (!unique) {
                    return Domain::Result<ManagerLmStudioReadScope>::failure(
                        std::move(unique).error());
                }
            }
            if (candidate.configurationPath) {
                auto unique = appendResourceObservation(
                    resources,
                    *candidate.configurationPath,
                    ObservedResourceKind::Configuration);
                if (!unique) {
                    return Domain::Result<ManagerLmStudioReadScope>::failure(
                        std::move(unique).error());
                }
            }
            if (!evaluation.selected()) {
                continue;
            }
            if (!evaluation.valid() || resourceCount != 1U) {
                return Domain::Result<ManagerLmStudioReadScope>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "LM Studio selected evidence is invalid or "
                        "resource-less."));
            }
            if (candidate.configurationPath) {
                if (selectedConfiguration != nullptr) {
                    return Domain::Result<ManagerLmStudioReadScope>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "LM Studio selection contains multiple selected "
                            "configuration resources."));
                }
                selectedConfiguration = &evaluation;
            } else {
                if (selectedApplication != nullptr) {
                    return Domain::Result<ManagerLmStudioReadScope>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Conflict,
                            "LM Studio selection contains multiple selected "
                            "application resources."));
                }
                selectedApplication = &evaluation;
            }
        }

        if (selectedConfiguration == nullptr ||
            selectedApplication == nullptr ||
            !selection.status().configurationPath ||
            !selection.status().applicationExecutable ||
            !selection.status().lmStudioPresent) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "LM Studio maintenance requires one selected valid "
                    "configuration and application resource."));
        }
        const auto& selectedConfigurationPath =
            *selectedConfiguration->candidate().configurationPath;
        const auto& selectedApplicationPath =
            *selectedApplication->candidate().applicationExecutable;
        if (selection.status().configurationPath !=
                std::optional<Domain::PathText>{selectedConfigurationPath} ||
            selection.status().applicationExecutable !=
                std::optional<Domain::PathText>{selectedApplicationPath}) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "LM Studio selected paths do not match their retained "
                    "candidate evidence."));
        }

        auto configurationRoot = canonicalParent(
            selectedConfigurationPath,
            "The selected LM Studio configuration");
        if (!configurationRoot) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(configurationRoot).error());
        }
        auto applicationRoot = canonicalParent(
            selectedApplicationPath,
            "The selected LM Studio executable");
        if (!applicationRoot) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(applicationRoot).error());
        }
        auto cliRoot = canonicalParent(
            configuration_.forgeCliExecutable,
            "The Forge Conductor CLI executable");
        if (!cliRoot) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(cliRoot).error());
        }
        auto dataRoot = canonicalRoot(configuration_.forgeDataRoot);
        if (!dataRoot) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(dataRoot).error());
        }

        std::vector<Domain::PathText> roots;
        roots.reserve(MaximumAuthorityRoots);
        std::vector<std::wstring> wideRoots;
        wideRoots.reserve(MaximumAuthorityRoots);
        std::vector<Domain::PathText> requestedRoots;
        requestedRoots.reserve(MaximumAuthorityRoots);
        requestedRoots.push_back(std::move(configurationRoot).value());
        requestedRoots.push_back(std::move(applicationRoot).value());
        requestedRoots.push_back(std::move(cliRoot).value());
        requestedRoots.push_back(std::move(dataRoot).value());
        for (auto& root : requestedRoots) {
            auto appended = appendAuthorityRoot(
                roots, wideRoots, std::move(root));
            if (!appended) {
                return Domain::Result<ManagerLmStudioReadScope>::failure(
                    std::move(appended).error());
            }
        }
        if (roots.empty() || roots.size() > MaximumAuthorityRoots) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "LM Studio maintenance produced an invalid authority "
                    "root count."));
        }

        auto issuer = std::make_unique<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<
                InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                InfrastructureWindows::WindowsWorkspaceAuthorityPolicy{
                    configuration_.readScopeIdentity.authorityId,
                    configuration_.readScopeIdentity.projectId,
                    configuration_.readScopeIdentity.callerId,
                    roots,
                    Domain::FileAccess::Read,
                    {Domain::FileAccess::Read},
                    {Domain::FileAccess::Write,
                     Domain::FileAccess::Create,
                     Domain::FileAccess::Delete,
                     Domain::FileAccess::Execute},
                    false,
                    configuration_.readScopeIdentity.generation}});
        auto authority = issuer->authorityFor(
            configuration_.readScopeIdentity.projectId, context);
        if (!authority) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                std::move(authority).error());
        }
        if (authority.value().authorityId() !=
                configuration_.readScopeIdentity.authorityId ||
            authority.value().projectId() !=
                configuration_.readScopeIdentity.projectId ||
            authority.value().callerId() !=
                configuration_.readScopeIdentity.callerId ||
            authority.value().generation() !=
                configuration_.readScopeIdentity.generation ||
            authority.value().trustedRoots() != roots ||
            !hasExactAccess(authority.value())) {
            return Domain::Result<ManagerLmStudioReadScope>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The issued Manager LM Studio read scope did not retain "
                    "its exact policy."));
        }
        return Domain::Result<ManagerLmStudioReadScope>::success(
            ManagerLmStudioReadScope{
                std::move(issuer), std::move(authority).value()});
    } catch (...) {
        return Domain::Result<ManagerLmStudioReadScope>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Manager LM Studio read scope could not be resolved."));
    }
}

} // namespace ForgeConductor::Composition::Windows
