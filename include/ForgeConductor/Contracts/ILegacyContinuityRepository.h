#pragma once

#include "ForgeConductor/Domain/LegacyContinuityModels.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace ForgeConductor::Contracts {

// The central SQLite implementation owns transaction and document-codec
// details. compareExchange must allocate write_sequence and update both
// continuity pointer notes in the same BEGIN IMMEDIATE transaction. A stale
// expectedWriteSequence, or an insert whose id already exists, returns the
// stable Domain::ErrorCodes::Conflict code without mutating durable state.
class ILegacyContinuityRepository {
public:
    virtual ~ILegacyContinuityRepository() = default;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
    get(
        const Domain::LegacyHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
    latest(
        const std::optional<Domain::ClientId>& clientId,
        bool resumeReadyOnly,
        const Domain::OperationContext& context) noexcept = 0;

    // Results are ordered by write_sequence descending, then id ascending for
    // deterministic behavior if an imported database contains a sequence tie.
    [[nodiscard]] virtual Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
    list(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
    listAll(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityRecord>
    compareExchange(
        const Domain::LegacyContinuityCompareExchange& mutation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityPointerRepairOutcome>
    repairPointers(const Domain::OperationContext& context) noexcept = 0;

    // This is the central legacy scope only. The project lifecycle reset is a
    // separate, convergent transaction and must never be folded into this call.
    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityResetOutcome>
    reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept = 0;

    // Closes only this repository's admission. The composition root retains
    // ownership of the shared central database until all attached repositories drain.
    virtual void close() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
