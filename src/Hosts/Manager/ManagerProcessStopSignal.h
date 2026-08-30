#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace ForgeConductor::Hosts::Manager {

// Shared one-shot process-stop edge. Dashboard post-delivery work requests the
// edge, while the composition root observes it without polling and remains the
// sole owner of ManagerProcessHost shutdown.
class ManagerProcessStopSignal final {
public:
    ManagerProcessStopSignal() noexcept = default;
    ~ManagerProcessStopSignal() noexcept = default;

    ManagerProcessStopSignal(const ManagerProcessStopSignal&) = delete;
    ManagerProcessStopSignal& operator=(const ManagerProcessStopSignal&) = delete;
    ManagerProcessStopSignal(ManagerProcessStopSignal&&) = delete;
    ManagerProcessStopSignal& operator=(ManagerProcessStopSignal&&) = delete;

    // Returns true only for the caller that publishes the process-stop edge.
    [[nodiscard]] bool requestStop() noexcept;

    [[nodiscard]] std::stop_token token() const noexcept;
    [[nodiscard]] bool requested() const noexcept;

    // Event-driven watcher boundary. Returns true for the process-stop edge
    // and false when watcher teardown requests cancellation first.
    [[nodiscard]] bool wait(std::stop_token cancellation) noexcept;

private:
    std::stop_source stopSource_;
    std::mutex waitMutex_;
    std::condition_variable_any waitCondition_;
};

} // namespace ForgeConductor::Hosts::Manager
