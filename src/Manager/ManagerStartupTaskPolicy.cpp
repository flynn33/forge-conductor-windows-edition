#include "ForgeConductor/Manager/ManagerStartupTaskPolicy.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <limits>
#include <string_view>
#include <utility>

namespace ForgeConductor::Manager {
namespace {

[[nodiscard]] Domain::Result<void> policyError(
    const std::string_view code,
    std::string message)
{
    return Domain::Result<void>::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] Domain::Result<void> validateText(
    const std::string_view value,
    const bool required,
    const std::size_t maximumBytes,
    const std::string_view label)
{
    if (value.size() > maximumBytes) {
        return policyError(
            Domain::ErrorCodes::LimitExceeded,
            std::string{label} + " exceeds its byte limit.");
    }
    if ((required && value.empty()) ||
        value.find('\0') != std::string_view::npos ||
        !Domain::isValidUtf8(value)) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            std::string{label} + " is not valid strict UTF-8 text.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateOptionalText(
    const std::optional<std::string>& value,
    const std::string_view label)
{
    if (!value.has_value()) {
        return Domain::Result<void>::success();
    }
    return validateText(
        *value,
        true,
        ManagerStartupTaskPolicy::MaximumTextBytes,
        label);
}

[[nodiscard]] Domain::Result<void> validateDuration(
    const ManagerStartupTaskDuration& duration,
    const std::string_view label)
{
    if (const auto* fixed = duration.fixedSeconds(); fixed != nullptr) {
        if (*fixed < std::chrono::seconds::zero()) {
            return policyError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{label} + " cannot be negative.");
        }
        return Domain::Result<void>::success();
    }
    const auto* preserved = duration.preservedText();
    return validateText(
        *preserved,
        true,
        ManagerStartupTaskPolicy::MaximumTextBytes,
        label);
}

[[nodiscard]] bool isFixedDuration(
    const ManagerStartupTaskDuration& duration,
    const std::chrono::seconds expected) noexcept
{
    const auto* fixed = duration.fixedSeconds();
    return fixed != nullptr && *fixed == expected;
}

[[nodiscard]] bool isLowerHex(const char value) noexcept
{
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool hasCanonicalOwnershipUri(
    const std::string_view value) noexcept
{
    const auto prefix = ManagerStartupTaskPolicy::RequiredOwnershipUriPrefix;
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U) {
        return false;
    }
    for (const char digit : value.substr(prefix.size())) {
        if (!isLowerHex(digit)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isCanonicalDecimal(
    const std::string_view value,
    const std::uint64_t maximum) noexcept
{
    if (value.empty() ||
        (value.size() > 1U && value.front() == '0')) {
        return false;
    }
    std::uint64_t parsed = 0U;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (maximum - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    return true;
}

[[nodiscard]] bool isCanonicalIdentifierAuthority(
    const std::string_view value) noexcept
{
    if (!value.starts_with("0x")) {
        return isCanonicalDecimal(
            value,
            std::numeric_limits<std::uint32_t>::max());
    }
    if (value.size() != 14U) {
        return false;
    }

    std::uint64_t parsed = 0U;
    for (const char character : value.substr(2U)) {
        std::uint8_t digit = 0U;
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint8_t>(character - '0');
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<std::uint8_t>(character - 'A' + 10);
        } else {
            return false;
        }
        parsed = (parsed << 4U) | digit;
    }
    return parsed > std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool hasCanonicalSidText(const std::string_view value) noexcept
{
    constexpr std::string_view prefix{"S-1-"};
    constexpr std::size_t maximumSubAuthorities = 15U;
    if (!value.starts_with(prefix) || value.size() <= prefix.size() ||
        value.size() > 256U || value.back() == '-') {
        return false;
    }

    const auto authorityEnd = value.find('-', prefix.size());
    if (authorityEnd == std::string_view::npos ||
        !isCanonicalIdentifierAuthority(
            value.substr(prefix.size(), authorityEnd - prefix.size()))) {
        return false;
    }

    std::size_t componentStart = authorityEnd + 1U;
    std::size_t componentCount = 0U;
    while (componentStart < value.size()) {
        const auto separator = value.find('-', componentStart);
        const auto componentEnd = separator == std::string_view::npos
            ? value.size()
            : separator;
        if (!isCanonicalDecimal(
                value.substr(componentStart, componentEnd - componentStart),
                std::numeric_limits<std::uint32_t>::max()) ||
            ++componentCount > maximumSubAuthorities) {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        componentStart = separator + 1U;
    }
    return componentCount > 0U;
}

[[nodiscard]] std::size_t utf16CodeUnits(
    const std::string_view strictUtf8) noexcept
{
    std::size_t units = 0U;
    for (std::size_t index = 0U; index < strictUtf8.size();) {
        const auto lead = static_cast<unsigned char>(strictUtf8[index]);
        if (lead < 0x80U) {
            ++index;
            ++units;
        } else if (lead < 0xe0U) {
            index += 2U;
            ++units;
        } else if (lead < 0xf0U) {
            index += 3U;
            ++units;
        } else {
            index += 4U;
            units += 2U;
        }
    }
    return units;
}

[[nodiscard]] Domain::Result<void> validateObservedShape(
    const ManagerStartupTaskObservation& observed,
    const bool requireCompleteProjection)
{
    if (!observed.exists) {
        if (observed.launchProjectionComplete ||
            observed.registrationIdentity.has_value() ||
            observed.definition.has_value() || observed.enabled ||
            observed.running || observed.lastResult.has_value() ||
            observed.lastRunAt.has_value()) {
            return policyError(
                Domain::ErrorCodes::InvalidRequest,
                "A missing startup task observation cannot expose registration state or history.");
        }
        return Domain::Result<void>::success();
    }

    if (!observed.registrationIdentity.has_value() ||
        !observed.definition.has_value()) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "An existing startup task observation requires an identity and definition.");
    }
    auto validText = validateText(
        *observed.registrationIdentity,
        true,
        Domain::MaximumManagerStartupRegistrationIdentityBytes,
        "The observed registration identity");
    if (!validText) {
        return validText;
    }

    const auto& definition = *observed.definition;
    for (const auto& field : {
             std::pair{std::string_view{definition.ownership.source},
                       std::string_view{"The observed ownership source"}},
             std::pair{std::string_view{definition.ownership.uri},
                       std::string_view{"The observed ownership URI"}}}) {
        validText = validateText(
            field.first,
            false,
            ManagerStartupTaskPolicy::MaximumTextBytes,
            field.second);
        if (!validText) {
            return validText;
        }
    }
    if (!requireCompleteProjection) {
        return Domain::Result<void>::success();
    }
    if (!observed.launchProjectionComplete) {
        return policyError(
            Domain::ErrorCodes::IntegrityFailure,
            "The native startup task observation did not inspect every launch-relevant field.");
    }
    if (definition.triggers.size() >
            ManagerStartupTaskPolicy::MaximumObservedTriggerCount ||
        definition.actions.size() >
            ManagerStartupTaskPolicy::MaximumObservedActionCount ||
        definition.principal.requiredPrivileges.size() >
            ManagerStartupTaskPolicy::MaximumObservedPrivilegeCount) {
        return policyError(
            Domain::ErrorCodes::LimitExceeded,
            "The observed startup task exceeds a bounded collection count.");
    }

    const auto validateObservedText = [](
        const std::string_view value,
        const std::string_view label) {
        return validateText(
            value,
            false,
            ManagerStartupTaskPolicy::MaximumTextBytes,
            label);
    };
    for (const auto& field : {
             std::pair{std::string_view{definition.ownership.source},
                       std::string_view{"The observed ownership source"}},
             std::pair{std::string_view{definition.ownership.uri},
                       std::string_view{"The observed ownership URI"}},
             std::pair{std::string_view{definition.principal.id},
                       std::string_view{"The observed principal ID"}},
             std::pair{std::string_view{definition.principal.displayName},
                       std::string_view{"The observed principal display name"}},
             std::pair{std::string_view{definition.principal.userIdentity},
                       std::string_view{"The observed principal user identity"}},
             std::pair{std::string_view{definition.principal.groupIdentity},
                       std::string_view{"The observed principal group identity"}},
             std::pair{std::string_view{definition.actionContext},
                       std::string_view{"The observed action context"}},
             std::pair{std::string_view{definition.settings.networkProfileId},
                       std::string_view{"The observed network profile ID"}},
             std::pair{std::string_view{definition.settings.networkProfileName},
                       std::string_view{"The observed network profile name"}}}) {
        validText = validateObservedText(field.first, field.second);
        if (!validText) {
            return validText;
        }
    }
    for (const auto& privilege : definition.principal.requiredPrivileges) {
        validText = validateObservedText(
            privilege,
            "An observed principal privilege");
        if (!validText) {
            return validText;
        }
    }
    for (const auto& trigger : definition.triggers) {
        validText = validateDuration(
            trigger.executionTimeLimit,
            "An observed trigger execution limit");
        if (!validText) {
            return validText;
        }
        validText = validateDuration(
            trigger.delay,
            "An observed trigger delay");
        if (!validText) {
            return validText;
        }
        if (trigger.repetition.has_value()) {
            validText = validateDuration(
                trigger.repetition->interval,
                "An observed repetition interval");
            if (!validText) {
                return validText;
            }
            validText = validateDuration(
                trigger.repetition->duration,
                "An observed repetition duration");
            if (!validText) {
                return validText;
            }
        }
        validText = validateObservedText(
            trigger.id,
            "An observed trigger ID");
        if (!validText) {
            return validText;
        }
        validText = validateObservedText(
            trigger.userIdentity,
            "An observed trigger user identity");
        if (!validText) {
            return validText;
        }
        validText = validateOptionalText(
            trigger.startBoundary,
            "An observed trigger start boundary");
        if (!validText) {
            return validText;
        }
        validText = validateOptionalText(
            trigger.endBoundary,
            "An observed trigger end boundary");
        if (!validText) {
            return validText;
        }
    }
    for (const auto& action : definition.actions) {
        validText = validateObservedText(
            action.id,
            "An observed action ID");
        if (!validText) {
            return validText;
        }
        validText = validateObservedText(
            action.arguments,
            "Observed action arguments");
        if (!validText) {
            return validText;
        }
        if ((action.executable.has_value() &&
             !Domain::isValidUtf8(action.executable->value())) ||
            (action.workingDirectory.has_value() &&
             !Domain::isValidUtf8(action.workingDirectory->value()))) {
            return policyError(
                Domain::ErrorCodes::InvalidRequest,
                "Observed executable action paths must be strict UTF-8 text.");
        }
    }
    for (const auto& duration : {
             std::pair{&definition.settings.executionTimeLimit,
                       std::string_view{"The observed task execution limit"}},
             std::pair{&definition.settings.restartInterval,
                       std::string_view{"The observed restart interval"}},
             std::pair{&definition.settings.idleDuration,
                       std::string_view{"The observed idle duration"}},
             std::pair{&definition.settings.idleWaitTimeout,
                       std::string_view{"The observed idle wait timeout"}},
             std::pair{&definition.settings.deleteExpiredTaskAfter,
                       std::string_view{"The observed expiration delay"}}}) {
        validText = validateDuration(*duration.first, duration.second);
        if (!validText) {
            return validText;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<Domain::ManagerStartupStatus> validatedStatus(
    Domain::ManagerStartupStatus status)
{
    auto valid = Domain::validateManagerStartupStatus(status);
    if (!valid) {
        return Domain::Result<Domain::ManagerStartupStatus>::failure(
            std::move(valid).error());
    }
    return Domain::Result<Domain::ManagerStartupStatus>::success(
        std::move(status));
}

} // namespace

Domain::Result<void> ManagerStartupTaskPolicy::validateExpectedDefinition(
    const ManagerStartupTaskDefinition& expected)
{
    for (const auto& field : {
             std::pair{std::string_view{expected.ownership.source},
                       std::string_view{"The expected ownership source"}},
             std::pair{std::string_view{expected.ownership.uri},
                       std::string_view{"The expected ownership URI"}},
             std::pair{std::string_view{expected.principal.id},
                       std::string_view{"The expected principal ID"}},
             std::pair{std::string_view{expected.principal.userIdentity},
                       std::string_view{"The expected principal user identity"}},
             std::pair{std::string_view{expected.actionContext},
                       std::string_view{"The expected action context"}}}) {
        auto validText = validateText(
            field.first,
            true,
            MaximumTextBytes,
            field.second);
        if (!validText) {
            return validText;
        }
    }
    for (const auto& field : {
             std::pair{std::string_view{expected.principal.displayName},
                       std::string_view{"The expected principal display name"}},
             std::pair{std::string_view{expected.principal.groupIdentity},
                       std::string_view{"The expected principal group identity"}},
             std::pair{std::string_view{expected.settings.networkProfileId},
                       std::string_view{"The expected network profile ID"}},
             std::pair{std::string_view{expected.settings.networkProfileName},
                       std::string_view{"The expected network profile name"}}}) {
        auto validText = validateText(
            field.first,
            false,
            MaximumTextBytes,
            field.second);
        if (!validText) {
            return validText;
        }
    }
    if (expected.ownership.source != RequiredOwnershipSource ||
        !hasCanonicalOwnershipUri(expected.ownership.uri) ||
        expected.principal.id != RequiredPrincipalId ||
        !hasCanonicalSidText(expected.principal.userIdentity) ||
        !expected.principal.displayName.empty() ||
        !expected.principal.groupIdentity.empty() ||
        expected.actionContext != expected.principal.id) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected startup task does not use the canonical ownership and principal identities.");
    }
    if (expected.principal.logonType !=
            ManagerStartupTaskLogonType::CurrentInteractiveUser ||
        expected.principal.runLevel !=
            ManagerStartupTaskRunLevel::LeastPrivilege ||
        expected.principal.processTokenSidType !=
            ManagerStartupTaskProcessTokenSidType::Default ||
        !expected.principal.requiredPrivileges.empty()) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected startup task must use the unprivileged current interactive user token.");
    }
    if (expected.triggers.size() != 1U || expected.actions.size() != 1U) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected startup task requires exactly one trigger and one action.");
    }

    const auto& trigger = expected.triggers.front();
    if (trigger.kind != ManagerStartupTaskTriggerKind::UserLogon ||
        trigger.id != RequiredTriggerId ||
        trigger.userIdentity != expected.principal.userIdentity ||
        !trigger.enabled || trigger.startBoundary.has_value() ||
        trigger.endBoundary.has_value() ||
        !isFixedDuration(
            trigger.executionTimeLimit,
            RequiredExecutionTimeLimit) ||
        trigger.repetition.has_value() ||
        !isFixedDuration(trigger.delay, RequiredTriggerDelay)) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected startup task requires the canonical immediate user-logon trigger without boundaries, repetition, or a time limit.");
    }

    const auto& action = expected.actions.front();
    if (action.kind != ManagerStartupTaskActionKind::Execute ||
        action.id != RequiredActionId || !action.executable.has_value() ||
        !action.workingDirectory.has_value() || !action.hideAppWindow) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected startup task requires one complete executable action.");
    }
    auto validActionArguments = validateText(
        action.arguments,
        true,
        MaximumTextBytes,
        "The expected action arguments");
    if (!validActionArguments) {
        return validActionArguments;
    }
    if (!Domain::isValidUtf8(action.executable->value()) ||
        !Domain::isValidUtf8(action.workingDirectory->value())) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected executable action paths must be strict UTF-8 text.");
    }
    if (utf16CodeUnits(action.executable->value()) > MaximumTaskPathUtf16Units ||
        utf16CodeUnits(action.workingDirectory->value()) >
            MaximumTaskPathUtf16Units) {
        return policyError(
            Domain::ErrorCodes::LimitExceeded,
            "The expected executable action paths exceed the Task Scheduler path limit.");
    }
    constexpr std::size_t maximumWindowsCommandLineUtf16Units = 32'767U;
    constexpr std::size_t quotedExecutableSpaceAndNullUnits = 4U;
    const auto executableUnits = utf16CodeUnits(action.executable->value());
    const auto argumentUnits = utf16CodeUnits(action.arguments);
    if (executableUnits > maximumWindowsCommandLineUtf16Units -
            quotedExecutableSpaceAndNullUnits ||
        argumentUnits > maximumWindowsCommandLineUtf16Units -
            quotedExecutableSpaceAndNullUnits - executableUnits) {
        return policyError(
            Domain::ErrorCodes::LimitExceeded,
            "The expected startup action exceeds the Windows command-line limit.");
    }

