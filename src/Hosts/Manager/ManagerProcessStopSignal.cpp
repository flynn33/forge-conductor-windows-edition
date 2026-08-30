#include "ManagerProcessStopSignal.h"

namespace ForgeConductor::Hosts::Manager {

bool ManagerProcessStopSignal::requestStop() noexcept
{
    const bool published = stopSource_.request_stop();
    if (!published) {
        return false;
    }

    try {
        const std::lock_guard lock{waitMutex_};
        waitCondition_.notify_all();
    } catch (...) {
        waitCondition_.notify_all();
    }
    return true;
}

std::stop_token ManagerProcessStopSignal::token() const noexcept
{
    return stopSource_.get_token();
}

bool ManagerProcessStopSignal::requested() const noexcept
{
    return stopSource_.stop_requested();
}

bool ManagerProcessStopSignal::wait(const std::stop_token cancellation) noexcept
{
    try {
        std::unique_lock lock{waitMutex_};
        return waitCondition_.wait(
            lock,
            cancellation,
            [this]() noexcept { return stopSource_.stop_requested(); });
    } catch (...) {
        return requested();
    }
}

} // namespace ForgeConductor::Hosts::Manager
