#include "ForgeConductor/Application/ProjectSetupCoordinator.h"

#include <iostream>
#include <stdexcept>

using namespace ForgeConductor::Application;

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error{message};
}

class Backend final : public IProjectSetupOperations {
public:
    int calls{};
    int failAt{-1};
    bool omitIdentity{};
    bool throwFailure{};
    SetupOperationResult next()
    {
        ++calls;
        if (throwFailure) throw std::runtime_error{"offline"};
        return {calls != failAt, calls == failAt ? "Needs repair" : "Verified"};
    }
    SetupOperationResult ensureManager(std::stop_token) override { return next(); }
    SetupOperationResult ensurePlugins(std::stop_token) override { return next(); }
    SetupOperationResult ensureProject(ProjectSetupSnapshot& result, std::stop_token) override
    {
        if (!omitIdentity) result.projectId = "existing-project";
        return next();
    }
    SetupOperationResult ensureProvider(ProjectSetupSnapshot& result, std::stop_token) override
    {
        result.model = "local-model";
        return next();
    }
    SetupOperationResult verifyProvider(ProjectSetupSnapshot&, std::stop_token) override
    { return next(); }
};
}

int main()
{
    try {
        for (int failedStage = 1; failedStage <= 5; ++failedStage) {
            Backend backend;
            backend.failAt = failedStage;
            ProjectSetupCoordinator coordinator{backend};
            auto result = coordinator.prepare("C:/project", {});
            require(!result.ready, "A failed stage must not advertise readiness");
            require(backend.calls == failedStage, "No dependent step may run after failure");
            require(result.checks[static_cast<std::size_t>(failedStage - 1)].state ==
                SetupState::NeedsAction, "Failure must provide an actionable state");
            backend.calls = 0;
            backend.failAt = -1;
            auto retry = coordinator.prepare("C:/project", {});
            require(retry.checks.size() == 5 && retry.checks[2].stage == SetupStage::Plugins,
                "Default setup must install plugins before preparing the model");
            require(retry.ready && retry.projectId == "existing-project",
                "Retry must revalidate all stages and retain authoritative project identity");
        }
        Backend backend;
        ProjectSetupCoordinator coordinator{backend};
        std::stop_source stop;
        auto cancelled = coordinator.prepare("C:/project", stop.get_token(),
            [&stop](const ProjectSetupSnapshot& snapshot) {
                if (snapshot.checks[0].state == SetupState::Ready) stop.request_stop();
            });
        require(!cancelled.ready && backend.calls == 1,
            "Cancellation must prevent further setup mutations");
        backend.calls = 0;
        backend.omitIdentity = true;
        auto missing = coordinator.prepare("C:/project", {});
        require(!missing.ready && backend.calls == 2,
            "A success flag without project readback must fail closed");
        backend.throwFailure = true;
        auto exception = coordinator.prepare("C:/project", {});
        require(!exception.ready && exception.checks[0].detail == "offline",
            "Backend exceptions must become visible recovery states");
        std::cout << "Project setup failure, cancellation, identity and retry checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
