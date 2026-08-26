#pragma once

#include "ForgeConductor/Domain/LegacyMemoryModels.h"
#include "ForgeConductor/Domain/ProjectMemoryModels.h"

#include <string_view>

namespace ForgeConductor::Contracts {

class ILegacyMemoryRepository {
public:
    virtual ~ILegacyMemoryRepository() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemorySetOutcome> upsert(
        const Domain::LegacyMemoryUpsert& note,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        std::string_view key,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListQuery& query,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        std::string_view key,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchQuery& query,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