    const auto& settings = expected.settings;
    for (const auto& duration : {
             std::pair{&settings.executionTimeLimit,
                       std::string_view{"The expected task execution limit"}},
             std::pair{&settings.restartInterval,
                       std::string_view{"The expected restart interval"}},
             std::pair{&settings.idleDuration,
                       std::string_view{"The expected idle duration"}},
             std::pair{&settings.idleWaitTimeout,
                       std::string_view{"The expected idle wait timeout"}},
             std::pair{&settings.deleteExpiredTaskAfter,
                       std::string_view{"The expected expiration delay"}}}) {
        auto validDuration = validateDuration(*duration.first, duration.second);
        if (!validDuration) {
            return validDuration;
        }
    }
    if (!settings.allowDemandStart ||
        settings.multipleInstances !=
            ManagerStartupTaskMultipleInstances::IgnoreNew ||
        !isFixedDuration(
            settings.executionTimeLimit,
            RequiredExecutionTimeLimit) ||
        !isFixedDuration(settings.restartInterval, RequiredRestartInterval) ||
        settings.restartCount != RequiredRestartCount ||
        settings.runOnlyIfIdle ||
        !isFixedDuration(settings.idleDuration, RequiredIdleDuration) ||
        !isFixedDuration(
            settings.idleWaitTimeout,
            RequiredIdleWaitTimeout) ||
        settings.stopOnIdleEnd || settings.restartOnIdle ||
        settings.runOnlyIfNetworkAvailable ||
        !settings.networkProfileId.empty() ||
        !settings.networkProfileName.empty() ||
        settings.disallowStartIfOnBatteries ||
        settings.stopIfGoingOnBatteries ||
        !settings.allowHardTerminate || settings.wakeToRun ||
        settings.hidden || settings.startWhenAvailable ||
        !isFixedDuration(
            settings.deleteExpiredTaskAfter,
            RequiredDeleteExpiredTaskAfter) ||
        settings.priority != RequiredPriority ||
        settings.compatibility !=
            ManagerStartupTaskCompatibility::Windows10OrLater ||
        settings.disallowStartOnRemoteAppSession ||
        settings.useUnifiedSchedulingEngine ||
        settings.maintenanceSettingsPresent || settings.volatileTask) {
        return policyError(
            Domain::ErrorCodes::InvalidRequest,
            "The expected startup task settings do not match the canonical per-user Manager restart policy.");
    }

