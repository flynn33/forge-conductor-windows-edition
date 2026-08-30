#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace ForgeConductor::Composition::Windows {

// Truthful optional-host adapter used when native LM Studio deployment cannot
// be composed. It owns no worker, callback, queue, or host process.
class UnavailableLmStudioDeploymentService final
    : public Contracts::ILMStudioDeploymentService {
public:
    static constexpr std::size_t MaximumReasonBytes = 512U;

    UnavailableLmStudioDeploymentService(
        const Contracts::IClock& clock,
        std::string reason);
    ~UnavailableLmStudioDeploymentService() noexcept override;

    UnavailableLmStudioDeploymentService(
        const UnavailableLmStudioDeploymentService&) = delete;
    UnavailableLmStudioDeploymentService& operator=(
        const UnavailableLmStudioDeploymentService&) = delete;
    UnavailableLmStudioDeploymentService(
        UnavailableLmStudioDeploymentService&&) = delete;
    UnavailableLmStudioDeploymentService& operator=(
        UnavailableLmStudioDeploymentService&&) = delete;

    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    [[nodiscard]] Domain::Result<Domain::LMStudioPluginStatus> status(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioHostActivationRequest& request,
        const Contracts::WorkspaceAuthority& executionAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;

    // At most one exact cancellation identifier is retained. Calls perform no
    // background work, so shutdown only closes admission and is idempotent.
    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept;

    const Contracts::IClock& clock_;
    const std::string reason_;
    mutable std::mutex stateMutex_;
    std::optional<Domain::OperationId> cancelledOperation_;
    bool shutdown_{};
};

} // namespace ForgeConductor::Composition::Windows
