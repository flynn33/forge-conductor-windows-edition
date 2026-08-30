#include "Infrastructure/Windows/Detail/IWindowsTaskSchedulerStartupPlatform.h"
#include "Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.h"
#include "Infrastructure/Windows/Detail/WindowsManagerStartupComHandler.h"
#include "Infrastructure/TestSupport.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ForgeConductor::Tests {
namespace {

namespace Detail = Infrastructure::Windows::Detail;

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::ManagerStartupDefinition startupDefinition()
{
    return Domain::ManagerStartupDefinition{
        path("C:\\Forge Conductor\\ForgeConductor.Manager.exe"),
        path("C:\\Forge Conductor\\Home")};
}

enum class FakePlatformFailurePoint {
    None,
    Resolve,
    InitialInspect,
    RegisterCanonical,
    SetEnabled,
    StartNow,
    Remove,
    PostInspect
};

class FakeStartupPlatform final
    : public Detail::IWindowsTaskSchedulerStartupPlatform {
public:
    FakeStartupPlatform()
    {
        const auto identity = take(
            Infrastructure::Windows::WindowsCurrentUserIdentity::load());
        canonical_ = take(Detail::ManagerStartupDefinitionBuilder::build(
            startupDefinition(), identity, "handler-test"));
    }

    [[nodiscard]] Domain::Result<Detail::ManagerStartupResolvedRegistration>
    resolve(
        const Domain::ManagerStartupDefinition& expected,
        const std::string_view purposeSuffix,
        const Domain::OperationContext&) noexcept override
    {
        try {
            ++resolveCalls;
            if (failurePoint == FakePlatformFailurePoint::Resolve) {
                return Domain::Result<
                    Detail::ManagerStartupResolvedRegistration>::failure(
                        injectedFailure());
            }
            lastPurposeSuffix = std::string{purposeSuffix};
            const auto identity = take(
                Infrastructure::Windows::WindowsCurrentUserIdentity::load());
            return Detail::ManagerStartupDefinitionBuilder::build(
                expected, identity, purposeSuffix);
        } catch (const std::exception& exception) {
            return Domain::Result<Detail::ManagerStartupResolvedRegistration>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        }
    }

    [[nodiscard]] Domain::Result<Manager::ManagerStartupTaskObservation>
    inspect(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        ++inspectCalls;
        if ((failurePoint == FakePlatformFailurePoint::InitialInspect &&
             inspectCalls == 1U) ||
            (failurePoint == FakePlatformFailurePoint::PostInspect &&
             inspectCalls == 2U)) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                    injectedFailure());
        }
        return Domain::Result<Manager::ManagerStartupTaskObservation>::success(
            observation);
    }

    [[nodiscard]] Domain::Result<void> registerCanonical(
        const Detail::ManagerStartupResolvedRegistration& registration,
        const Detail::ManagerStartupRegistrationMutation mutation,
        const bool enabled,
        const Domain::OperationContext&) noexcept override
    {
        ++registerCalls;
        lastRegistrationMutation = mutation;
        if (failurePoint == FakePlatformFailurePoint::RegisterCanonical) {
            return Domain::Result<void>::failure(injectedFailure());
        }
        if (raceOnNextMutation) {
            raceOnNextMutation = false;
            return ownershipRace();
        }
        if (postMutationObservation.has_value()) {
            observation = *postMutationObservation;
        } else if (!ignoreMutations) {
            observation = exactObservation(registration, enabled);
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> setEnabled(
        const Detail::ManagerStartupResolvedRegistration&,
        const bool enabled,
        const Domain::OperationContext&) noexcept override
    {
        ++setEnabledCalls;
        if (failurePoint == FakePlatformFailurePoint::SetEnabled) {
            return Domain::Result<void>::failure(injectedFailure());
        }
        if (raceOnNextMutation) {
            raceOnNextMutation = false;
            return ownershipRace();
        }
        if (postMutationObservation.has_value()) {
            observation = *postMutationObservation;
        } else if (!ignoreMutations) {
            observation.enabled = enabled;
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> startNow(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        ++startCalls;
        if (failurePoint == FakePlatformFailurePoint::StartNow) {
            return Domain::Result<void>::failure(injectedFailure());
        }
        if (raceOnNextMutation) {
            raceOnNextMutation = false;
            return ownershipRace();
        }
        if (postMutationObservation.has_value()) {
            observation = *postMutationObservation;
        } else if (!ignoreMutations) {
            observation.running = true;
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        ++removeCalls;
        if (failurePoint == FakePlatformFailurePoint::Remove) {
            return Domain::Result<void>::failure(injectedFailure());
        }
        if (raceOnNextMutation) {
            raceOnNextMutation = false;
            return ownershipRace();
        }
        if (postMutationObservation.has_value()) {
            observation = *postMutationObservation;
        } else if (!ignoreMutations) {
            observation = {};
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Manager::ManagerStartupTaskObservation exact(
        const bool enabled) const
    {
        return exactObservation(canonical_, enabled);
    }

    [[nodiscard]] Manager::ManagerStartupTaskObservation drifted(
        const bool enabled) const
    {
        auto result = exact(enabled);
        result.definition->actions.front().arguments += " --drift";
        return result;
    }

    [[nodiscard]] Manager::ManagerStartupTaskObservation foreign(
        const bool enabled) const
    {
        auto result = exact(enabled);
        result.definition->ownership.source = "Foreign.Product";
        return result;
    }

    Manager::ManagerStartupTaskObservation observation;
    std::optional<Manager::ManagerStartupTaskObservation>
        postMutationObservation;
    FakePlatformFailurePoint failurePoint{FakePlatformFailurePoint::None};
    bool ignoreMutations{};
    bool raceOnNextMutation{};
    std::size_t resolveCalls{};
    std::size_t inspectCalls{};
    std::size_t registerCalls{};
    std::size_t setEnabledCalls{};
    std::size_t startCalls{};
    std::size_t removeCalls{};
    std::string lastPurposeSuffix;
    Detail::ManagerStartupRegistrationMutation lastRegistrationMutation{
        Detail::ManagerStartupRegistrationMutation::CreateMissing};

private:
    [[nodiscard]] static Manager::ManagerStartupTaskObservation exactObservation(
        const Detail::ManagerStartupResolvedRegistration& registration,
        const bool enabled)
    {
        Manager::ManagerStartupTaskObservation result;
        result.exists = true;
        result.launchProjectionComplete = true;
        result.registrationIdentity = "ForgeConductor.Manager.Startup.Test";
        result.definition = registration.definition;
        result.enabled = enabled;
        return result;
    }

    [[nodiscard]] static Domain::Result<void> ownershipRace()
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::OwnershipConflict,
            "The task changed ownership immediately before mutation."));
    }

    [[nodiscard]] static Domain::Error injectedFailure()
    {
        return Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "The scripted Task Scheduler platform operation failed.");
    }

    Detail::ManagerStartupResolvedRegistration canonical_;
};

class HandlerFixture final {
public:
    HandlerFixture()
        : platform{std::make_shared<FakeStartupPlatform>()},
          handler{platform}
    {
    }

    [[nodiscard]] Detail::ManagerStartupComRequest request(
        const Detail::ManagerStartupComOperationKind kind,
        const bool enabled = false,
        std::string suffix = "handler-test") const
    {
        return Detail::ManagerStartupComRequest{
            kind,
            startupDefinition(),
            std::move(suffix),
            enabled,
            context.active()};
    }

    [[nodiscard]] Domain::ManagerStartupStatus status(
        const Detail::ManagerStartupComOperationKind kind =
            Detail::ManagerStartupComOperationKind::Inspect)
    {
        auto response = take(handler.handle(request(kind)));
        require(
            std::holds_alternative<Domain::ManagerStartupStatus>(response),
            "the handler did not return a status response");
        return std::get<Domain::ManagerStartupStatus>(std::move(response));
    }

    [[nodiscard]] Domain::ManagerStartupOutcome outcome(
        const Detail::ManagerStartupComOperationKind kind,
        const bool enabled = false)
    {
        auto response = take(handler.handle(request(kind, enabled)));
        require(
            std::holds_alternative<Domain::ManagerStartupOutcome>(response),
            "the handler did not return an outcome response");
        return std::get<Domain::ManagerStartupOutcome>(std::move(response));
    }

    TestContext context;
    std::shared_ptr<FakeStartupPlatform> platform;
    Detail::WindowsManagerStartupComHandler handler;
};

void inspectClassifiesAndResolvesEveryOperation()
{
    HandlerFixture fixture;
    auto missing = fixture.status();
    require(
        missing.state == Domain::ManagerStartupState::Missing,
        "a missing task did not classify as missing");

    fixture.platform->observation = fixture.platform->exact(true);
    auto ready = fixture.status();
    require(
        ready.state == Domain::ManagerStartupState::Ready &&
            ready.definitionMatches,
        "an exact enabled task did not classify as ready");
    require(
        fixture.platform->resolveCalls == 2U &&
            fixture.platform->lastPurposeSuffix == "handler-test",
        "the handler did not resolve identity and purpose on every operation");
}

void registerUsesCreateEnableAndExplicitRepairBoundary()
{
    HandlerFixture fixture;
    auto created = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Register);
    require(
        created.changed &&
            created.status.state == Domain::ManagerStartupState::Ready &&
            fixture.platform->registerCalls == 1U &&
            fixture.platform->lastRegistrationMutation ==
                Detail::ManagerStartupRegistrationMutation::CreateMissing,
        "register did not create the exact enabled task");

    auto unchanged = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Register);
    require(
        !unchanged.changed && fixture.platform->registerCalls == 1U,
        "register rewrote an already-ready task");

    fixture.platform->observation = fixture.platform->exact(false);
    auto enabled = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Register);
    require(
        enabled.changed &&
            enabled.status.state == Domain::ManagerStartupState::Ready &&
            fixture.platform->setEnabledCalls == 1U,
        "register did not enable an exact disabled task");

    fixture.platform->observation = fixture.platform->drifted(true);
    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::Register)),
        Domain::ErrorCodes::Conflict,
        "register silently repaired owned drift");

    fixture.platform->observation = fixture.platform->foreign(true);
    auto foreign = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Register);
    require(
        !foreign.changed &&
            foreign.status.state ==
                Domain::ManagerStartupState::ForeignConflict,
        "register mutated or hid a foreign task");
}

