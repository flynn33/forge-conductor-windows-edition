#include "Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.h"

#include "Infrastructure/Windows/Detail/CommandLineBuilder.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::wstring_view TaskPathPrefix =
    L"\\ForgeConductor.Manager.v1.";
constexpr std::size_t MaximumNativePathUtf16Units = 32'767U;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] wchar_t asciiUpper(const wchar_t value) noexcept
{
    if (value >= L'a' && value <= L'z') {
        return static_cast<wchar_t>(value - L'a' + L'A');
    }
    return value;
}

[[nodiscard]] bool equalAsciiIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (asciiUpper(left[index]) != asciiUpper(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isReservedDeviceComponent(
    const std::wstring_view component) noexcept
{
    const std::size_t dot = component.find(L'.');
    const std::wstring_view base = component.substr(0U, dot);
    if (equalAsciiIgnoreCase(base, L"CON") ||
        equalAsciiIgnoreCase(base, L"PRN") ||
        equalAsciiIgnoreCase(base, L"AUX") ||
        equalAsciiIgnoreCase(base, L"NUL") ||
        equalAsciiIgnoreCase(base, L"CONIN$") ||
        equalAsciiIgnoreCase(base, L"CONOUT$")) {
        return true;
    }
    return base.size() == 4U && base[3U] >= L'1' && base[3U] <= L'9' &&
        (equalAsciiIgnoreCase(base.substr(0U, 3U), L"COM") ||
         equalAsciiIgnoreCase(base.substr(0U, 3U), L"LPT"));
}

[[nodiscard]] bool isForbiddenPathCharacter(const wchar_t value) noexcept
{
    return value < 0x20 || value == L'<' || value == L'>' ||
        value == L'"' || value == L'|' || value == L'?' ||
        value == L'*' || value == L':';
}

[[nodiscard]] bool isValidPathComponent(
    const std::wstring_view component) noexcept
{
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L' ' || component.back() == L'.' ||
        isReservedDeviceComponent(component)) {
        return false;
    }
    for (const wchar_t character : component) {
        if (isForbiddenPathCharacter(character)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Domain::Result<std::wstring> validateAbsoluteLocalPath(
    const Domain::PathText& path,
    const std::string_view label)
{
    auto converted = strictUtf8ToUtf16(path.value());
    if (!converted) {
        return Domain::Result<std::wstring>::failure(
            std::move(converted).error());
    }

    const std::wstring& value = converted.value();
    if (value.starts_with(L"\\\\") || value.starts_with(L"//") ||
        value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\")) {
        return failure<std::wstring>(
            Domain::ErrorCodes::PathOutsideAuthority,
            std::string{label} +
                " must be a local drive path, not a UNC or device path.");
    }
    const bool hasAsciiDrive = value.size() >= 1U &&
        ((value[0U] >= L'A' && value[0U] <= L'Z') ||
         (value[0U] >= L'a' && value[0U] <= L'z'));
    if (value.size() < 3U || value.size() > MaximumNativePathUtf16Units ||
        !hasAsciiDrive || value[1U] != L':' || value[2U] != L'\\' ||
        value.find(L'/') != std::wstring::npos ||
        value.find(L'\0') != std::wstring::npos ||
        (value.size() > 3U && value.back() == L'\\')) {
        return failure<std::wstring>(
            value.size() > MaximumNativePathUtf16Units
                ? Domain::ErrorCodes::LimitExceeded
                : Domain::ErrorCodes::InvalidRequest,
            std::string{label} +
                " must have a bounded absolute local Windows drive-path form.");
    }

    std::size_t start = 3U;
    while (start < value.size()) {
        const std::size_t separator = value.find(L'\\', start);
        const std::size_t end = separator == std::wstring::npos
            ? value.size()
            : separator;
        if (!isValidPathComponent(value.substr(start, end - start))) {
            return failure<std::wstring>(
                Domain::ErrorCodes::InvalidRequest,
                std::string{label} +
                    " contains an empty, relative, reserved, or forbidden component.");
        }
        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1U;
    }
    return converted;
}

[[nodiscard]] bool isLowerHexStableKey(
    const std::string_view stableKey) noexcept
{
    if (stableKey.size() != WindowsCurrentUserIdentity::StableKeyCharacters) {
        return false;
    }
    for (const char character : stableKey) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Domain::Result<void> validateIdentity(
    const WindowsCurrentUserIdentity& identity)
{
    if (!isLowerHexStableKey(identity.stableKey())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The current-user identity does not contain a canonical stable key."));
    }
    const auto sidBytes = identity.sidBytes();
    if (sidBytes.size() < 8U ||
        sidBytes.size() > WindowsCurrentUserIdentity::MaximumSidBytes ||
        identity.sidText().empty()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The current-user identity does not contain a bounded SID."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::wstring> buildTaskPath(
    const WindowsCurrentUserIdentity& identity,
    const std::string_view purposeSuffix)
{
    if (purposeSuffix.size() >
        ManagerStartupDefinitionBuilder::MaximumPurposeSuffixCharacters) {
        return failure<std::wstring>(
            Domain::ErrorCodes::LimitExceeded,
            "The Manager startup purpose suffix exceeds its character bound.");
    }
    for (const char character : purposeSuffix) {
        const bool safe =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_';
        if (!safe) {
            return failure<std::wstring>(
                Domain::ErrorCodes::InvalidRequest,
                "The Manager startup purpose suffix must contain only safe ASCII characters.");
        }
    }

    std::wstring taskPath{TaskPathPrefix};
    taskPath.reserve(
        taskPath.size() + identity.stableKey().size() +
        (purposeSuffix.empty() ? 0U : purposeSuffix.size() + 1U));
    for (const char character : identity.stableKey()) {
        taskPath.push_back(static_cast<wchar_t>(character));
    }
    if (!purposeSuffix.empty()) {
        taskPath.push_back(L'.');
        for (const char character : purposeSuffix) {
            taskPath.push_back(static_cast<wchar_t>(character));
        }
    }
    if (taskPath.size() >
        Manager::ManagerStartupTaskPolicy::MaximumTaskPathUtf16Units) {
        return failure<std::wstring>(
            Domain::ErrorCodes::LimitExceeded,
            "The resolved Manager startup task path exceeds its UTF-16 bound.");
    }
    return Domain::Result<std::wstring>::success(std::move(taskPath));
}

[[nodiscard]] Domain::Result<Domain::PathText> lexicalParent(
    const std::wstring& executable)
{
    const std::size_t separator = executable.rfind(L'\\');
    if (separator == std::wstring::npos ||
        separator < 2U || separator + 1U >= executable.size()) {
        return failure<Domain::PathText>(
            Domain::ErrorCodes::InvalidRequest,
            "The Manager executable must have a lexical parent and file name.");
    }

    const std::wstring_view parent = separator == 2U
        ? std::wstring_view{executable}.substr(0U, 3U)
        : std::wstring_view{executable}.substr(0U, separator);
    auto converted = strictUtf16ToUtf8(parent);
    if (!converted) {
        return Domain::Result<Domain::PathText>::failure(
            std::move(converted).error());
    }
    return Domain::PathText::create(converted.value());
}

[[nodiscard]] Manager::ManagerStartupTaskDefinition makeDefinition(
    const Domain::ManagerStartupDefinition& startup,
    const WindowsCurrentUserIdentity& identity,
    std::string ownershipUri,
    Domain::PathText workingDirectory,
    std::string arguments)
{
    Manager::ManagerStartupTaskDefinition definition;
    definition.ownership.source =
        Manager::ManagerStartupTaskPolicy::RequiredOwnershipSource;
    definition.ownership.uri = std::move(ownershipUri);

    definition.principal.id =
        Manager::ManagerStartupTaskPolicy::RequiredPrincipalId;
    definition.principal.displayName.clear();
    definition.principal.userIdentity = identity.sidText();
    definition.principal.groupIdentity.clear();
    definition.principal.logonType =
        Manager::ManagerStartupTaskLogonType::CurrentInteractiveUser;
    definition.principal.runLevel =
        Manager::ManagerStartupTaskRunLevel::LeastPrivilege;
    definition.principal.processTokenSidType =
        Manager::ManagerStartupTaskProcessTokenSidType::Default;
    definition.principal.requiredPrivileges.clear();

    Manager::ManagerStartupTaskTrigger trigger;
    trigger.kind = Manager::ManagerStartupTaskTriggerKind::UserLogon;
    trigger.id = Manager::ManagerStartupTaskPolicy::RequiredTriggerId;
    trigger.userIdentity = identity.sidText();
    trigger.enabled = true;
    trigger.startBoundary.reset();
    trigger.endBoundary.reset();
    trigger.executionTimeLimit = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredExecutionTimeLimit};
    trigger.repetition.reset();
    trigger.delay = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredTriggerDelay};
    definition.triggers.push_back(std::move(trigger));

    definition.actionContext = definition.principal.id;
    Manager::ManagerStartupTaskAction action;
    action.kind = Manager::ManagerStartupTaskActionKind::Execute;
    action.id = Manager::ManagerStartupTaskPolicy::RequiredActionId;
    action.executable = startup.managerExecutable;
    action.arguments = std::move(arguments);
    action.workingDirectory = std::move(workingDirectory);
    action.hideAppWindow = true;
    definition.actions.push_back(std::move(action));

    auto& settings = definition.settings;
    settings.allowDemandStart = true;
    settings.multipleInstances =
        Manager::ManagerStartupTaskMultipleInstances::IgnoreNew;
    settings.executionTimeLimit = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredExecutionTimeLimit};
    settings.restartInterval = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredRestartInterval};
    settings.restartCount =
        Manager::ManagerStartupTaskPolicy::RequiredRestartCount;
    settings.runOnlyIfIdle = false;
    settings.idleDuration = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredIdleDuration};
    settings.idleWaitTimeout = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredIdleWaitTimeout};
    settings.stopOnIdleEnd = false;
    settings.restartOnIdle = false;
    settings.runOnlyIfNetworkAvailable = false;
    settings.networkProfileId.clear();
    settings.networkProfileName.clear();
    settings.disallowStartIfOnBatteries = false;
    settings.stopIfGoingOnBatteries = false;
    settings.allowHardTerminate = true;
    settings.wakeToRun = false;
    settings.hidden = false;
    settings.startWhenAvailable = false;
    settings.deleteExpiredTaskAfter = Manager::ManagerStartupTaskDuration{
        Manager::ManagerStartupTaskPolicy::RequiredDeleteExpiredTaskAfter};
    settings.priority = Manager::ManagerStartupTaskPolicy::RequiredPriority;
    settings.compatibility =
        Manager::ManagerStartupTaskCompatibility::Windows10OrLater;
    settings.disallowStartOnRemoteAppSession = false;
    settings.useUnifiedSchedulingEngine = true;
    settings.maintenanceSettingsPresent = false;
    settings.volatileTask = false;
    return definition;
}

} // namespace

