#include "ForgeConductor/Contracts/IManagerStartupService.h"
#include "ForgeConductor/Manager/ManagerStartupTaskPolicy.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Manager;

using namespace std::chrono_literals;

static_assert(std::is_abstract_v<Contracts::IManagerStartupService>);
static_assert(std::has_virtual_destructor_v<Contracts::IManagerStartupService>);
static_assert(std::is_final_v<Manager::ManagerStartupTaskPolicy>);

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error{message};
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result, const std::string& message)
{
    if (!result) {
        fail(message + ": " + result.error().message);
    }
    return std::move(result).value();
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string& message)
{
    require(!result, message + " unexpectedly succeeded");
    require(result.error().code == code, message + " returned the wrong error");
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value), "create path");
}

[[nodiscard]] Manager::ManagerStartupTaskDefinition expectedDefinition()
{
    Manager::ManagerStartupTaskDefinition definition;
    definition.ownership = {
        std::string{Manager::ManagerStartupTaskPolicy::RequiredOwnershipSource},
        std::string{
            Manager::ManagerStartupTaskPolicy::RequiredOwnershipUriPrefix} +
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    definition.principal.id =
        Manager::ManagerStartupTaskPolicy::RequiredPrincipalId;
    definition.principal.userIdentity = "S-1-5-21-1000-1001-1002-1003";
    definition.principal.logonType =
        Manager::ManagerStartupTaskLogonType::CurrentInteractiveUser;
    definition.principal.runLevel =
        Manager::ManagerStartupTaskRunLevel::LeastPrivilege;
    definition.principal.processTokenSidType =
        Manager::ManagerStartupTaskProcessTokenSidType::Default;

    Manager::ManagerStartupTaskTrigger trigger;
    trigger.kind = Manager::ManagerStartupTaskTriggerKind::UserLogon;
    trigger.id = Manager::ManagerStartupTaskPolicy::RequiredTriggerId;
    trigger.userIdentity = definition.principal.userIdentity;
    trigger.enabled = true;
    trigger.executionTimeLimit =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredExecutionTimeLimit};
    trigger.delay = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredTriggerDelay};
    definition.triggers.push_back(std::move(trigger));

    definition.actionContext = definition.principal.id;
    Manager::ManagerStartupTaskAction action;
    action.kind = Manager::ManagerStartupTaskActionKind::Execute;
    action.id = Manager::ManagerStartupTaskPolicy::RequiredActionId;
    action.executable = path(
        "C:\\Forge Conductor\\ForgeConductor.Manager.exe");
    action.arguments = "--home \"C:\\Forge Home\"";
    action.workingDirectory = path("C:\\Forge Conductor");
    action.hideAppWindow = true;
    definition.actions.push_back(std::move(action));

    auto& settings = definition.settings;
    settings.allowDemandStart = true;
    settings.multipleInstances =
        Manager::ManagerStartupTaskMultipleInstances::IgnoreNew;
    settings.executionTimeLimit =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredExecutionTimeLimit};
    settings.restartInterval =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredRestartInterval};
    settings.restartCount =
        Manager::ManagerStartupTaskPolicy::RequiredRestartCount;
    settings.idleDuration =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredIdleDuration};
    settings.idleWaitTimeout =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredIdleWaitTimeout};
    settings.allowHardTerminate = true;
    settings.deleteExpiredTaskAfter =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredDeleteExpiredTaskAfter};
    settings.priority = Manager::ManagerStartupTaskPolicy::RequiredPriority;
    settings.compatibility =
        Manager::ManagerStartupTaskCompatibility::Windows10OrLater;
    return definition;
}

[[nodiscard]] Manager::ManagerStartupTaskObservation observation(
    Manager::ManagerStartupTaskDefinition definition,
    const bool enabled)
{
    Manager::ManagerStartupTaskObservation observed;
    observed.exists = true;
    observed.launchProjectionComplete = true;
    observed.registrationIdentity =
        "\\\\ForgeConductor.Manager.v1.test-user";
    observed.definition = std::move(definition);
    observed.enabled = enabled;
    observed.lastResult = 0;
    observed.lastRunAt = Domain::UtcTimePoint{10s};
    return observed;
}

