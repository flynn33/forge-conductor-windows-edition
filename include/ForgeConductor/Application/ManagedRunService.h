#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"

#include <memory>

namespace ForgeConductor::Application {

class ManagedRunService final : public Contracts::IManagedRunService {
public:
    ManagedRunService(
        Contracts::IManagedResponsesTransport& transport,
        Contracts::IManagedRunStore& store,
        Contracts::IClock& clock);
    ~ManagedRunService() noexcept override;

    ManagedRunService(const ManagedRunService&) = delete;
    ManagedRunService& operator=(const ManagedRunService&) = delete;
    ManagedRunService(ManagedRunService&&) = delete;
    ManagedRunService& operator=(ManagedRunService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> start(
        const Domain::ManagedRunStartRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
