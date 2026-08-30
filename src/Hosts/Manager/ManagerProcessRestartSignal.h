#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace ForgeConductor::Hosts::Manager {

enum class ManagerProcessRestartRequestResult : std::uint8_t {
    Published,
    Coalesced,
    Closed,
};

enum class ManagerProcessRestartWaitResult : std::uint8_t {
    RestartRequested,
    Closed,
    Cancelled,
};

// Process-owned, reusable restart edge. Request producers can publish at most
// one pending or in-flight restart, while the composition-root worker remains
// the sole owner of performing and completing the restart operation.
class ManagerProcessRestartSignal final {
public:
    ManagerProcessRestartSignal() noexcept = default;
    ~ManagerProcessRestartSignal() noexcept = default;

    ManagerProcessRestartSignal(const ManagerProcessRestartSignal&) = delete;
    ManagerProcessRestartSignal& operator=(
        const ManagerProcessRestartSignal&) = delete;
    ManagerProcessRestartSignal(ManagerProcessRestartSignal&&) = delete;
    ManagerProcessRestartSignal& operator=(
        ManagerProcessRestartSignal&&) = delete;

    [[nodiscard]] ManagerProcessRestartRequestResult requestRestart() noexcept;

    // Waits without polling and atomically claims a pending request for the
    // caller. There must be only one composition-root worker calling this API.
    [[nodiscard]] ManagerProcessRestartWaitResult waitAndBegin(
        std::stop_token cancellation) noexcept;

    // Returns true only when the caller completes the active restart.
    [[nodiscard]] bool completeRestart() noexcept;

    // Permanently rejects new work and wakes a waiting worker. If a worker is
    // active, inFlight remains true until completeRestart acknowledges it.
    void close() noexcept;

    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool inFlight() const noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    enum class State : std::uint8_t {
        Idle,
        Pending,
        InFlight,
        ClosingInFlight,
        Closed,
    };

    std::atomic<State> state_{State::Idle};
    mutable std::mutex waitMutex_;
    std::condition_variable_any waitCondition_;
};

} // namespace ForgeConductor::Hosts::Manager