void testDomainValidation()
{
    const Domain::ManagerStartupDefinition valid{
        path("C:\\Forge\\ForgeConductor.Manager.exe"),
        path("C:\\ForgeHome")};
    require(
        Domain::validateManagerStartupDefinition(valid).hasValue(),
        "valid startup definition");

    const Domain::ManagerStartupDefinition samePath{
        path("C:\\Forge"), path("C:\\Forge")};
    requireError(
        Domain::validateManagerStartupDefinition(samePath),
        Domain::ErrorCodes::InvalidRequest,
        "same startup paths");

    Domain::ManagerStartupStatus incoherent;
    incoherent.running = true;
    requireError(
        Domain::validateManagerStartupStatus(incoherent),
        Domain::ErrorCodes::InvalidRequest,
        "missing startup status with task state");

    std::string malformedPath{"C:\\Forge\\"};
    malformedPath.push_back(static_cast<char>(0xff));
    const Domain::ManagerStartupDefinition malformed{
        path(malformedPath), path("C:\\ForgeHome")};
    requireError(
        Domain::validateManagerStartupDefinition(malformed),
        Domain::ErrorCodes::InvalidRequest,
        "invalid UTF-8 startup path");

    Domain::ManagerStartupStatus foreign;
    foreign.state = Domain::ManagerStartupState::ForeignConflict;
    foreign.registered = true;
    foreign.enabled = true;
    foreign.registrationIdentity = "foreign";
    require(
        Domain::validateManagerStartupOutcome({foreign, false}).hasValue(),
        "unchanged foreign outcome");
    requireError(
        Domain::validateManagerStartupOutcome({foreign, true}),
        Domain::ErrorCodes::InvalidRequest,
        "mutated foreign outcome");
}

void testMissingClassification()
{
    const auto classified = take(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), {}),
        "classify missing");
    require(
        classified.state == Domain::ManagerStartupState::Missing &&
            !classified.registered && !classified.enabled &&
            !classified.definitionMatches,
        "missing classification");
}

void testReadyClassification()
{
    auto observed = observation(expectedDefinition(), true);
    observed.running = true;
    const auto classified = take(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), observed),
        "classify ready");
    require(
        classified.state == Domain::ManagerStartupState::Ready &&
            classified.registered && classified.enabled &&
            classified.definitionMatches && classified.running,
        "ready classification");
}

void testDisabledClassification()
{
    auto observed = observation(expectedDefinition(), false);
    observed.running = true;
    const auto classified = take(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), observed),
        "classify disabled");
    require(
        classified.state == Domain::ManagerStartupState::Disabled &&
            classified.registered && !classified.enabled &&
            classified.definitionMatches && classified.running,
        "disabled classification preserves a still-running task instance");
}