Domain::Result<ManagerStartupResolvedRegistration>
ManagerStartupDefinitionBuilder::build(
    const Domain::ManagerStartupDefinition& startup,
    const WindowsCurrentUserIdentity& identity,
    const std::string_view purposeSuffix) noexcept
{
    try {
        auto validStartup = Domain::validateManagerStartupDefinition(startup);
        if (!validStartup) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(validStartup).error());
        }
        auto validIdentity = validateIdentity(identity);
        if (!validIdentity) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(validIdentity).error());
        }
        auto taskPath = buildTaskPath(identity, purposeSuffix);
        if (!taskPath) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(taskPath).error());
        }
        auto ownershipUri = strictUtf16ToUtf8(taskPath.value());
        if (!ownershipUri) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(ownershipUri).error());
        }
        auto executable = validateAbsoluteLocalPath(
            startup.managerExecutable,
            "The Manager executable");
        if (!executable) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(executable).error());
        }
        auto home = validateAbsoluteLocalPath(
            startup.home,
            "The Manager home");
        if (!home) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(home).error());
        }
        auto workingDirectory = lexicalParent(executable.value());
        if (!workingDirectory) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(workingDirectory).error());
        }

        auto argumentString = CommandLineBuilder::buildArgumentString(
            {"--home", startup.home.value()});
        if (!argumentString) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(argumentString).error());
        }
        auto arguments = strictUtf16ToUtf8(argumentString.value());
        if (!arguments) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(arguments).error());
        }

        auto definition = makeDefinition(
            startup,
            identity,
            std::move(ownershipUri).value(),
            std::move(workingDirectory).value(),
            std::move(arguments).value());
        auto validDefinition =
            Manager::ManagerStartupTaskPolicy::validateExpectedDefinition(
                definition);
        if (!validDefinition) {
            return Domain::Result<ManagerStartupResolvedRegistration>::failure(
                std::move(validDefinition).error());
        }

        return Domain::Result<ManagerStartupResolvedRegistration>::success(
            ManagerStartupResolvedRegistration{
                std::move(taskPath).value(),
                std::move(definition)});
    } catch (...) {
        return failure<ManagerStartupResolvedRegistration>(
            Domain::ErrorCodes::InternalFailure,
            "The Manager startup registration definition could not allocate bounded state.");
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
