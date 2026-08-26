#pragma once

#include "ForgeConductor/Domain/ContinuityModels.h"

#include <optional>

namespace ForgeConductor::Domain {

// One provider-budget observation paired with the canonical handoff that is
// persisted if the budget policy requests a checkpoint or rollover.
struct ContinuityAutomationObservation final {
    ContinuityHandoff handoff;
    ContextBudget budget;
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
};

} // namespace ForgeConductor::Domain
