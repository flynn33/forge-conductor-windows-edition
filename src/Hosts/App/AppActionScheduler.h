#pragma once

#include <cstddef>
#include <deque>
#include <optional>

namespace ForgeConductor::Hosts::App {

enum class AppActionLane { Observation, Command };

enum class AppActionAdmission {
    Started,
    Queued,
    Coalesced,
    Rejected,
    Cancelled,
};

// UI-thread-owned admission state. Telemetry uses a latest-value lane while
// user commands retain FIFO disposition independently of observation traffic.
class AppActionScheduler final {
public:
    static constexpr std::size_t MaximumQueuedCommands = 16U;

    [[nodiscard]] AppActionAdmission admit(
        const AppActionLane lane,
        const std::size_t action) noexcept
    {
        if (cancelled_) return AppActionAdmission::Cancelled;
        if (lane == AppActionLane::Observation) {
            if (!observationInFlight_) {
                observationInFlight_ = true;
                return AppActionAdmission::Started;
            }
            pendingObservation_ = action;
            return AppActionAdmission::Coalesced;
        }
        if (!commandInFlight_) {
            commandInFlight_ = true;
            return AppActionAdmission::Started;
        }
        if (commands_.size() >= MaximumQueuedCommands) {
            return AppActionAdmission::Rejected;
        }
        commands_.push_back(action);
        return AppActionAdmission::Queued;
    }

    [[nodiscard]] std::optional<std::size_t> complete(
        const AppActionLane lane) noexcept
    {
        if (lane == AppActionLane::Observation) {
            observationInFlight_ = false;
            auto next = pendingObservation_;
            pendingObservation_.reset();
            return cancelled_ ? std::nullopt : next;
        }
        commandInFlight_ = false;
        if (cancelled_ || commands_.empty()) return std::nullopt;
        const auto next = commands_.front();
        commands_.pop_front();
        return next;
    }

    void cancel() noexcept
    {
        cancelled_ = true;
        pendingObservation_.reset();
        commands_.clear();
    }

    [[nodiscard]] bool observationInFlight() const noexcept
    {
        return observationInFlight_;
    }
    [[nodiscard]] bool commandInFlight() const noexcept
    {
        return commandInFlight_;
    }
    [[nodiscard]] std::size_t queuedCommands() const noexcept
    {
        return commands_.size();
    }

private:
    bool observationInFlight_{};
    bool commandInFlight_{};
    bool cancelled_{};
    std::optional<std::size_t> pendingObservation_;
    std::deque<std::size_t> commands_;
};

} // namespace ForgeConductor::Hosts::App
