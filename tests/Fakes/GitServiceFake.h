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

    DeterministicResult<Domain::ProcessResult> statusResult;
    DeterministicResult<Domain::ProcessResult> diffResult;
    DeterministicResult<Domain::ProcessResult> logResult;
    DeterministicResult<Domain::ProcessResult> addResult;
    DeterministicResult<Domain::ProcessResult> commitResult;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> status(
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        return simpleProcessCall(
            GitServiceCall::Status,
            repository,
            0,
            maximumBytes,
            statusResult,
            context);
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult> diff(
        const Contracts::AuthorizedPath& repository,
        const std::span<const std::string> arguments,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::ProcessResult>::failure(
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
                return Domain::Result<Domain::ProcessResult>::failure(
                    boundaryLimitExceeded(
                        "The git diff arguments exceed their capture bound."));
            }
            return boundedProcess(diffResult, maximumBytes);
        } catch (...) {
            return processFailure(
                "The deterministic git diff could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult> log(
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumEntries,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        if (maximumEntries > captureItemsMaximum_) {
            try {
                auto gate = state_.begin(context);
                if (!gate) {
                    return Domain::Result<Domain::ProcessResult>::failure(
                        std::move(gate).error());
                }
                captureBasic(
                    GitServiceCall::Log,
                    repository,
                    maximumEntries,
                    maximumBytes);
                return Domain::Result<Domain::ProcessResult>::failure(
                    boundaryLimitExceeded(
                        "The git log entry request exceeds its bound."));
            } catch (...) {
                return processFailure(
                    "The deterministic git log could not be captured.");
            }
        }
        return simpleProcessCall(
            GitServiceCall::Log,
            repository,
            maximumEntries,
            maximumBytes,
            logResult,
            context);
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult> add(
        const Contracts::AuthorizedPath& repository,
        const std::span<const Contracts::AuthorizedPath> paths,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(gate).error());
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
                return Domain::Result<Domain::ProcessResult>::failure(
                    boundaryLimitExceeded(
                        "The git add path set exceeds its capture bound."));
            }
            return boundedProcess(addResult, outputBytesMaximum_);
        } catch (...) {
            return processFailure(
                "The deterministic git add could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult> commit(
        const Contracts::AuthorizedPath& repository,
        const std::string_view message,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::ProcessResult>::failure(
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
                return Domain::Result<Domain::ProcessResult>::failure(
                    boundaryPayloadTooLarge(
                        "The git commit message exceeds its capture bound."));
            }
            return boundedProcess(commitResult, outputBytesMaximum_);
        } catch (...) {
            return processFailure(
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
    [[nodiscard]] Domain::Result<Domain::ProcessResult> simpleProcessCall(
        const GitServiceCall call,
        const Contracts::AuthorizedPath& repository,
        const std::size_t maximumEntries,
        const std::size_t maximumBytes,
        const DeterministicResult<Domain::ProcessResult>& scripted,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(gate).error());
            }
            captureBasic(call, repository, maximumEntries, maximumBytes);
            return boundedProcess(scripted, maximumBytes);
        } catch (...) {
            return processFailure(
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

    [[nodiscard]] Domain::Result<Domain::ProcessResult> boundedProcess(
        const DeterministicResult<Domain::ProcessResult>& scripted,
        const std::size_t maximumBytes) const
    {
        auto result = scripted.get();
        if (!result) {
            return result;
        }
        auto process = std::move(result).value();
        capText(
            process.stdoutUtf8,
            (std::min)(maximumBytes, outputBytesMaximum_),
            process.stdoutTruncated);
        capText(
            process.stderrUtf8,
            outputBytesMaximum_,
            process.stderrTruncated);
        return Domain::Result<Domain::ProcessResult>::success(
            std::move(process));
    }

    static void capText(
        std::string& value,
        const std::size_t maximumBytes,
        bool& truncated) noexcept
    {
        if (value.size() <= maximumBytes) {
            return;
        }
        auto retainedBytes = maximumBytes;
        while (retainedBytes > 0U && retainedBytes < value.size() &&
               (static_cast<unsigned char>(value[retainedBytes]) & 0xC0U) ==
                   0x80U) {
            --retainedBytes;
        }
        value.resize(retainedBytes);
        truncated = true;
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

    [[nodiscard]] static Domain::Result<Domain::ProcessResult> processFailure(
        const char* message)
    {
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureItemsMaximum_;
    std::size_t captureTextBytesMaximum_;
    std::size_t outputBytesMaximum_;
    std::optional<GitServiceCapture> lastCapture_;
};

} // namespace ForgeConductor::Tests::Fakes