void repairPreservesOwnedEnablementAndNeverTouchesForeignTasks()
{
    HandlerFixture fixture;
    fixture.platform->observation = fixture.platform->drifted(false);
    auto repairedDisabled = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Repair);
    require(
        repairedDisabled.changed &&
            repairedDisabled.status.state ==
                Domain::ManagerStartupState::Disabled &&
            fixture.platform->lastRegistrationMutation ==
                Detail::ManagerStartupRegistrationMutation::ReplaceOwned,
        "repair did not preserve the disabled state of owned drift");

    fixture.platform->observation = fixture.platform->drifted(true);
    auto repairedEnabled = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Repair);
    require(
        repairedEnabled.changed &&
            repairedEnabled.status.state == Domain::ManagerStartupState::Ready,
        "repair did not preserve the enabled state of owned drift");

    fixture.platform->observation = {};
    auto created = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Repair);
    require(
        created.changed &&
            created.status.state == Domain::ManagerStartupState::Ready &&
            fixture.platform->lastRegistrationMutation ==
                Detail::ManagerStartupRegistrationMutation::CreateMissing,
        "repair did not create a missing registration safely");

    fixture.platform->observation = fixture.platform->foreign(false);
    auto foreign = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Repair);
    require(
        !foreign.changed &&
            foreign.status.state ==
                Domain::ManagerStartupState::ForeignConflict,
        "repair mutated a foreign task");
}

