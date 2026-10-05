#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace ForgeConductor::Hosts::App {

struct RegisteredSetupProject final {
    Domain::ProjectMemoryDescriptor project;
    Domain::PathText alias;
};

namespace ProjectSetupDetail {

[[nodiscard]] inline std::wstring normalizedPath(const Domain::PathText& value)
{
    const auto& utf8 = value.value();
    auto path = std::filesystem::path{std::u8string_view{
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}}.lexically_normal();
    if (!path.is_absolute()) return {};
    path.make_preferred();
    auto text = path.native();
    while (text.size() > path.root_path().native().size() && text.back() == L'\\')
        text.pop_back();
    return text;
}

[[nodiscard]] inline bool samePath(const Domain::PathText& first,
                                 const Domain::PathText& second)
{
    const auto left = normalizedPath(first);
    const auto right = normalizedPath(second);
    return !left.empty() && !right.empty() &&
        ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
            right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

} // namespace ProjectSetupDetail

[[nodiscard]] inline Domain::Result<std::optional<RegisteredSetupProject>>
registeredProjectForSetupFolder(
    const Domain::PathText& folder,
    const std::span<const Domain::ProjectMemoryDescriptor> projects) noexcept
{
    using Result = Domain::Result<std::optional<RegisteredSetupProject>>;
    try {
        if (ProjectSetupDetail::normalizedPath(folder).empty())
            return Result::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                "Project preparation requires an absolute workspace folder."));
        std::optional<RegisteredSetupProject> selected;
        for (const auto& project : projects) {
            for (const auto& alias : project.aliases) {
                if (!ProjectSetupDetail::samePath(folder, alias)) continue;
                if (selected && selected->project.id != project.id)
                    return Result::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                        "The workspace folder belongs to multiple registered projects. Open Diagnostics before preparing it."));
                if (!selected) selected = RegisteredSetupProject{project, alias};
            }
        }
        return Result::success(std::move(selected));
    } catch (...) {
        return Result::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
            "Registered project folders could not be compared."));
    }
}

[[nodiscard]] inline Domain::Result<Domain::PathText> authorizeRegisteredSetupFolder(
    const RegisteredSetupProject& selected,
    const Domain::PathText& folder,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    const Domain::OperationContext& context) noexcept
{
    using Result = Domain::Result<Domain::PathText>;
    try {
        if (!ProjectSetupDetail::samePath(folder, selected.alias) ||
            std::none_of(selected.project.aliases.begin(), selected.project.aliases.end(),
                [&](const Domain::PathText& alias) {
                    return ProjectSetupDetail::samePath(alias, selected.alias);
                }))
            return Result::failure(Domain::makeError(Domain::ErrorCodes::ProjectScopeMismatch,
                "Project preparation must use an exact registered workspace folder."));
        auto authority = workspaceAuthority.authorityFor(selected.project.id, context);
        if (!authority) return Result::failure(authority.error());
        auto authorized = workspaceAuthority.authorize(authority.value(),
            Domain::PathAuthorizationRequest{folder, selected.alias,
                Domain::FileAccess::Read, false}, context);
        if (!authorized) return Result::failure(authorized.error());
        if (!ProjectSetupDetail::samePath(authorized.value().canonicalPath(),
                authorized.value().authorityRoot()) ||
            !ProjectSetupDetail::samePath(authorized.value().canonicalPath(), selected.alias))
            return Result::failure(Domain::makeError(Domain::ErrorCodes::ProjectScopeMismatch,
                "The prepared folder does not resolve to its exact registered workspace root."));
        return Result::success(authorized.value().canonicalPath());
    } catch (...) {
        return Result::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
            "The registered workspace folder could not be authorized."));
    }
}

} // namespace ForgeConductor::Hosts::App
