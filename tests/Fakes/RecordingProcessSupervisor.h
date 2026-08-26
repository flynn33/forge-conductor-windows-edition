#pragma once

#include "ForgeConductor/Contracts/IProcessSupervisor.h"
#include "DeterministicResult.h"

#include <cstddef>
#include <optional>

namespace ForgeConductor::Tests::Fakes {

class RecordingProcessSupervisor final
    : public Contracts::IProcessSupervisor {
public:
    DeterministicResult<Domain::ProcessResult> runResult;

    [[nodiscard]] Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            ++runCalls_;
            lastRequest_ = request;
            lastAuthority_.emplace(authority);
            lastOperationId_ = context.operationId;
            if (shutdown_ || cancelAllRequested_ ||
                context.isCancellationRequested() ||
                (lastCancelledOperation_ &&
                 lastCancelledOperation_.value() == context.operationId)) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The deterministic process operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<Domain::ProcessResult>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::DeadlineExceeded,
                        "The deterministic process deadline expired."));
            }
            return runResult.get();
        } catch (...) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The deterministic process call could not be recorded."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            ++cancelCalls_;
            lastCancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void cancelAll() noexcept override
    {
        cancelAllRequested_ = true;
    }

    void shutdown() noexcept override
    {
        shutdown_ = true;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        now_ = now;
    }

    [[nodiscard]] std::size_t runCalls() const noexcept { return runCalls_; }
    [[nodiscard]] std::size_t cancelCalls() const noexcept { return cancelCalls_; }
    [[nodiscard]] bool cancelAllRequested() const noexcept
    {
        return cancelAllRequested_;
    }

    [[nodiscard]] const std::optional<Domain::ProcessRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastAuthority() const noexcept
    {
        return lastAuthority_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastOperationId() const noexcept
    {
        return lastOperationId_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastCancelledOperation() const noexcept
    {
        return lastCancelledOperation_;
    }

private:
    std::optional<Domain::ProcessRequest> lastRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastAuthority_;
    std::optional<Domain::OperationId> lastOperationId_;
    std::optional<Domain::OperationId> lastCancelledOperation_;
    Domain::MonotonicTimePoint now_{};
    std::size_t runCalls_{};
    std::size_t cancelCalls_{};
    bool cancelAllRequested_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
