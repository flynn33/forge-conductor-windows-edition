#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"

#include "NativeToolValidation.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

constexpr std::string_view DefaultCommitMessage =
    "chore: forge-conductor commit";

[[nodiscard]] Domain::Result<void> validateOutputMaximum(
    const std::size_t maximumBytes) noexcept
{
    if (maximumBytes == 0U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The Git output limit must be positive."));
    }
    if (maximumBytes > WindowsGitService::MaximumOutputBytes) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "The Git output limit exceeds 80000 bytes."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateArgument(
    const std::string_view value,
    const bool allowEmpty,
    const std::string_view purpose) noexcept
{
    try {
        if ((!allowEmpty && value.empty()) ||
            value.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(value)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{purpose} + " must be valid NUL-free UTF-8."));
        }
        if (value.size() > WindowsGitService::MaximumArgumentBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                std::string{purpose} + " exceeds 4096 UTF-8 bytes."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Git argument could not be validated."));
    }
}

void enforceOutputBounds(
    Domain::ProcessResult& result,
    const std::size_t maximumStdoutBytes,
    const std::size_t maximumStderrBytes) noexcept
{
    const auto truncate = [](std::string& value, const std::size_t maximumBytes) {
        if (value.size() <= maximumBytes) {
            return false;
        }
        std::size_t boundary = maximumBytes;
        while (boundary > 0U && boundary < value.size() &&
               (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
            --boundary;
        }
        value.resize(boundary);
        return true;
    };
    result.stdoutTruncated =
        truncate(result.stdoutUtf8, maximumStdoutBytes) ||
        result.stdoutTruncated;
    result.stderrTruncated =
        truncate(result.stderrUtf8, maximumStderrBytes) ||
        result.stderrTruncated;
}

} // namespace

WindowsGitService::WindowsGitService(
    Domain::PathText gitExecutable,
    Contracts::WorkspaceAuthority executionAuthority,
    std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor)
    : gitExecutable_{std::move(gitExecutable)},
      executionAuthority_{std::move(executionAuthority)},
      processSupervisor_{std::move(processSupervisor)}
{
}

Domain::Result<Domain::ProcessResult> WindowsGitService::run(
    const Contracts::AuthorizedPath& repository,
    const Domain::FileAccess repositoryAccess,
    std::vector<std::string> arguments,
    const std::size_t maximumStdoutBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto active = Detail::checkContext(context, "Git operation");
        if (!active) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(active).error());
        }
        if (!processSupervisor_) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Git service requires a process supervisor owner."));
        }
        auto executable = Detail::validateExecutable(
            gitExecutable_, executionAuthority_, "Git");
        if (!executable) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(executable).error());
        }
        auto repositoryCapability = Detail::validateAuthorizedPath(
            repository, executionAuthority_, repositoryAccess, "Git repository");
        if (!repositoryCapability) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(repositoryCapability).error());
        }

        Domain::ProcessRequest request{gitExecutable_};
        request.arguments = std::move(arguments);
        request.workingDirectory = repository.canonicalPath();
        request.environment = {{"GIT_TERMINAL_PROMPT", "0"}};
        request.inheritEnvironment = true;
        request.timeout = std::chrono::seconds{30};
        request.maximumStdoutBytes = maximumStdoutBytes;
        request.maximumStderrBytes = MaximumErrorBytes;
        auto outcome =
            processSupervisor_->run(request, executionAuthority_, context);
        if (!outcome) {
            return outcome;
        }
        auto result = std::move(outcome).value();
        enforceOutputBounds(
            result, maximumStdoutBytes, MaximumErrorBytes);
        return Domain::Result<Domain::ProcessResult>::success(
            std::move(result));
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Git process request could not be prepared."));
    }
}

Domain::Result<Domain::ProcessResult> WindowsGitService::status(
    const Contracts::AuthorizedPath& repository,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto bounded = validateOutputMaximum(maximumBytes);
        if (!bounded) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(bounded).error());
        }
        return run(
            repository,
            Domain::FileAccess::Read,
            {"status", "--porcelain=v1", "-b"},
            maximumBytes,
            context);
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Git status request could not be prepared."));
    }
}

