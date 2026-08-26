#pragma once

#include "ForgeConductor/Domain/LegacyMemoryModels.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"

namespace ForgeConductor::Contracts {

class ILegacyMemoryService {
public:
    virtual ~ILegacyMemoryService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemorySetOutcome> set(
        const Domain::LegacyMemorySetRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const Domain::LegacyMemoryGetRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const Domain::LegacyMemoryRemoveRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
