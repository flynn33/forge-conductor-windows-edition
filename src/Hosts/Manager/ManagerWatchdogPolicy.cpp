#include "ManagerWatchdogPolicy.h"

#include <algorithm>

namespace ForgeConductor::Hosts::Manager {

ManagerWatchdogAction ManagerWatchdogPolicy::decide(
    const Domain::ManagerControllerSnapshot& snapshot) noexcept
{
    if (snapshot.shutdownRequested) {
        return ManagerWatchdogAction::None;
    }

    const auto& status = snapshot.status;

    // The loopback control plane remains operable independently of the
    // desired operational-service state and of automatic restart policy.
    if (!status.httpListening) {
        return ManagerWatchdogAction::Repair;
    }

    // Desired-stopped is authoritative. An erroneously active operational
    // service is always reconciled, even when automatic restart is disabled.
    if (!status.desiredRunning) {
        return status.serviceActive
            ? ManagerWatchdogAction::Repair
            : ManagerWatchdogAction::None;
    }

    if (!status.autoRestart) {
        return ManagerWatchdogAction::None;
    }

    return !status.serviceActive ||
            status.state == Domain::ManagerServiceState::Failed
        ? ManagerWatchdogAction::Repair
        : ManagerWatchdogAction::None;
}

std::chrono::seconds ManagerWatchdogPolicy::normalizedInterval(
    const Domain::ManagerControllerSnapshot& snapshot) noexcept
{
    return normalizedInterval(snapshot.status);
}

std::chrono::seconds ManagerWatchdogPolicy::normalizedInterval(
    const Domain::ManagerStatus& status) noexcept
{
    return std::clamp(
        status.watchdogInterval,
        MinimumInterval,
        MaximumInterval);
}

} // namespace ForgeConductor::Hosts::Manager