void enablementRejectsUnsafeStatesButCanDisableOwnedDrift()
{
    HandlerFixture fixture;
    fixture.platform->observation = fixture.platform->exact(true);
    auto disabled = fixture.outcome(
        Detail::ManagerStartupComOperationKind::SetEnabled, false);
    require(
        disabled.changed &&
            disabled.status.state == Domain::ManagerStartupState::Disabled,
        "setEnabled did not disable an exact task");

    fixture.platform->observation = fixture.platform->drifted(true);
    auto driftDisabled = fixture.outcome(
        Detail::ManagerStartupComOperationKind::SetEnabled, false);
    require(
        driftDisabled.changed &&
            driftDisabled.status.state ==
                Domain::ManagerStartupState::Drifted &&
            !driftDisabled.status.enabled,
        "setEnabled could not safely disable owned drift");

    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::SetEnabled, true)),
        Domain::ErrorCodes::Conflict,
        "setEnabled enabled owned drift before repair");

    fixture.platform->observation = {};
    auto missing = fixture.outcome(
        Detail::ManagerStartupComOperationKind::SetEnabled, true);
    require(
        !missing.changed &&
            missing.status.state == Domain::ManagerStartupState::Missing,
        "setEnabled created a missing registration implicitly");
}

void startRequiresAnExactEnabledTask()
{
    HandlerFixture fixture;
    fixture.platform->observation = fixture.platform->exact(true);
    auto started = fixture.outcome(
        Detail::ManagerStartupComOperationKind::StartNow);
    require(
        started.changed &&
            started.status.state == Domain::ManagerStartupState::Ready &&
            started.status.running && fixture.platform->startCalls == 1U,
        "startNow did not run an exact enabled task");

    fixture.platform->observation = fixture.platform->exact(false);
    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::StartNow)),
        Domain::ErrorCodes::Conflict,
        "startNow ran a disabled task");

    fixture.platform->observation = fixture.platform->drifted(true);
    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::StartNow)),
        Domain::ErrorCodes::Conflict,
        "startNow ran a drifted task");

    fixture.platform->observation = fixture.platform->foreign(true);
    auto foreign = fixture.outcome(
        Detail::ManagerStartupComOperationKind::StartNow);
    require(
        !foreign.changed && fixture.platform->startCalls == 1U,
        "startNow ran a foreign task");
}