void testOwnedDriftClassification()
{
    using Definition = Manager::ManagerStartupTaskDefinition;
    struct DriftCase final {
        std::string_view label;
        void (*mutate)(Definition&);
    };

    const std::vector<DriftCase> cases{
        {"principal ID", [](Definition& value) {
             value.principal.id += ".drift";
         }},
        {"principal display name", [](Definition& value) {
             value.principal.displayName = "Forge Conductor Manager";
         }},
        {"principal user identity", [](Definition& value) {
             value.principal.userIdentity = "S-1-5-21-9-8-7-6";
         }},
        {"principal group identity", [](Definition& value) {
             value.principal.groupIdentity = "S-1-5-32-545";
         }},
        {"principal logon type", [](Definition& value) {
             value.principal.logonType =
                 Manager::ManagerStartupTaskLogonType::Other;
         }},
        {"principal run level", [](Definition& value) {
             value.principal.runLevel =
                 Manager::ManagerStartupTaskRunLevel::Other;
         }},
        {"principal process-token SID type", [](Definition& value) {
             value.principal.processTokenSidType =
                 Manager::ManagerStartupTaskProcessTokenSidType::Other;
         }},
        {"principal required privilege", [](Definition& value) {
             value.principal.requiredPrivileges.push_back(
                 "SeChangeNotifyPrivilege");
         }},
        {"trigger kind", [](Definition& value) {
             value.triggers.front().kind =
                 Manager::ManagerStartupTaskTriggerKind::Other;
         }},
        {"trigger ID", [](Definition& value) {
             value.triggers.front().id += ".drift";
         }},
        {"trigger user identity", [](Definition& value) {
             value.triggers.front().userIdentity = "S-1-5-21-9-8-7-6";
         }},
        {"trigger enablement", [](Definition& value) {
             value.triggers.front().enabled = false;
         }},
        {"trigger start boundary", [](Definition& value) {
             value.triggers.front().startBoundary = "2026-08-30T07:00:00";
         }},
        {"trigger end boundary", [](Definition& value) {
             value.triggers.front().endBoundary = "2026-08-31T07:00:00";
         }},
        {"trigger execution limit", [](Definition& value) {
             value.triggers.front().executionTimeLimit =
                 Manager::ManagerStartupTaskDuration{1s};
         }},
        {"trigger repetition", [](Definition& value) {
             value.triggers.front().repetition =
                 Manager::ManagerStartupTaskRepetition{
                     Manager::ManagerStartupTaskDuration{1min},
                     Manager::ManagerStartupTaskDuration{2min},
                     true};
         }},
        {"trigger delay", [](Definition& value) {
             value.triggers.front().delay =
                 Manager::ManagerStartupTaskDuration{1s};
         }},
        {"extra trigger", [](Definition& value) {
             value.triggers.push_back(value.triggers.front());
         }},
        {"action context", [](Definition& value) {
             value.actionContext += ".drift";
         }},
        {"action kind", [](Definition& value) {
             value.actions.front().kind =
                 Manager::ManagerStartupTaskActionKind::Other;
         }},
        {"action ID", [](Definition& value) {
             value.actions.front().id += ".drift";
         }},
        {"action executable", [](Definition& value) {
             value.actions.front().executable = path("C:\\Other\\Manager.exe");
         }},
        {"missing action executable", [](Definition& value) {
             value.actions.front().executable.reset();
         }},
        {"action arguments", [](Definition& value) {
             value.actions.front().arguments += " --unexpected";
         }},
        {"action working directory", [](Definition& value) {
             value.actions.front().workingDirectory = path("C:\\Other");
         }},
        {"missing action working directory", [](Definition& value) {
             value.actions.front().workingDirectory.reset();
         }},
        {"action window visibility", [](Definition& value) {
             value.actions.front().hideAppWindow = false;
         }},
        {"extra action", [](Definition& value) {
             value.actions.push_back(value.actions.front());
         }},
        {"allow demand start", [](Definition& value) {
             value.settings.allowDemandStart = false;
         }},
        {"multiple-instance policy", [](Definition& value) {
             value.settings.multipleInstances =
                 Manager::ManagerStartupTaskMultipleInstances::Other;
         }},
        {"task execution limit", [](Definition& value) {
             value.settings.executionTimeLimit =
                 Manager::ManagerStartupTaskDuration{1s};
         }},
        {"preserved calendar execution limit", [](Definition& value) {
             value.settings.executionTimeLimit =
                 Manager::ManagerStartupTaskDuration::preserved("P1M");
         }},
        {"restart interval", [](Definition& value) {
             value.settings.restartInterval =
                 Manager::ManagerStartupTaskDuration{2min};
         }},
        {"restart count", [](Definition& value) {
             value.settings.restartCount = 2U;
         }},
        {"idle gate", [](Definition& value) {
             value.settings.runOnlyIfIdle = true;
         }},
        {"idle duration", [](Definition& value) {
             value.settings.idleDuration =
                 Manager::ManagerStartupTaskDuration{9min};
         }},
        {"idle wait timeout", [](Definition& value) {
             value.settings.idleWaitTimeout =
                 Manager::ManagerStartupTaskDuration{59min};
         }},
        {"stop on idle end", [](Definition& value) {
             value.settings.stopOnIdleEnd = true;
         }},
        {"restart on idle", [](Definition& value) {
             value.settings.restartOnIdle = true;
         }},
        {"network gate", [](Definition& value) {
             value.settings.runOnlyIfNetworkAvailable = true;
         }},
        {"network profile ID", [](Definition& value) {
             value.settings.networkProfileId = "{network-profile}";
         }},
        {"network profile name", [](Definition& value) {
             value.settings.networkProfileName = "Network";
         }},
        {"start battery gate", [](Definition& value) {
             value.settings.disallowStartIfOnBatteries = true;
         }},
        {"stop battery gate", [](Definition& value) {
             value.settings.stopIfGoingOnBatteries = true;
         }},
        {"hard termination", [](Definition& value) {
             value.settings.allowHardTerminate = false;
         }},
        {"wake to run", [](Definition& value) {
             value.settings.wakeToRun = true;
         }},
        {"hidden task", [](Definition& value) {
             value.settings.hidden = true;
         }},
        {"start when available", [](Definition& value) {
             value.settings.startWhenAvailable = true;
         }},
        {"expiration delay", [](Definition& value) {
             value.settings.deleteExpiredTaskAfter =
                 Manager::ManagerStartupTaskDuration{1s};
         }},
        {"priority", [](Definition& value) {
             value.settings.priority = 6U;
         }},
        {"compatibility", [](Definition& value) {
             value.settings.compatibility =
                 Manager::ManagerStartupTaskCompatibility::Other;
         }},
        {"remote-app-session policy", [](Definition& value) {
             value.settings.disallowStartOnRemoteAppSession = true;
         }},
        {"unified scheduling engine", [](Definition& value) {
             value.settings.useUnifiedSchedulingEngine = true;
         }},
        {"maintenance settings", [](Definition& value) {
             value.settings.maintenanceSettingsPresent = true;
         }},
        {"volatile task", [](Definition& value) {
             value.settings.volatileTask = true;
         }}
    };

    for (const auto& driftCase : cases) {
        auto drifted = expectedDefinition();
        driftCase.mutate(drifted);
        const auto classified = take(
            Manager::ManagerStartupTaskPolicy::classify(
                expectedDefinition(), observation(std::move(drifted), true)),
            "classify owned drift: " + std::string{driftCase.label});
        require(
            classified.state == Domain::ManagerStartupState::Drifted &&
                classified.registered && classified.enabled &&
                !classified.definitionMatches,
            "owned field must classify as drift: " +
                std::string{driftCase.label});
    }
}

