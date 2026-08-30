#include "Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.h"
#include "Infrastructure/TestSupport.h"

#include <chrono>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace Detail = Infrastructure::Windows::Detail;

static_assert(std::is_final_v<Detail::ManagerStartupDefinitionBuilder>);
static_assert(std::is_final_v<Detail::ManagerStartupResolvedRegistration>);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::ManagerStartupDefinition startup(
    const std::string_view executable,
    const std::string_view home)
{
    return Domain::ManagerStartupDefinition{path(executable), path(home)};
}

[[nodiscard]] Manager::ManagerStartupTaskDefinition expectedDefinition(
    const Domain::ManagerStartupDefinition& input,
    const Infrastructure::Windows::WindowsCurrentUserIdentity& identity,
    const std::string_view arguments,
    const std::string_view workingDirectory,
    const std::string_view purposeSuffix = {})
{
    Manager::ManagerStartupTaskDefinition expected;
    expected.ownership.source =
        Manager::ManagerStartupTaskPolicy::RequiredOwnershipSource;
    expected.ownership.uri =
        std::string{
            Manager::ManagerStartupTaskPolicy::RequiredOwnershipUriPrefix} +
        identity.stableKey();
    if (!purposeSuffix.empty()) {
        expected.ownership.uri.push_back('.');
        expected.ownership.uri.append(purposeSuffix);
    }
    expected.principal.id =
        Manager::ManagerStartupTaskPolicy::RequiredPrincipalId;
    expected.principal.userIdentity = identity.sidText();
    expected.principal.logonType =
        Manager::ManagerStartupTaskLogonType::CurrentInteractiveUser;
    expected.principal.runLevel =
        Manager::ManagerStartupTaskRunLevel::LeastPrivilege;
    expected.principal.processTokenSidType =
        Manager::ManagerStartupTaskProcessTokenSidType::Default;

    Manager::ManagerStartupTaskTrigger trigger;
    trigger.kind = Manager::ManagerStartupTaskTriggerKind::UserLogon;
    trigger.id = Manager::ManagerStartupTaskPolicy::RequiredTriggerId;
    trigger.userIdentity = identity.sidText();
    trigger.enabled = true;
    trigger.executionTimeLimit = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredExecutionTimeLimit};
    trigger.delay = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredTriggerDelay};
    expected.triggers.push_back(std::move(trigger));

    expected.actionContext = expected.principal.id;
    Manager::ManagerStartupTaskAction action;
    action.kind = Manager::ManagerStartupTaskActionKind::Execute;
    action.id = Manager::ManagerStartupTaskPolicy::RequiredActionId;
    action.executable = input.managerExecutable;
    action.arguments = arguments;
    action.workingDirectory = path(workingDirectory);
    action.hideAppWindow = true;
    expected.actions.push_back(std::move(action));

    expected.settings.allowDemandStart = true;
    expected.settings.multipleInstances =
        Manager::ManagerStartupTaskMultipleInstances::IgnoreNew;
    expected.settings.executionTimeLimit = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredExecutionTimeLimit};
    expected.settings.restartInterval = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredRestartInterval};
    expected.settings.restartCount =
        Manager::ManagerStartupTaskPolicy::RequiredRestartCount;
    expected.settings.idleDuration = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredIdleDuration};
    expected.settings.idleWaitTimeout = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredIdleWaitTimeout};
    expected.settings.allowHardTerminate = true;
    expected.settings.deleteExpiredTaskAfter =
        Manager::ManagerStartupTaskDuration{
            Manager::ManagerStartupTaskPolicy::RequiredDeleteExpiredTaskAfter};
    expected.settings.priority =
        Manager::ManagerStartupTaskPolicy::RequiredPriority;
    expected.settings.compatibility =
        Manager::ManagerStartupTaskCompatibility::Windows10OrLater;
    expected.settings.useUnifiedSchedulingEngine = true;
    return expected;
}

[[nodiscard]] std::wstring expectedTaskPath(
    const Infrastructure::Windows::WindowsCurrentUserIdentity& identity,
    const std::string_view suffix = {})
{
    std::wstring value = L"\\ForgeConductor.Manager.v1.";
    for (const char character : identity.stableKey()) {
        value.push_back(static_cast<wchar_t>(character));
    }
    if (!suffix.empty()) {
        value.push_back(L'.');
        for (const char character : suffix) {
            value.push_back(static_cast<wchar_t>(character));
        }
    }
    return value;
}

