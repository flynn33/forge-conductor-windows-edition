#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Domain/Domain.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows::Detail {

[[nodiscard]] inline bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess candidate) noexcept
{
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

[[nodiscard]] inline Domain::Result<void> checkContext(
    const Domain::OperationContext& context,
    const std::string_view operation) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                std::string{operation} + " was cancelled before execution."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                std::string{operation} + " exceeded its deadline before execution."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The native-tool operation context could not be validated."));
    }
}

[[nodiscard]] inline std::optional<std::string> normalizedLocalPathKey(
    const Domain::PathText& path) noexcept
{
    try {
        const auto& value = path.value();
        if (value.size() < 3U ||
            std::isalpha(static_cast<unsigned char>(value[0])) == 0 ||
            value[1] != ':' || (value[2] != '\\' && value[2] != '/')) {
            return std::nullopt;
        }

        std::string normalized;
        normalized.reserve(value.size());
        std::size_t componentStart{};
        for (std::size_t index = 0U; index <= value.size(); ++index) {
            const auto atEnd = index == value.size();
            const auto separator = !atEnd && (value[index] == '\\' || value[index] == '/');
            if (!atEnd && !separator) {
                continue;
            }
            const auto component = std::string_view{value}.substr(
                componentStart, index - componentStart);
            if (component == "." || component == "..") {
                return std::nullopt;
            }
            componentStart = index + 1U;
        }

        for (const auto character : value) {
            const auto normalizedCharacter = character == '/' ? '\\' : character;
            normalized.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(normalizedCharacter))));
        }
        while (normalized.size() > 3U && normalized.back() == '\\') {
            normalized.pop_back();
        }
        return normalized;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] inline bool isWithin(
    const std::string_view candidate,
    const std::string_view root) noexcept
{
    if (candidate == root) {
        return true;
    }
    if (candidate.size() <= root.size() || !candidate.starts_with(root)) {
        return false;
    }
    return root.ends_with('\\') || candidate[root.size()] == '\\';
}

[[nodiscard]] inline bool isAuthorizedLocalPath(
    const Domain::PathText& candidate,
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const auto normalizedCandidate = normalizedLocalPathKey(candidate);
    if (!normalizedCandidate) {
        return false;
    }
    return std::any_of(
        authority.trustedRoots().begin(),
        authority.trustedRoots().end(),
        [&](const Domain::PathText& root) {
            const auto normalizedRoot = normalizedLocalPathKey(root);
            return normalizedRoot &&
                   isWithin(normalizedCandidate.value(), normalizedRoot.value());
        });
}

[[nodiscard]] inline bool authoritiesMatch(
    const Contracts::WorkspaceAuthority& expected,
    const Contracts::WorkspaceAuthority& actual) noexcept
{
    return expected.authorityId() == actual.authorityId() &&
           expected.projectId() == actual.projectId() &&
           expected.callerId() == actual.callerId() &&
           expected.generation() == actual.generation();
}

class PrivateExecutionAuthorityIssuer
    : private Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Domain::Result<Contracts::WorkspaceAuthority> issue(
        const Contracts::WorkspaceAuthority& caller,
        std::vector<Domain::PathText> trustedRoots)
    {
        return issueAuthority(
            caller.authorityId(),
            caller.projectId(),
            caller.callerId(),
            std::move(trustedRoots),
            Domain::FileAccess::Execute,
            {Domain::FileAccess::Execute},
            {},
            true,
            caller.generation());
    }
};

[[nodiscard]] inline Domain::Result<Domain::PathText> executableParent(
    const Domain::PathText& executable,
    const std::string_view toolName) noexcept
{
    try {
        if (!normalizedLocalPathKey(executable)) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{toolName} +
                    " executable must be an explicit absolute local path."));
        }
        const auto separator = executable.value().find_last_of("\\/");
        if (separator == std::string::npos || separator < 2U ||
            separator + 1U >= executable.value().size()) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{toolName} +
                    " executable must name a file in an absolute directory."));
        }
        const auto parentLength = separator == 2U ? 3U : separator;
        auto parent = Domain::PathText::create(
            executable.value().substr(0U, parentLength));
        if (!parent) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(parent).error());
        }
        return Domain::Result<Domain::PathText>::success(
            std::move(parent).value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The native-tool executable parent could not be derived."));
    }
}

[[nodiscard]] inline Domain::Result<Contracts::WorkspaceAuthority>
derivePrivateExecutionAuthority(
    const Contracts::WorkspaceAuthority& caller,
    const Domain::PathText& executable,
    const std::string_view toolName) noexcept
{
    try {
        auto parent = executableParent(executable, toolName);
        if (!parent) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                std::move(parent).error());
        }
        std::vector<Domain::PathText> roots;
        roots.reserve(caller.trustedRoots().size() + 1U);
        for (const auto& root : caller.trustedRoots()) {
            if (!normalizedLocalPathKey(root)) {
                return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The caller authority contains a non-local trusted root."));
            }
            roots.push_back(root);
        }
        const auto parentKey = normalizedLocalPathKey(parent.value());
        const auto alreadyPresent = std::any_of(
            roots.begin(), roots.end(), [&](const Domain::PathText& root) {
                const auto rootKey = normalizedLocalPathKey(root);
                return rootKey && parentKey && rootKey.value() == parentKey.value();
            });
        if (!alreadyPresent) {
            roots.push_back(std::move(parent).value());
        }
        auto issued = PrivateExecutionAuthorityIssuer::issue(
            caller, std::move(roots));
        if (!issued) {
            return issued;
        }
        if (!authoritiesMatch(caller, issued.value())) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The private execution authority identity did not match its caller."));
        }
        return issued;
    } catch (...) {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The private native-tool execution authority could not be derived."));
    }
}

[[nodiscard]] inline Domain::Result<void> validateAuthorizedPath(
    const Contracts::AuthorizedPath& path,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::FileAccess requiredAccess,
    const std::string_view purpose) noexcept
{
    try {
        if (path.authorityId() != authority.authorityId()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                std::string{purpose} + " carries a different authority identifier."));
        }
        if (path.access() != requiredAccess) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                std::string{purpose} + " does not carry the required access capability."));
        }
        const auto candidate = normalizedLocalPathKey(path.canonicalPath());
        const auto root = normalizedLocalPathKey(path.authorityRoot());
        if (!candidate || !root || !isWithin(candidate.value(), root.value())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                std::string{purpose} + " is not inside its canonical authority root."));
        }
        const auto trusted = std::any_of(
            authority.trustedRoots().begin(),
            authority.trustedRoots().end(),
            [&](const Domain::PathText& trustedRoot) {
                const auto normalized = normalizedLocalPathKey(trustedRoot);
                return normalized && normalized.value() == root.value();
            });
        if (!trusted) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                std::string{purpose} + " is bound to an untrusted root."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The native-tool path capability could not be validated."));
    }
}

[[nodiscard]] inline Domain::Result<void> validateWorkingDirectory(
    const Domain::PathText& workingDirectory,
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    if (!normalizedLocalPathKey(workingDirectory)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The working directory must be an explicit absolute local path."));
    }
    if (!isAuthorizedLocalPath(workingDirectory, authority)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The working directory is outside the bound authority roots."));
    }
    return Domain::Result<void>::success();
}

} // namespace ForgeConductor::NativeTools::Windows::Detail
