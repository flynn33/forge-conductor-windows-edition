#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IContinuityAutomation.h"
#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/IToolServices.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ForgeConductor::Application {

struct ManagedRunToolDependencies final {
    Contracts::IToolCatalog* catalog{};
    Contracts::IToolRouter* router{};
    Contracts::IWorkspaceAuthority* workspaceAuthority{};
};

struct ManagedRunContinuityDependencies final {
    Contracts::IContinuityAutomation* automation{};
    Contracts::IContinuityDocumentCodec* codec{};
    Contracts::IProjectRegistryRepository* projects{};
    std::optional<Domain::AdapterId> adapterId;
    std::uint64_t contextCapacity{};
    std::uint64_t reservedTokens{};
    std::optional<std::string> model;
    std::optional<std::string> provider;
};

class ManagedRunService final : public Contracts::IManagedRunService {
public:
    ManagedRunService(
        Contracts::IManagedResponsesTransport& transport,
        Contracts::IManagedRunStore& store,
        Contracts::IClock& clock,
        ManagedRunToolDependencies tools = {},
        ManagedRunContinuityDependencies continuity = {});
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

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> pause(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> resume(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