void testExactProjection()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    const auto input = startup(
        "C:\\Program Files\\Forge Conductor\\ForgeConductor.Manager.exe",
        "D:\\Forge Home\\Manager State");
    const auto resolved = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            input,
            identity,
            "alpha_1"));

    require(
        resolved.taskPath == expectedTaskPath(identity, "alpha_1"),
        "the resolved task path did not match the canonical per-user name");
    const auto expected = expectedDefinition(
        input,
        identity,
        "--home \"D:\\Forge Home\\Manager State\"",
        "C:\\Program Files\\Forge Conductor",
        "alpha_1");
    require(
        resolved.definition == expected,
        "the resolved task definition did not exactly match the canonical projection");
    require(
        Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
            resolved.definition)
            .hasValue(),
        "the resolved task definition did not pass the final policy gate");
}

void testQuotedHomeArgumentPreservesBackslashes()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    const auto input = startup(
        "C:\\Forge Tools\\Manager\\ForgeConductor.Manager.exe",
        "C:\\Forge Home\\Nested Folder\\State");
    const auto resolved = take(
        Detail::ManagerStartupDefinitionBuilder::build(input, identity));

    require(
        resolved.definition.actions.front().arguments ==
            "--home \"C:\\Forge Home\\Nested Folder\\State\"",
        "the --home argument was not quoted with its Windows backslashes intact");
    require(
        resolved.definition.actions.front().workingDirectory ==
            path("C:\\Forge Tools\\Manager"),
        "the executable parent was not projected lexically");
}

void testDriveRootIsAValidLexicalParent()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    const auto resolved = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            startup("C:\\ForgeConductor.Manager.exe", "D:\\ForgeHome"),
            identity));
    require(
        resolved.definition.actions.front().workingDirectory == path("C:\\"),
        "a drive root was not retained as the executable lexical parent");
}

void testInvalidPurposeSuffixesAndBounds()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    const auto input = startup(
        "C:\\Forge\\ForgeConductor.Manager.exe",
        "C:\\ForgeHome");
    for (const std::string_view invalid : {
             std::string_view{"has space"},
             std::string_view{"dot.suffix"},
             std::string_view{"slash/suffix"},
             std::string_view{"slash\\suffix"},
             std::string_view{"caf\xc3\xa9"}}) {
        requireError(
            Detail::ManagerStartupDefinitionBuilder::build(
                input,
                identity,
                invalid),
            Domain::ErrorCodes::InvalidRequest,
            "an unsafe purpose suffix was accepted");
    }

    const std::string maximum(
        Detail::ManagerStartupDefinitionBuilder::
            MaximumPurposeSuffixCharacters,
        'a');
    const auto bounded = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            input,
            identity,
            maximum));
    require(
        bounded.taskPath == expectedTaskPath(identity, maximum) &&
            bounded.definition.ownership.uri ==
                std::string{
                    Manager::ManagerStartupTaskPolicy::
                        RequiredOwnershipUriPrefix} +
                    identity.stableKey() + "." + maximum &&
            bounded.taskPath.size() <=
                Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units,
        "the maximum safe purpose suffix did not resolve to one bounded native registration identity");

    const std::string overflow(
        Detail::ManagerStartupDefinitionBuilder::
                MaximumPurposeSuffixCharacters +
            1U,
        'a');
    requireError(
        Detail::ManagerStartupDefinitionBuilder::build(
            input,
            identity,
            overflow),
        Domain::ErrorCodes::LimitExceeded,
        "an overlong purpose suffix was accepted");
}

