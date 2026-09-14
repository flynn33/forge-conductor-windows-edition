#include "AppActionScheduler.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using ForgeConductor::Hosts::App::AppActionAdmission;
using ForgeConductor::Hosts::App::AppActionLane;
using ForgeConductor::Hosts::App::AppActionScheduler;

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error{message};
}

void testRefreshBarrierDoesNotDropLifecycleCommand()
{
    AppActionScheduler scheduler;
    require(scheduler.admit(AppActionLane::Observation, 1U) ==
                AppActionAdmission::Started,
            "refresh starts behind the injected barrier");
    require(scheduler.admit(AppActionLane::Command, 2U) ==
                AppActionAdmission::Started,
            "lifecycle command starts independently of refresh");
    require(scheduler.admit(AppActionLane::Observation, 1U) ==
                AppActionAdmission::Coalesced,
            "timer refresh is retained as latest-value observation");

    const auto command = scheduler.complete(AppActionLane::Command);
    require(!command, "exactly one lifecycle command was accepted");
    const auto refresh = scheduler.complete(AppActionLane::Observation);
    require(refresh == 1U, "sampling resumes after the held refresh");
}

void testNavigationAndCommandsAreFifoAndVisible()
{
    AppActionScheduler scheduler;
    require(scheduler.admit(AppActionLane::Command, 10U) ==
                AppActionAdmission::Started,
            "settings load starts");
    require(scheduler.admit(AppActionLane::Command, 11U) ==
                AppActionAdmission::Queued,
            "provider navigation is queued");
    require(scheduler.admit(AppActionLane::Command, 12U) ==
                AppActionAdmission::Queued,
            "lifecycle action is queued");
    require(scheduler.queuedCommands() == 2U, "queued disposition is observable");
    require(scheduler.complete(AppActionLane::Command) == 11U,
            "first queued command is next");
    require(scheduler.admit(AppActionLane::Command, 11U) ==
                AppActionAdmission::Started,
            "returned command is admitted after lane release");
    require(scheduler.complete(AppActionLane::Command) == 12U,
            "second queued command retains order");
}

void testWindowCloseCancelsPendingReplies()
{
    AppActionScheduler scheduler;
    require(scheduler.admit(AppActionLane::Observation, 1U) ==
                AppActionAdmission::Started,
            "refresh starts");
    require(scheduler.admit(AppActionLane::Command, 2U) ==
                AppActionAdmission::Started,
            "command starts");
    require(scheduler.admit(AppActionLane::Command, 3U) ==
                AppActionAdmission::Queued,
            "follow-up queues");
    scheduler.cancel();
    require(!scheduler.complete(AppActionLane::Observation),
            "closed observer cannot replay a refresh");
    require(!scheduler.complete(AppActionLane::Command),
            "closed window cannot replay a command");
    require(scheduler.admit(AppActionLane::Command, 4U) ==
                AppActionAdmission::Cancelled,
            "closed window rejects new work explicitly");
}

} // namespace

int main()
{
    try {
        testRefreshBarrierDoesNotDropLifecycleCommand();
        testNavigationAndCommandsAreFifoAndVisible();
        testWindowCloseCancelsPendingReplies();
        std::cout << "App action scheduler tests passed: 3 groups\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "App action scheduler tests failed: " << error.what()
                  << '\n';
        return 1;
    }
}