Domain::Result<Domain::ProcessResult> WindowsGitService::diff(
    const Contracts::AuthorizedPath& repository,
    const std::span<const std::string> arguments,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto bounded = validateOutputMaximum(maximumBytes);
        if (!bounded) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(bounded).error());
        }
        if (arguments.size() > 1U ||
            (!arguments.empty() && arguments.front() != "--cached")) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Git diff accepts only the macOS-parity --cached option."));
        }
        if (!arguments.empty()) {
            auto valid = validateArgument(arguments.front(), false, "Git diff option");
            if (!valid) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(valid).error());
            }
        }
        std::vector<std::string> processArguments{"diff"};
        processArguments.insert(
            processArguments.end(), arguments.begin(), arguments.end());
        return run(
            repository,
            Domain::FileAccess::Read,
            std::move(processArguments),
            maximumBytes,
            context);
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Git diff request could not be prepared."));
    }
}

Domain::Result<Domain::ProcessResult> WindowsGitService::log(
    const Contracts::AuthorizedPath& repository,
    const std::size_t maximumEntries,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto bounded = validateOutputMaximum(maximumBytes);
        if (!bounded) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(bounded).error());
        }
        if (maximumEntries == 0U) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Git log entry limit must be positive."));
        }
        if (maximumEntries > MaximumLogEntries) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The Git log entry limit exceeds 200."));
        }
        return run(
            repository,
            Domain::FileAccess::Read,
            {"log", "-n", std::to_string(maximumEntries), "--oneline"},
            maximumBytes,
            context);
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Git log request could not be prepared."));
    }
}

Domain::Result<Domain::ProcessResult> WindowsGitService::add(
    const Contracts::AuthorizedPath& repository,
    const std::span<const Contracts::AuthorizedPath> paths,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (paths.size() > MaximumAddPaths) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The Git add path count exceeds 200."));
        }

        std::vector<std::string> arguments{"add"};
        if (paths.empty()) {
            arguments.emplace_back("-A");
        } else {
            arguments.emplace_back("--");
            const auto repositoryKey =
                Detail::normalizedLocalPathKey(repository.canonicalPath());
            if (!repositoryKey) {
                return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The Git repository path is not an absolute local path."));
            }

            std::size_t aggregateBytes =
                arguments.front().size() + arguments.back().size();
            arguments.reserve(paths.size() + 2U);
            for (const auto& path : paths) {
                auto authorized = Detail::validateAuthorizedPath(
                    path, executionAuthority_, Domain::FileAccess::Read, "Git add path");
                if (!authorized) {
                    return Domain::Result<Domain::ProcessResult>::failure(
                        std::move(authorized).error());
                }
                const auto pathKey =
                    Detail::normalizedLocalPathKey(path.canonicalPath());
                if (!pathKey || !Detail::isWithin(pathKey.value(), repositoryKey.value())) {
                    return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                        Domain::ErrorCodes::PathOutsideAuthority,
                        "A Git add path is outside the authorized repository."));
                }
                const auto& value = path.canonicalPath().value();
                auto valid = validateArgument(value, false, "Git add path");
                if (!valid) {
                    return Domain::Result<Domain::ProcessResult>::failure(
                        std::move(valid).error());
                }
                if (value.size() >
                    Domain::MaximumProcessArgumentsBytes - aggregateBytes) {
                    return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "The Git add path arguments exceed 15360 UTF-8 bytes."));
                }
                aggregateBytes += value.size();
                arguments.push_back(value);
            }
        }

        return run(
            repository,
            Domain::FileAccess::Write,
            std::move(arguments),
            MaximumOutputBytes,
            context);
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Git add request could not be prepared."));
    }
}

Domain::Result<Domain::ProcessResult> WindowsGitService::commit(
    const Contracts::AuthorizedPath& repository,
    const std::string_view message,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto selectedMessage = message.empty() ? DefaultCommitMessage : message;
        auto valid = validateArgument(selectedMessage, false, "Git commit message");
        if (!valid) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(valid).error());
        }
        return run(
            repository,
            Domain::FileAccess::Write,
            {"commit", "-m", std::string{selectedMessage}},
            MaximumOutputBytes,
            context);
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Git commit request could not be prepared."));
    }
}

} // namespace ForgeConductor::NativeTools::Windows