void testInvalidWindowsPathShapes()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    struct PathCase final {
        std::string_view executable;
        std::string_view home;
    };
    const std::vector<PathCase> invalid{
        {"ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"C:Forge\\ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"\\\\server\\share\\ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"\\\\?\\C:\\Forge\\ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"C:/Forge/ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"C:\\Forge\\..\\ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"C:\\Forge\\CON\\ForgeConductor.Manager.exe", "C:\\ForgeHome"},
        {"C:\\Forge\\ForgeConductor.Manager\".exe", "C:\\ForgeHome"},
        {"C:\\Forge\\ForgeConductor.Manager.exe\\", "C:\\ForgeHome"},
        {"C:\\", "D:\\ForgeHome"},
        {"C:\\Forge\\ForgeConductor.Manager.exe", "ForgeHome"},
        {"C:\\Forge\\ForgeConductor.Manager.exe", "\\\\server\\share\\home"},
        {"C:\\Forge\\ForgeConductor.Manager.exe", "C:\\ForgeHome\\"},
        {"C:\\Forge\\ForgeConductor.Manager.exe", "C:\\Forge\\.\\Home"}};

    for (const auto& testCase : invalid) {
        require(
            !Detail::ManagerStartupDefinitionBuilder::build(
                startup(testCase.executable, testCase.home),
                identity),
            "an invalid local Windows path shape was accepted");
    }
}

void testPurposeSuffixesIsolateRegistrations()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    const auto input = startup(
        "C:\\Forge\\ForgeConductor.Manager.exe",
        "C:\\ForgeHome");
    const auto first = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            input,
            identity,
            "first"));
    const auto second = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            input,
            identity,
            "second"));

    require(
        first.taskPath != second.taskPath &&
            first.taskPath == expectedTaskPath(identity, "first") &&
            second.taskPath == expectedTaskPath(identity, "second"),
        "distinct purpose suffixes did not isolate the task paths");
    require(
        first.definition.ownership.uri != second.definition.ownership.uri &&
            first.definition.ownership.uri ==
                std::string{
                    Manager::ManagerStartupTaskPolicy::
                        RequiredOwnershipUriPrefix} +
                    identity.stableKey() + ".first" &&
            second.definition.ownership.uri ==
                std::string{
                    Manager::ManagerStartupTaskPolicy::
                        RequiredOwnershipUriPrefix} +
                    identity.stableKey() + ".second",
        "distinct purpose suffixes did not isolate the native ownership URIs");
    auto firstLaunch = first.definition;
    auto secondLaunch = second.definition;
    firstLaunch.ownership.uri.clear();
    secondLaunch.ownership.uri.clear();
    require(
        firstLaunch == secondLaunch,
        "a purpose suffix changed launch behavior instead of only the registration identity");
}

void testProjectionUsesCurrentIdentity()
{
    const auto identity = take(
        Infrastructure::Windows::WindowsCurrentUserIdentity::load());
    const auto resolved = take(
        Detail::ManagerStartupDefinitionBuilder::build(
            startup(
                "C:\\Forge\\ForgeConductor.Manager.exe",
                "C:\\ForgeHome"),
            identity));

    require(
        resolved.definition.ownership.uri ==
            std::string{
                Manager::ManagerStartupTaskPolicy::
                    RequiredOwnershipUriPrefix} +
                identity.stableKey(),
        "the ownership URI did not use the current identity stable key");
    require(
        resolved.definition.principal.userIdentity == identity.sidText() &&
            resolved.definition.triggers.front().userIdentity ==
                identity.sidText(),
        "the principal and logon trigger did not use the current SID");
    require(
        resolved.taskPath == expectedTaskPath(identity),
        "the default task path did not use the current identity stable key");
}

} // namespace

void registerManagerStartupDefinitionBuilderTests(TestRegistry& tests)
{
    addTest(
        tests,
        "manager_startup_definition_builder_exact_projection",
        testExactProjection);
    addTest(
        tests,
        "manager_startup_definition_builder_quotes_home",
        testQuotedHomeArgumentPreservesBackslashes);
    addTest(
        tests,
        "manager_startup_definition_builder_drive_root_parent",
        testDriveRootIsAValidLexicalParent);
    addTest(
        tests,
        "manager_startup_definition_builder_suffix_validation",
        testInvalidPurposeSuffixesAndBounds);
    addTest(
        tests,
        "manager_startup_definition_builder_path_validation",
        testInvalidWindowsPathShapes);
    addTest(
        tests,
        "manager_startup_definition_builder_suffix_isolation",
        testPurposeSuffixesIsolateRegistrations);
    addTest(
        tests,
        "manager_startup_definition_builder_current_identity",
        testProjectionUsesCurrentIdentity);
}

} // namespace ForgeConductor::Tests