void removeDeletesOnlyOwnedRegistrations()
{
    HandlerFixture fixture;
    fixture.platform->observation = fixture.platform->drifted(true);
    auto removed = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Remove);
    require(
        removed.changed &&
            removed.status.state == Domain::ManagerStartupState::Missing &&
            fixture.platform->removeCalls == 1U,
        "remove did not delete owned drift");

    auto missing = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Remove);
    require(
        !missing.changed && fixture.platform->removeCalls == 1U,
        "remove attempted to delete a missing task");

    fixture.platform->observation = fixture.platform->foreign(true);
    auto foreign = fixture.outcome(
        Detail::ManagerStartupComOperationKind::Remove);
    require(
        !foreign.changed && fixture.platform->removeCalls == 1U,
        "remove deleted a foreign task");
}

void postconditionsAndOwnershipRacesFailClosed()
{
    HandlerFixture fixture;
    fixture.platform->ignoreMutations = true;
    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::Register)),
        Domain::ErrorCodes::IntegrityFailure,
        "a successful native call without its postcondition was accepted");

    fixture.platform->ignoreMutations = false;
    fixture.platform->raceOnNextMutation = true;
    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::Register)),
        Domain::ErrorCodes::OwnershipConflict,
        "an ownership replacement race was not surfaced");
    require(
        fixture.platform->observation.exists == false,
        "an ownership replacement race mutated the scripted task");
}