void testForeignConflictClassification()
{
    for (const bool changeSource : {true, false}) {
        auto foreign = expectedDefinition();
        if (changeSource) {
            foreign.ownership.source = "Foreign.Product";
        } else {
            foreign.ownership.uri = "urn:foreign:manager";
        }
        const auto classified = take(
            Manager::ManagerStartupTaskPolicy::classify(
                expectedDefinition(), observation(std::move(foreign), true)),
            "classify foreign registration");
        require(
            classified.state == Domain::ManagerStartupState::ForeignConflict &&
                classified.registered && !classified.definitionMatches,
            "each ownership field must classify as foreign");
    }

    auto incompleteForeign = observation(expectedDefinition(), true);
    incompleteForeign.definition->ownership.source = "Foreign.Product";
    incompleteForeign.launchProjectionComplete = false;
    const auto incompleteClassified = take(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), incompleteForeign),
        "classify incomplete foreign registration");
    require(
        incompleteClassified.state ==
            Domain::ManagerStartupState::ForeignConflict,
        "foreign ownership is resolved before full launch projection");
}

void testInvalidAndBoundedInputs()
{
    auto invalidExpected = expectedDefinition();
    invalidExpected.triggers.push_back(invalidExpected.triggers.front());
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            invalidExpected),
        Domain::ErrorCodes::InvalidRequest,
        "multiple expected triggers");

    auto delayedExpected = expectedDefinition();
    delayedExpected.triggers.front().delay =
        Manager::ManagerStartupTaskDuration{1s};
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            delayedExpected),
        Domain::ErrorCodes::InvalidRequest,
        "delayed expected trigger");

    auto finiteExpected = expectedDefinition();
    finiteExpected.settings.executionTimeLimit =
        Manager::ManagerStartupTaskDuration{1min};
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            finiteExpected),
        Domain::ErrorCodes::InvalidRequest,
        "finite expected task execution limit");

    auto shortRestart = expectedDefinition();
    shortRestart.settings.restartInterval =
        Manager::ManagerStartupTaskDuration{30s};
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            shortRestart),
        Domain::ErrorCodes::InvalidRequest,
        "sub-minute expected restart interval");

    auto networkGated = expectedDefinition();
    networkGated.settings.runOnlyIfNetworkAvailable = true;
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            networkGated),
        Domain::ErrorCodes::InvalidRequest,
        "network-gated expected task");

    auto malformedSid = expectedDefinition();
    malformedSid.principal.userIdentity = "S-1-5-021-1000";
    malformedSid.triggers.front().userIdentity =
        malformedSid.principal.userIdentity;
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            malformedSid),
        Domain::ErrorCodes::InvalidRequest,
        "noncanonical expected SID");

    auto overflowingSid = expectedDefinition();
    overflowingSid.principal.userIdentity = "S-1-5-4294967296";
    overflowingSid.triggers.front().userIdentity =
        overflowingSid.principal.userIdentity;
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            overflowingSid),
        Domain::ErrorCodes::InvalidRequest,
        "overflowing expected SID component");

    auto trailingSeparatorSid = expectedDefinition();
    trailingSeparatorSid.principal.userIdentity = "S-1-5-21-";
    trailingSeparatorSid.triggers.front().userIdentity =
        trailingSeparatorSid.principal.userIdentity;
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            trailingSeparatorSid),
        Domain::ErrorCodes::InvalidRequest,
        "trailing-separator expected SID");

    auto azureAdSid = expectedDefinition();
    azureAdSid.principal.userIdentity =
        "S-1-12-1-111111111-222222222-333333333-444444444";
    azureAdSid.triggers.front().userIdentity =
        azureAdSid.principal.userIdentity;
    require(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            azureAdSid).hasValue(),
        "authority-12 expected SID");

    auto highAuthoritySid = expectedDefinition();
    highAuthoritySid.principal.userIdentity = "S-1-0x000100000000-1";
    highAuthoritySid.triggers.front().userIdentity =
        highAuthoritySid.principal.userIdentity;
    require(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            highAuthoritySid).hasValue(),
        "canonical high-authority expected SID");

    auto decimalHighAuthoritySid = expectedDefinition();
    decimalHighAuthoritySid.principal.userIdentity = "S-1-4294967296-1";
    decimalHighAuthoritySid.triggers.front().userIdentity =
        decimalHighAuthoritySid.principal.userIdentity;
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            decimalHighAuthoritySid),
        Domain::ErrorCodes::InvalidRequest,
        "noncanonical decimal high-authority expected SID");

    auto oversizedExecutablePath = expectedDefinition();
    oversizedExecutablePath.actions.front().executable =
        path("C:\\" + std::string(
            Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units - 2U,
            'x'));
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            oversizedExecutablePath),
        Domain::ErrorCodes::LimitExceeded,
        "oversized Task Scheduler executable path");

    auto oversizedWorkingDirectory = expectedDefinition();
    oversizedWorkingDirectory.actions.front().workingDirectory =
        path("C:\\" + std::string(
            Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units - 2U,
            'x'));
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            oversizedWorkingDirectory),
        Domain::ErrorCodes::LimitExceeded,
        "oversized Task Scheduler working directory");

    auto maximumTaskPaths = expectedDefinition();
    const auto maximumTaskPath = path("C:\\" + std::string(
        Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units - 3U,
        'x'));
    maximumTaskPaths.actions.front().executable = maximumTaskPath;
    maximumTaskPaths.actions.front().workingDirectory = maximumTaskPath;
    require(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            maximumTaskPaths).hasValue(),
        "maximum Task Scheduler path length");

    auto oversizedCommandLine = expectedDefinition();
    oversizedCommandLine.actions.front().arguments.assign(
        Manager::ManagerStartupTaskPolicy::MaximumTextBytes - 1U,
        'x');
    requireError(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            oversizedCommandLine),
        Domain::ErrorCodes::LimitExceeded,
        "combined Windows command line limit");

    Manager::ManagerStartupTaskObservation incoherent;
    incoherent.enabled = true;
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), incoherent),
        Domain::ErrorCodes::InvalidRequest,
        "incoherent missing observation");

    auto incomplete = observation(expectedDefinition(), true);
    incomplete.launchProjectionComplete = false;
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), incomplete),
        Domain::ErrorCodes::IntegrityFailure,
        "incomplete native launch projection");

    auto malformed = expectedDefinition();
    malformed.ownership.source.push_back(static_cast<char>(0xff));
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), observation(std::move(malformed), true)),
        Domain::ErrorCodes::InvalidRequest,
        "invalid UTF-8 observed task text");

    auto embeddedNull = expectedDefinition();
    embeddedNull.actions.front().arguments.push_back('\0');
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), observation(std::move(embeddedNull), true)),
        Domain::ErrorCodes::InvalidRequest,
        "embedded NUL observed task text");

    std::string malformedActionPath{"C:\\Forge Conductor\\"};
    malformedActionPath.push_back(static_cast<char>(0xff));
    auto malformedPathObservation = expectedDefinition();
    malformedPathObservation.actions.front().executable =
        path(malformedActionPath);
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(),
            observation(std::move(malformedPathObservation), true)),
        Domain::ErrorCodes::InvalidRequest,
        "invalid UTF-8 observed action path");

    auto maximumCollections = expectedDefinition();
    maximumCollections.triggers.resize(
        Manager::ManagerStartupTaskPolicy::MaximumObservedTriggerCount,
        maximumCollections.triggers.front());
    maximumCollections.actions.resize(
        Manager::ManagerStartupTaskPolicy::MaximumObservedActionCount,
        maximumCollections.actions.front());
    maximumCollections.principal.requiredPrivileges.resize(
        Manager::ManagerStartupTaskPolicy::MaximumObservedPrivilegeCount,
        "SeChangeNotifyPrivilege");
    const auto maximumCollectionsClassified = take(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(),
            observation(std::move(maximumCollections), true)),
        "classify native-maximum observed collections");
    require(
        maximumCollectionsClassified.state ==
            Domain::ManagerStartupState::Drifted,
        "native-maximum observed collections remain repairable drift");

    auto oversizedTriggers = expectedDefinition();
    oversizedTriggers.triggers.resize(
        Manager::ManagerStartupTaskPolicy::MaximumObservedTriggerCount + 1U,
        oversizedTriggers.triggers.front());
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(),
            observation(std::move(oversizedTriggers), true)),
        Domain::ErrorCodes::LimitExceeded,
        "oversized observed trigger collection");

    auto oversizedActions = expectedDefinition();
    oversizedActions.actions.resize(
        Manager::ManagerStartupTaskPolicy::MaximumObservedActionCount + 1U,
        oversizedActions.actions.front());
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(),
            observation(std::move(oversizedActions), true)),
        Domain::ErrorCodes::LimitExceeded,
        "oversized observed action collection");

    auto oversizedPrivileges = expectedDefinition();
    oversizedPrivileges.principal.requiredPrivileges.resize(
        Manager::ManagerStartupTaskPolicy::MaximumObservedPrivilegeCount + 1U,
        "SeChangeNotifyPrivilege");
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(),
            observation(std::move(oversizedPrivileges), true)),
        Domain::ErrorCodes::LimitExceeded,
        "oversized observed privilege collection");

    auto oversizedIdentity = observation(expectedDefinition(), true);
    oversizedIdentity.registrationIdentity = std::string(
        Domain::MaximumManagerStartupRegistrationIdentityBytes + 1U,
        'x');
    requireError(
        Manager::ManagerStartupTaskPolicy::classify(
            expectedDefinition(), oversizedIdentity),
        Domain::ErrorCodes::LimitExceeded,
        "oversized registration identity");
}

} // namespace

int main()
{
    try {
        testDomainValidation();
        testMissingClassification();
        testReadyClassification();
        testDisabledClassification();
        testOwnedDriftClassification();
        testForeignConflictClassification();
        testInvalidAndBoundedInputs();
        std::cout << "Manager startup task policy tests passed: 7 groups\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Manager startup task policy tests failed: "
                  << failure.what() << '\n';
        return 1;
    }
}
