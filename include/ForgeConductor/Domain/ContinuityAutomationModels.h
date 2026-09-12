#pragma once

#include "ForgeConductor/Domain/ContinuityModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumContinuityAutomationImplicitRoots = 16U;

struct ContinuityAutomationPolicy final {
    double checkpointReserveFraction{0.20};
    double rolloverReserveFraction{0.10};
};

// One provider-budget observation paired with the canonical handoff that is
// persisted if the budget policy requests a checkpoint or rollover.
struct ContinuityAutomationObservation final {
    ContinuityHandoff handoff;
    ContextBudgetSignals budgetSignals;
    bool forceCheckpoint{};
};

// A compact immutable-by-convention summary of the synchronous automation
// decision. The durable coordinator outcomes remain the source of truth.
struct ContinuityAutomationOutcome final {
    ProjectId projectId;
    ContinuityHandoffId handoffId;
    ContextBudgetAction action{ContextBudgetAction::Normal};
    std::optional<ContinuityOperationId> operationId;
    std::optional<SessionId> successorSessionId;
    bool checkpointPersisted{};
    bool rolloverRequested{};
    bool successorActivated{};
    std::optional<ProviderSessionId> successorProviderResponseId;
};

// Compact context-only continuity status. Count and time rollover controls are
// intentionally absent from the Alpha product.
struct ContinuityAutomationStatusSnapshot final {
    bool enabled{true};
    bool blocked{};
    std::optional<std::string> handoffId;
    std::vector<PathText> implicitRoots;

    bool operator==(const ContinuityAutomationStatusSnapshot&) const = default;
};

} // namespace ForgeConductor::Domain