void platformFailuresPropagateWithoutAdditionalNativeCalls()
{
    {
        HandlerFixture fixture;
        fixture.platform->failurePoint = FakePlatformFailurePoint::Resolve;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Inspect)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "a resolve failure was not propagated");
        require(
            fixture.platform->resolveCalls == 1U &&
                fixture.platform->inspectCalls == 0U,
            "the handler continued after a resolve failure");
    }

    {
        HandlerFixture fixture;
        fixture.platform->failurePoint =
            FakePlatformFailurePoint::InitialInspect;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Register)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "an initial inspect failure was not propagated");
        require(
            fixture.platform->inspectCalls == 1U &&
                fixture.platform->registerCalls == 0U,
            "the handler mutated after an initial inspect failure");
    }

    {
        HandlerFixture fixture;
        fixture.platform->failurePoint =
            FakePlatformFailurePoint::RegisterCanonical;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Register)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "a registration mutation failure was not propagated");
        require(
            fixture.platform->registerCalls == 1U &&
                fixture.platform->inspectCalls == 1U &&
                !fixture.platform->observation.exists,
            "the handler inspected or changed state after registration failed");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->failurePoint =
            FakePlatformFailurePoint::SetEnabled;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::SetEnabled,
                false)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "an enablement mutation failure was not propagated");
        require(
            fixture.platform->setEnabledCalls == 1U &&
                fixture.platform->inspectCalls == 1U &&
                fixture.platform->observation.enabled,
            "the handler inspected or changed state after enablement failed");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->failurePoint = FakePlatformFailurePoint::StartNow;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::StartNow)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "a start mutation failure was not propagated");
        require(
            fixture.platform->startCalls == 1U &&
                fixture.platform->inspectCalls == 1U &&
                !fixture.platform->observation.running,
            "the handler inspected or changed state after start failed");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->failurePoint = FakePlatformFailurePoint::Remove;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Remove)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "a remove mutation failure was not propagated");
        require(
            fixture.platform->removeCalls == 1U &&
                fixture.platform->inspectCalls == 1U &&
                fixture.platform->observation.exists,
            "the handler inspected or changed state after remove failed");
    }

    {
        HandlerFixture fixture;
        fixture.platform->failurePoint =
            FakePlatformFailurePoint::PostInspect;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Register)),
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "a post-mutation inspect failure was not propagated");
        require(
            fixture.platform->registerCalls == 1U &&
                fixture.platform->inspectCalls == 2U &&
                fixture.platform->observation.exists,
            "the post-mutation inspect failure did not follow one mutation");
    }
}

void enableStartAndRemovePostconditionsFailClosed()
{
    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->postMutationObservation =
            fixture.platform->exact(true);
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::SetEnabled,
                false)),
            Domain::ErrorCodes::IntegrityFailure,
            "an unmet enablement postcondition was accepted");
        require(
            fixture.platform->setEnabledCalls == 1U &&
                fixture.platform->observation.enabled,
            "the enablement postcondition test did not preserve the wrong state");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->postMutationObservation =
            fixture.platform->exact(false);
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::StartNow)),
            Domain::ErrorCodes::IntegrityFailure,
            "an unmet start postcondition was accepted");
        require(
            fixture.platform->startCalls == 1U &&
                !fixture.platform->observation.enabled,
            "the start postcondition test did not expose the wrong state");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->postMutationObservation =
            fixture.platform->exact(true);
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Remove)),
            Domain::ErrorCodes::IntegrityFailure,
            "an unmet remove postcondition was accepted");
        require(
            fixture.platform->removeCalls == 1U &&
                fixture.platform->observation.exists,
            "the remove postcondition test did not preserve the task");
    }
}

