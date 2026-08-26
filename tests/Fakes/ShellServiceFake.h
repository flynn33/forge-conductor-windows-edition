#pragma once

#include "ForgeConductor/Contracts/INativeToolServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

struct ShellExecutionCapture final {
    Contracts::WorkspaceAuthority authority;
    std::optional<Domain::ProcessRequest> request;
    std::size_t requestedItems{};
    std::size_t requestedTextBytes{};
};

class RecordingShellServiceFake final
    : public Contracts::IShellService {
public:
    explicit RecordingShellServiceFake(
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

    DeterministicResult<Domain::ProcessResult> executeResult;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> execute(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(gate).error());
            }
            if (lastCancelledOperation_ &&
                lastCancelledOperation_.value() == context.operationId) {
                return cancelled(
                    "The deterministic shell operation was explicitly cancelled.");
            }

            const auto items = request.arguments.size() + request.environment.size();
            const auto textBytes = requestTextBytes(request);
            const auto bounded =
                items <= captureItemsMaximum_ &&
                textBytes <= captureTextBytesMaximum_;
            lastExecution_.emplace(ShellExecutionCapture{
                authority,
                bounded
                    ? std::optional<Domain::ProcessRequest>{request}
                    : std::nullopt,
                items,
                textBytes});
            if (!bounded) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    boundaryLimitExceeded(
                        "The shell request exceeds its capture bound."));
            }
            if (!authority.shellEnabled() ||
                !contains(authority.grants(), Domain::FileAccess::Execute) ||
                contains(authority.denials(), Domain::FileAccess::Execute)) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ShellDisabled,
                        "The deterministic shell authority does not permit execution."));
            }

            auto result = executeResult.get();
            if (result &&
                (result.value().stdoutUtf8.size() > request.maximumStdoutBytes ||
                 result.value().stderrUtf8.size() > request.maximumStderrBytes ||
                 result.value().stdoutUtf8.size() > outputBytesMaximum_ ||
                 result.value().stderrUtf8.size() > outputBytesMaximum_)) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    boundaryPayloadTooLarge(
                        "The scripted shell output exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic shell execution could not be captured."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            lastCancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void shutdown() noexcept override { state_.shutdown(); }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] bool isShutdown() const noexcept { return state_.isShutdown(); }
    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<ShellExecutionCapture>&
    lastExecution() const noexcept
    {
        return lastExecution_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastCancelledOperation() const noexcept
    {
        return lastCancelledOperation_;
    }

private:
    [[nodiscard]] static bool contains(
        const std::vector<Domain::FileAccess>& values,
        const Domain::FileAccess candidate) noexcept
    {
        return std::find(values.begin(), values.end(), candidate) != values.end();
    }

    [[nodiscard]] static std::size_t requestTextBytes(
        const Domain::ProcessRequest& request) noexcept
    {
        std::size_t bytes = request.executable.value().size();
        if (request.workingDirectory) {
            bytes += request.workingDirectory->value().size();
        }
        for (const auto& argument : request.arguments) {
            bytes += argument.size();
        }
        for (const auto& variable : request.environment) {
            bytes += variable.name.size();
            bytes += variable.value.size();
        }
        return bytes;
    }

    [[nodiscard]] static Domain::Result<Domain::ProcessResult> cancelled(
        const char* message)
    {
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(Domain::ErrorCodes::Cancelled, message));
    }

    DeterministicBoundaryState state_;
    std::size_t captureItemsMaximum_;
    std::size_t captureTextBytesMaximum_;
    std::size_t outputBytesMaximum_;
    std::optional<ShellExecutionCapture> lastExecution_;
    std::optional<Domain::OperationId> lastCancelledOperation_;
};

} // namespace ForgeConductor::Tests::Fakes
