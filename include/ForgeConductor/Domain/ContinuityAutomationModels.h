#pragma once

#include "ForgeConductor/Domain/ContinuityModels.h"

#include <cstdint>
#include <optional>

namespace ForgeConductor::Domain {

inline constexpr std::uint32_t DefaultCheckpointProgressInterval = 50U;
inline constexpr std::uint32_t DefaultRolloverProgressInterval = 200U;
inline constexpr std::uint32_t DefaultCheckpointIntervalSeconds = 1'800U;
inline constexpr std::uint32_t DefaultRolloverIntervalSeconds = 7'200U;
inline constexpr std::uint32_t MaximumProgressUnitsPerObservation = 1'024U;

struct ContinuityAutomationPolicy final {
    std::uint32_t checkpointProgressInterval{
        DefaultCheckpointProgressInterval};
    std::uint32_t rolloverProgressInterval{
        DefaultRolloverProgressInterval};
    std::uint32_t checkpointIntervalSeconds{
        DefaultCheckpointIntervalSeconds};
    std::uint32_t rolloverIntervalSeconds{
        DefaultRolloverIntervalSeconds};
    double checkpointReserveFraction{0.20};
    double rolloverReserveFraction{0.10};
};

// One provider-budget observation paired with the canonical handoff that is
// persisted if the budget policy requests a checkpoint or rollover.
struct ContinuityAutomationObservation final {
    ContinuityHandoff handoff;
    ContextBudgetSignals budgetSignals;
    std::uint32_t completedProgressUnits{};
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
};

} // namespace ForgeConductor::Domain
