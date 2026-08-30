#include "ManagerProcessRestartSignal.h"

namespace ForgeConductor::Hosts::Manager {

ManagerProcessRestartRequestResult
ManagerProcessRestartSignal::requestRestart() noexcept
{
    State state = state_.load(std::memory_order_acquire);
    for (;;) {
        switch (state) {
        case State::Idle:
            if (state_.compare_exchange_weak(
                    state,
                    State::Pending,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                waitCondition_.notify_one();
                return ManagerProcessRestartRequestResult::Published;
            }
            break;
        case State::Pending:
        case State::InFlight:
            return ManagerProcessRestartRequestResult::Coalesced;
        case State::ClosingInFlight:
        case State::Closed:
            return ManagerProcessRestartRequestResult::Closed;
        }
    }
}

ManagerProcessRestartWaitResult ManagerProcessRestartSignal::waitAndBegin(
    const std::stop_token cancellation) noexcept
{
    try {
        std::unique_lock lock{waitMutex_};
        for (;;) {
            const bool ready = waitCondition_.wait(
                lock,
                cancellation,
                [this]() noexcept {
                    const State state = state_.load(std::memory_order_acquire);
                    return state == State::Pending ||
                        state == State::ClosingInFlight ||
                        state == State::Closed;
                });
            if (!ready) {
                return ManagerProcessRestartWaitResult::Cancelled;
            }

            const State state = state_.load(std::memory_order_acquire);
            if (state == State::ClosingInFlight || state == State::Closed) {
                return ManagerProcessRestartWaitResult::Closed;
            }

            State expected = State::Pending;
            if (state_.compare_exchange_strong(
                    expected,
                    State::InFlight,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return ManagerProcessRestartWaitResult::RestartRequested;
            }
        }
    } catch (...) {
        const State state = state_.load(std::memory_order_acquire);
        if (state == State::ClosingInFlight || state == State::Closed) {
            return ManagerProcessRestartWaitResult::Closed;
        }

        State expected = State::Pending;
        if (state_.compare_exchange_strong(
                expected,
                State::InFlight,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return ManagerProcessRestartWaitResult::RestartRequested;
        }
        return expected == State::ClosingInFlight || expected == State::Closed
            ? ManagerProcessRestartWaitResult::Closed
            : ManagerProcessRestartWaitResult::Cancelled;
    }
}

ManagerProcessRestartWaitResult
ManagerProcessRestartSignal::waitAndBeginUntil(
    const std::stop_token cancellation,
    const std::chrono::steady_clock::time_point watchdogDeadline) noexcept
{
    try {
        std::unique_lock lock{waitMutex_};
        for (;;) {
            const bool ready = waitCondition_.wait_until(
                lock,
                cancellation,
                watchdogDeadline,
                [this]() noexcept {
                    const State state = state_.load(std::memory_order_acquire);
                    return state == State::Pending ||
                        state == State::ClosingInFlight ||
                        state == State::Closed;
                });
            if (!ready) {
                return cancellation.stop_requested()
                    ? ManagerProcessRestartWaitResult::Cancelled
                    : ManagerProcessRestartWaitResult::WatchdogDue;
            }

            const State state = state_.load(std::memory_order_acquire);
            if (state == State::ClosingInFlight || state == State::Closed) {
                return ManagerProcessRestartWaitResult::Closed;
            }

            State expected = State::Pending;
            if (state_.compare_exchange_strong(
                    expected,
                    State::InFlight,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return ManagerProcessRestartWaitResult::RestartRequested;
            }
        }
    } catch (...) {
        const State state = state_.load(std::memory_order_acquire);
        if (state == State::ClosingInFlight || state == State::Closed) {
            return ManagerProcessRestartWaitResult::Closed;
        }

        State expected = State::Pending;
        if (state_.compare_exchange_strong(
                expected,
                State::InFlight,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return ManagerProcessRestartWaitResult::RestartRequested;
        }
        return cancellation.stop_requested()
            ? ManagerProcessRestartWaitResult::Cancelled
            : ManagerProcessRestartWaitResult::WatchdogDue;
    }
}

bool ManagerProcessRestartSignal::completeRestart() noexcept
{
    State state = state_.load(std::memory_order_acquire);
    for (;;) {
        State completed{};
        switch (state) {
        case State::InFlight:
            completed = State::Idle;
            break;
        case State::ClosingInFlight:
            completed = State::Closed;
            break;
        case State::Idle:
        case State::Pending:
        case State::Closed:
            return false;
        }
        if (state_.compare_exchange_weak(
                state,
                completed,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            waitCondition_.notify_all();
            return true;
        }
    }
}

void ManagerProcessRestartSignal::close() noexcept
{
    State state = state_.load(std::memory_order_acquire);
    for (;;) {
        const State closedState =
            state == State::InFlight || state == State::ClosingInFlight
            ? State::ClosingInFlight
            : State::Closed;
        if (state == closedState || state_.compare_exchange_weak(
                state,
                closedState,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
    waitCondition_.notify_all();
}

bool ManagerProcessRestartSignal::pending() const noexcept
{
    return state_.load(std::memory_order_acquire) == State::Pending;
}

bool ManagerProcessRestartSignal::inFlight() const noexcept
{
    const State state = state_.load(std::memory_order_acquire);
    return state == State::InFlight || state == State::ClosingInFlight;
}

bool ManagerProcessRestartSignal::closed() const noexcept
{
    const State state = state_.load(std::memory_order_acquire);
    return state == State::ClosingInFlight || state == State::Closed;
}

} // namespace ForgeConductor::Hosts::Manager
