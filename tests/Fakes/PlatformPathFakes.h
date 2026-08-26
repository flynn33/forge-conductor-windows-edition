#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"

#include <optional>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

class RecordingApplicationPathsFake final
    : public Contracts::IApplicationPaths {
public:
    DeterministicResult<Domain::PathText> dataRootResult;
    DeterministicResult<Domain::PathText> configurationRootResult;
    DeterministicResult<Domain::PathText> diagnosticsRootResult;
    DeterministicResult<Domain::PathText> projectRootResult;

    [[nodiscard]] Domain::Result<Domain::PathText> dataRoot(
        const Domain::OperationContext& context) noexcept override
    {
        return pathResult(dataRootResult, context);
    }

    [[nodiscard]] Domain::Result<Domain::PathText> configurationRoot(
        const Domain::OperationContext& context) noexcept override
    {
        return pathResult(configurationRootResult, context);
    }

    [[nodiscard]] Domain::Result<Domain::PathText> diagnosticsRoot(
        const Domain::OperationContext& context) noexcept override
    {
        return pathResult(diagnosticsRootResult, context);
    }

    [[nodiscard]] Domain::Result<Domain::PathText> projectRoot(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastProjectId_ = projectId;
            return pathResult(projectRootResult, context);
        } catch (...) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic project path request could not be captured."));
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

    [[nodiscard]] const std::optional<Domain::ProjectId>&
    lastProjectId() const noexcept
    {
        return lastProjectId_;
    }

private:
    [[nodiscard]] Domain::Result<Domain::PathText> pathResult(
        const DeterministicResult<Domain::PathText>& scripted,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return Domain::Result<Domain::PathText>::failure(
                    std::move(gate).error());
            }
            return scripted.get();
        } catch (...) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic application path result could not be returned."));
        }
    }

    DeterministicBoundaryState state_;
    std::optional<Domain::ProjectId> lastProjectId_;
};

} // namespace ForgeConductor::Tests::Fakes
