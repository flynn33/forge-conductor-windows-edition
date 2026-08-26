#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

enum class GitServiceCall {
    Status,
    Diff,
    Log,
    Add,
    Commit
};

struct GitServiceCapture final {
    GitServiceCall call;
    Contracts::AuthorizedPath repository;
    std::vector<std::string> arguments;
    std::vector<Contracts::AuthorizedPath> paths;
    std::string message;
    std::size_t requestedItems{};
    std::size_t requestedTextBytes{};
    std::size_t maximumEntries{};
    std::size_t maximumBytes{};
};

class RecordingGitServiceFake final
    : public Contracts::IGitService {
public:
    explicit RecordingGitServiceFake(
        const std::size_t captureItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum,
        const std::size_t captureTextBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum,
        const std::size_t outputBytesMaximum =
            DefaultBoundaryCaptureBytesMaximum) noexcept
        : captureItemsMaximum_{captureItemsMaximum},
          captureTextBytesMaximum_{captureTextBytesMaximum},
          outputBytesMaximum_{outputBytesMaximum}
    {
    }

    DeterministicResult<std::string> statusResult;
    DeterministicResult<std::string> diffResult;
    DeterministicResult<std::string> logResult;
    DeterministicResult<void> addResult;
    DeterministicResult<std::string> commitResult;

    [[nodiscard]] Domain::Result<std::string> status(
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        return simpleStringCall(
            GitServiceCall::Status,
            repository,
            0,
            maximumBytes,
            statusResult,
            context);
    }

    [[nodiscard]] Domain::Result<std::string> diff(
        const Contracts::AuthorizedPath& repository,
        const std::span<const std::string> arguments,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<std::string>::failure(
                    std::move(gate).error());
            }
            const auto bytes = textBytes(arguments);
            const auto bounded =
                arguments.size() <= captureItemsMaximum_ &&
                bytes <= captureTextBytesMaximum_;
            lastCapture_.emplace(GitServiceCapture{
                GitServiceCall::Diff,
                repository,
                bounded
                    ? std::vector<std::string>{
                          arguments.begin(),
                          arguments.end()}
                    : std::vector<std::string>{},
                {},
                {},
                arguments.size(),
                bytes,
                0,
                maximumBytes});
            if (!bounded) {
                return Domain::Result<std::string>::failure(
                    boundaryLimitExceeded(
                        "The git diff arguments exceed their capture bound."));
            }
            return boundedString(diffResult, maximumBytes);
        } catch (...) {
            return stringFailure(
                "The deterministic git diff could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::string> log(
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumEntries,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        if (maximumEntries > captureItemsMaximum_) {
            try {
                auto gate = state_.begin(context);
                if (!gate) {
                    return Domain::Result<std::string>::failure(
                        std::move(gate).error());
                }
                captureBasic(
                    GitServiceCall::Log,
                    repository,
                    maximumEntries,
                    maximumBytes);
                return Domain::Result<std::string>::failure(
                    boundaryLimitExceeded(
                        "The git log entry request exceeds its bound."));
            } catch (...) {
                return stringFailure(
                    "The deterministic git log could not be captured.");
            }
        }
        return simpleStringCall(
            GitServiceCall::Log,
            repository,
            maximumEntries,
            maximumBytes,
            logResult,
            context);
    }

    [[nodiscard]] Domain::Result<void> add(
        const Contracts::AuthorizedPath& repository,
        const std::span<const Contracts::AuthorizedPath> paths,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            const auto bounded = paths.size() <= captureItemsMaximum_;
            lastCapture_.emplace(GitServiceCapture{
                GitServiceCall::Add,
                repository,
                {},
                bounded
                    ? std::vector<Contracts::AuthorizedPath>{
                          paths.begin(),
                          paths.end()}
                    : std::vector<Contracts::AuthorizedPath>{},
                {},
                paths.size(),
                0,
                0,
                0});
            if (!bounded) {
                return Domain::Result<void>::failure(boundaryLimitExceeded(
                    "The git add path set exceeds its capture bound."));
            }
            return addResult.get();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic git add could not be captured."));
        }
    }

    [[nodiscard]] Domain::Result<std::string> commit(
        const Contracts::AuthorizedPath& repository,
        const std::string_view message,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<std::string>::failure(
                    std::move(gate).error());
            }
            const auto capturedBytes =
                (std::min)(message.size(), captureTextBytesMaximum_);
            lastCapture_.emplace(GitServiceCapture{
                GitServiceCall::Commit,
                repository,
                {},
                {},
                std::string{message.substr(0, capturedBytes)},
                0,
                message.size(),
                0,
                0});
            if (message.size() > captureTextBytesMaximum_) {
                return Domain::Result<std::string>::failure(
                    boundaryPayloadTooLarge(
                        "The git commit message exceeds its capture bound."));
            }
            return boundedString(commitResult, outputBytesMaximum_);
        } catch (...) {
            return stringFailure(
                "The deterministic git commit could not be captured.");
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<GitServiceCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    [[nodiscard]] Domain::Result<std::string> simpleStringCall(
        const GitServiceCall call,
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumEntries,
        const std::size_t maximumBytes,
        const DeterministicResult<std::string>& scripted,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<std::string>::failure(
                    std::move(gate).error());
            }
            captureBasic(call, repository, maximumEntries, maximumBytes);
            return boundedString(scripted, maximumBytes);
        } catch (...) {
            return stringFailure(
                "The deterministic git request could not be captured.");
        }
    }

    void captureBasic(
        const GitServiceCall call,
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumEntries,
        const std::size_t maximumBytes)
    {
        lastCapture_.emplace(GitServiceCapture{
            call,
            repository,
            {},
            {},
            {},
            0,
            0,
            maximumEntries,
            maximumBytes});
    }

    [[nodiscard]] Domain::Result<std::string> boundedString(
        const DeterministicResult<std::string>& scripted,
        const std::size_t maximumBytes) const
    {
        auto result = scripted.get();
        if (result &&
            (result.value().size() > maximumBytes ||
             result.value().size() > outputBytesMaximum_)) {
            return Domain::Result<std::string>::failure(
                boundaryPayloadTooLarge(
                    "The scripted git response exceeds its output bound."));
        }
        return result;
    }

    [[nodiscard]] static std::size_t textBytes(
        const std::span<const std::string> values) noexcept
    {
        std::size_t bytes{};
        for (const auto& value : values) {
            bytes += value.size();
        }
        return bytes;
    }

    [[nodiscard]] static Domain::Result<std::string> stringFailure(
        const char* message)
    {
        return Domain::Result<std::string>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureItemsMaximum_;
    std::size_t captureTextBytesMaximum_;
    std::size_t outputBytesMaximum_;
    std::optional<GitServiceCapture> lastCapture_;
};

} // namespace ForgeConductor::Tests::Fakes