    return Domain::Result<void>::success();
}

Domain::Result<Domain::ManagerStartupStatus>
ManagerStartupTaskPolicy::classify(
    const ManagerStartupTaskDefinition& expected,
    const ManagerStartupTaskObservation& observed)
{
    auto validExpected = validateExpectedDefinition(expected);
    if (!validExpected) {
        return Domain::Result<Domain::ManagerStartupStatus>::failure(
            std::move(validExpected).error());
    }
    auto validObserved = validateObservedShape(observed, false);
    if (!validObserved) {
        return Domain::Result<Domain::ManagerStartupStatus>::failure(
            std::move(validObserved).error());
    }

    if (!observed.exists) {
        return validatedStatus({});
    }

    const auto& actual = *observed.definition;
    const bool owned = actual.ownership == expected.ownership;
    if (!owned) {
        return validatedStatus(Domain::ManagerStartupStatus{
            Domain::ManagerStartupState::ForeignConflict,
            true,
            observed.enabled,
            false,
            observed.running,
            observed.registrationIdentity,
            observed.lastResult,
            observed.lastRunAt});
    }
    validObserved = validateObservedShape(observed, true);
    if (!validObserved) {
        return Domain::Result<Domain::ManagerStartupStatus>::failure(
            std::move(validObserved).error());
    }
    const bool definitionMatches = actual == expected;
    Domain::ManagerStartupState state;
    if (!definitionMatches) {
        state = Domain::ManagerStartupState::Drifted;
    } else if (!observed.enabled) {
        state = Domain::ManagerStartupState::Disabled;
    } else {
        state = Domain::ManagerStartupState::Ready;
    }

    return validatedStatus(Domain::ManagerStartupStatus{
        state,
        true,
        observed.enabled,
        definitionMatches,
        observed.running,
        observed.registrationIdentity,
        observed.lastResult,
        observed.lastRunAt});
}

} // namespace ForgeConductor::Manager
