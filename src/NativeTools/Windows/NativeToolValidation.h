#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/Domain.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
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

[[nodiscard]] inline Domain::Result<void> validateBoundAuthority(
    const Contracts::WorkspaceAuthority& expected,
    const Contracts::WorkspaceAuthority& actual) noexcept
{
    if (expected.projectId() != actual.projectId()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::ProjectScopeMismatch,
            "The workspace authority belongs to a different project."));
    }
    if (!authoritiesMatch(expected, actual)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Unauthorized,
            "The workspace authority does not match the service binding."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] inline Domain::Result<void> validateExecutable(
    const Domain::PathText& executable,
    const Contracts::WorkspaceAuthority& authority,
    const std::string_view toolName) noexcept
{
    try {
        if (!normalizedLocalPathKey(executable)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{toolName} + " executable must be an explicit absolute local path."));
        }
        if (!isAuthorizedLocalPath(executable, authority)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                std::string{toolName} + " executable is outside the bound authority roots."));
        }
        if (!authority.shellEnabled() ||
            !containsAccess(authority.grants(), Domain::FileAccess::Execute) ||
            containsAccess(authority.denials(), Domain::FileAccess::Execute)) {
            return Domain::Result<void>::failure(Domain::makeError(
                authority.shellEnabled()
                    ? Domain::ErrorCodes::Unauthorized
                    : Domain::ErrorCodes::ShellDisabled,
                std::string{toolName} + " execution is not permitted by the bound authority."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The native-tool executable binding could not be validated."));
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