void everyOwnedMutationOwnershipRaceFailsClosed()
{
    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->drifted(true);
        fixture.platform->raceOnNextMutation = true;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Repair)),
            Domain::ErrorCodes::OwnershipConflict,
            "a ReplaceOwned repair race was not surfaced");
        require(
            fixture.platform->registerCalls == 1U &&
                fixture.platform->lastRegistrationMutation ==
                    Detail::ManagerStartupRegistrationMutation::ReplaceOwned &&
                fixture.platform->observation.definition.has_value() &&
                fixture.platform->observation.definition->actions.front()
                        .arguments.find("--drift") != std::string::npos,
            "a ReplaceOwned repair race changed the drifted task");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->raceOnNextMutation = true;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::SetEnabled,
                false)),
            Domain::ErrorCodes::OwnershipConflict,
            "an enablement ownership race was not surfaced");
        require(
            fixture.platform->setEnabledCalls == 1U &&
                fixture.platform->observation.enabled,
            "an enablement ownership race changed the task");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->raceOnNextMutation = true;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::StartNow)),
            Domain::ErrorCodes::OwnershipConflict,
            "a start ownership race was not surfaced");
        require(
            fixture.platform->startCalls == 1U &&
                !fixture.platform->observation.running,
            "a start ownership race ran the task");
    }

    {
        HandlerFixture fixture;
        fixture.platform->observation = fixture.platform->exact(true);
        fixture.platform->raceOnNextMutation = true;
        requireError(
            fixture.handler.handle(fixture.request(
                Detail::ManagerStartupComOperationKind::Remove)),
            Domain::ErrorCodes::OwnershipConflict,
            "a remove ownership race was not surfaced");
        require(
            fixture.platform->removeCalls == 1U &&
                fixture.platform->observation.exists,
            "a remove ownership race deleted the task");
    }
}

void unknownOperationIsRejectedWithoutMutation()
{
    HandlerFixture fixture;
    constexpr auto unknownKind =
        static_cast<Detail::ManagerStartupComOperationKind>(999);
    requireError(
        fixture.handler.handle(fixture.request(unknownKind)),
        Domain::ErrorCodes::InvalidRequest,
        "an unknown Manager startup operation was accepted");
    require(
        fixture.platform->resolveCalls == 1U &&
            fixture.platform->inspectCalls == 1U &&
            fixture.platform->registerCalls == 0U &&
            fixture.platform->setEnabledCalls == 0U &&
            fixture.platform->startCalls == 0U &&
            fixture.platform->removeCalls == 0U,
        "an unknown Manager startup operation mutated Task Scheduler");
}

void invalidPurposeAndMissingPlatformAreRejected()
{
    HandlerFixture fixture;
    requireError(
        fixture.handler.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::Inspect,
            false,
            "unsafe.suffix")),
        Domain::ErrorCodes::InvalidRequest,
        "an unsafe purpose suffix reached Task Scheduler");

    Detail::WindowsManagerStartupComHandler missingPlatform{nullptr};
    requireError(
        missingPlatform.handle(fixture.request(
            Detail::ManagerStartupComOperationKind::Inspect)),
        Domain::ErrorCodes::IntegrityFailure,
        "a handler without its platform was accepted");
}

} // namespace

void registerWindowsManagerStartupComHandlerTests(TestRegistry& tests)
{
    addTest(
        tests,
        "manager_startup.handler.inspect",
        inspectClassifiesAndResolvesEveryOperation);
    addTest(
        tests,
        "manager_startup.handler.register",
        registerUsesCreateEnableAndExplicitRepairBoundary);
    addTest(
        tests,
        "manager_startup.handler.repair",
        repairPreservesOwnedEnablementAndNeverTouchesForeignTasks);
    addTest(
        tests,
        "manager_startup.handler.enablement",
        enablementRejectsUnsafeStatesButCanDisableOwnedDrift);
    addTest(
        tests,
        "manager_startup.handler.start",
        startRequiresAnExactEnabledTask);
    addTest(
        tests,
        "manager_startup.handler.remove",
        removeDeletesOnlyOwnedRegistrations);
    addTest(
        tests,
        "manager_startup.handler.postconditions",
        postconditionsAndOwnershipRacesFailClosed);
    addTest(
        tests,
        "manager_startup.handler.platform-failures",
        platformFailuresPropagateWithoutAdditionalNativeCalls);
    addTest(
        tests,
        "manager_startup.handler.mutation-postconditions",
        enableStartAndRemovePostconditionsFailClosed);
    addTest(
        tests,
        "manager_startup.handler.ownership-races",
        everyOwnedMutationOwnershipRaceFailsClosed);
    addTest(
        tests,
        "manager_startup.handler.unknown-operation",
        unknownOperationIsRejectedWithoutMutation);
    addTest(
        tests,
        "manager_startup.handler.invalid-inputs",
        invalidPurposeAndMissingPlatformAreRejected);
}

} // namespace ForgeConductor::Tests
